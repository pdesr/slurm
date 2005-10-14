/*****************************************************************************\
 * src/slurmd/io.c - I/O handling routines for slurmd
 * $Id$
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <mgrondona@llnl.gov>.
 *  UCRL-CODE-2002-040.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#if HAVE_UNISTD_H
#  include <unistd.h>
#endif

#if HAVE_STRING_H
#  include <string.h>
#endif

#if HAVE_STDLIB_H
#  include <stdlib.h>
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "src/common/eio.h"
#include "src/common/io_hdr.h"
#include "src/common/cbuf.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/fd.h"
#include "src/common/list.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"

#include "src/slurmd/shm.h"
#include "src/slurmd/io.h"
#include "src/slurmd/fname.h"
#include "src/slurmd/slurmd.h"

/**********************************************************************
 * IO client socket declarations
 **********************************************************************/
static bool _client_readable(eio_obj_t *);
static bool _client_writable(eio_obj_t *);
static int  _client_read(eio_obj_t *, List);
static int  _client_write(eio_obj_t *, List);

struct io_operations client_ops = {
        readable:	&_client_readable,
	writable:	&_client_writable,
	handle_read:	&_client_read,
	handle_write:	&_client_write,
};

struct client_io_info {
#ifndef NDEBUG
#define CLIENT_IO_MAGIC  0x10102
	int                   magic;
#endif
	slurmd_job_t    *job;		 /* pointer back to job data   */

	/* incoming variables */
	struct slurm_io_header header;
	struct io_buf *in_msg;
	int32_t in_remaining;
	bool in_eof;

	/* outgoing variables */
	List msg_queue;
	struct io_buf *out_msg;
	int32_t out_remaining;
	bool out_eof;
};

/**********************************************************************
 * Task write declarations
 **********************************************************************/
static bool _task_writable(eio_obj_t *);
static int  _task_write(eio_obj_t *, List);

struct io_operations task_write_ops = {
	writable:	&_task_writable,
	handle_write:	&_task_write,
};

struct task_write_info {
#ifndef NDEBUG
#define TASK_IN_MAGIC  0x10103
	int              magic;
#endif
	slurmd_job_t    *job;		 /* pointer back to job data   */

	List msg_queue;
	struct io_buf *msg;
	int32_t remaining;
};

/**********************************************************************
 * Task read declarations
 **********************************************************************/
static bool _task_readable(eio_obj_t *);
static int  _task_read(eio_obj_t *, List);

struct io_operations task_read_ops = {
        readable:	&_task_readable,
	handle_read:	&_task_read,
};

struct task_read_info {
#ifndef NDEBUG
#define TASK_OUT_MAGIC  0x10103
	int              magic;
#endif
	uint16_t         type;           /* type of IO object          */
	uint16_t         gtaskid;
	uint16_t         ltaskid;
	slurmd_job_t    *job;		 /* pointer back to job data   */
	cbuf_t           buf;
	bool		 eof;
	bool		 eof_msg_sent;
};

/**********************************************************************
 * General declarations
 **********************************************************************/
static void *_io_thr(void *);
static int _send_io_init_msg(int sock, srun_key_t *key, slurmd_job_t *job);
static void _send_eof_msg(struct task_read_info *out);
static struct io_buf *_task_build_message(struct task_read_info *out,
					  slurmd_job_t *job, cbuf_t cbuf);
static struct io_obj *_io_obj(slurmd_job_t *, slurmd_task_info_t *, int, int);
static void *_io_thr(void *arg);
static void _route_msg_task_to_client(eio_obj_t *obj);
static void _free_outgoing_msg(struct io_buf *msg, slurmd_job_t *job);
static void _free_incoming_msg(struct io_buf *msg, slurmd_job_t *job);
static void _free_all_outgoing_msgs(List msg_queue, slurmd_job_t *job);

/**********************************************************************
 * IO client socket functions
 **********************************************************************/
static bool 
_client_readable(eio_obj_t *obj)
{
	struct client_io_info *client = (struct client_io_info *) obj->arg;

	debug3("Called _client_readable");
	xassert(client->magic == CLIENT_IO_MAGIC);

	if (client->in_eof) {
		debug3("  false");
		return false;
	}

	if (obj->shutdown) {
		debug3("  false, shutdown");
		shutdown(obj->fd, SHUT_RD);
		client->in_eof = true;
	}

	if (client->in_msg != NULL
	    || !list_is_empty(client->job->free_incoming))
		return true;

	debug3("  false");
	return false;
}

static bool 
_client_writable(eio_obj_t *obj)
{
	struct client_io_info *client = (struct client_io_info *) obj->arg;

	debug3("Called _client_writable");
	xassert(client->magic == CLIENT_IO_MAGIC);

	if (client->out_eof == true) {
		debug3("  false, out_eof");
		return false;
	}

	/* If this is a newly attached client its msg_queue needs
	 * to be intialized from the outgoing_cache
	 */
	if (client->msg_queue == NULL) {
		ListIterator msgs;
		struct io_buf *msg;
		client->msg_queue = list_create(NULL); /* need destructor */
		msgs = list_iterator_create(client->job->outgoing_cache);
		while (msg = list_next(msgs)) {
			msg->ref_count++;
			list_enqueue(client->msg_queue, msg);
		}
		list_iterator_destroy(msgs);
	}

	if (client->out_msg != NULL)
		debug3("  client->out.msg != NULL");
	if (!list_is_empty(client->msg_queue))
		debug3("  client->out.msg_queue queue length = %d",
		       list_count(client->msg_queue));

	if (client->out_msg != NULL
	    || !list_is_empty(client->msg_queue))
		return true;

	debug3("  false");
	return false;
}

static int 
_client_read(eio_obj_t *obj, List objs)
{
	struct client_io_info *client = (struct client_io_info *) obj->arg;
	void *buf;
	int n;

	debug2("Entering _client_read");
	xassert(client->magic == CLIENT_IO_MAGIC);

	/*
	 * Read the header, if a message read is not already in progress
	 */
	if (client->in_msg == NULL) {
		client->in_msg = list_dequeue(client->job->free_incoming);
		if (client->in_msg == NULL) {
			debug3("  _client_read free_incoming is empty");
			return SLURM_SUCCESS;
		}
		n = io_hdr_read_fd(obj->fd, &client->header);
		if (n <= 0) { /* got eof or fatal error */
			debug3("  got eof or error _client_read header, n=%d", n);
			client->in_eof = true;
			list_enqueue(client->job->free_incoming, client->in_msg);
			client->in_msg = NULL;
			return SLURM_SUCCESS;
		}
		debug3("client->header.length = %d", client->header.length);
		if (client->header.length > MAX_MSG_LEN)
			fatal("Message length of %d exceeds maximum of %d",
			      client->header.length, MAX_MSG_LEN);
		client->in_remaining = client->header.length;
		client->in_msg->length = client->header.length;
	}

	/*
	 * Read the body
	 */
	if (client->header.length == 0) { /* zero length is an eof message */
		debug3("  got stdin eof message!");
	} else {
		buf = client->in_msg->data + (client->in_msg->length - client->in_remaining);
	again:
		if ((n = read(obj->fd, buf, client->in_remaining)) < 0) {
			if (errno == EINTR)
				goto again;
			/* FIXME handle error */
			return SLURM_ERROR;
		}
		if (n == 0) { /* got eof */
			debug3("  got eof on _client_read body");
			client->in_eof = true;
			list_enqueue(client->job->free_incoming, client->in_msg);
			client->in_msg = NULL;
			return SLURM_SUCCESS;
		}
		client->in_remaining -= n;
		if (client->in_remaining > 0)
			return SLURM_SUCCESS;
/* 		*(char *)(buf + n) = '\0'; */
/* 		debug3("\"%s\"", buf); */
	}

	/*
	 * Route the message to its destination(s)
	 */
	if (client->header.type != SLURM_IO_STDIN
	    && client->header.type != SLURM_IO_ALLSTDIN) {
		error("Input client->header.type is not valid!");
		client->in_msg = NULL;
		return SLURM_ERROR;
	} else {
		int i;
		slurmd_task_info_t *task;
		struct task_write_info *io;

		client->in_msg->ref_count = 0;
		if (client->header.type == SLURM_IO_ALLSTDIN) {
			for (i = 0; i < client->job->ntasks; i++) {
				task = client->job->task[i];
				io = (struct task_write_info *)task->in->arg;
				client->in_msg->ref_count++;
				list_enqueue(io->msg_queue, client->in_msg);
			}
			debug3("  message ref_count = %d", client->in_msg->ref_count);
		} else {
			for (i = 0; i < client->job->ntasks; i++) {
				task = client->job->task[i];
				if (task->in == NULL)
					continue;
				io = (struct task_write_info *)task->in->arg;
				if (task->gtid != client->header.gtaskid)
					continue;
				client->in_msg->ref_count++;
				list_enqueue(io->msg_queue, client->in_msg);
				break;
			}
		}
	}
	client->in_msg = NULL;
	debug2("Leaving  _client_read");
	return SLURM_SUCCESS;
}

/*
 * Write outgoing packed messages to the client socket.
 */
static int
_client_write(eio_obj_t *obj, List objs)
{
	struct client_io_info *client = (struct client_io_info *) obj->arg;
	void *buf;
	int n;

	xassert(client->magic == CLIENT_IO_MAGIC);

	debug2("Entering _client_write");

	/*
	 * If we aren't already in the middle of sending a message, get the
	 * next message from the queue.
	 */
	if (client->out_msg == NULL) {
		client->out_msg = list_dequeue(client->msg_queue);
		if (client->out_msg == NULL) {
			debug3("_client_write: nothing in the queue");
			return SLURM_SUCCESS;
		}
		debug3("  dequeue successful, client->out_msg->length = %d", client->out_msg->length);
		client->out_remaining = client->out_msg->length;
	}

	debug3("  client->out_remaining = %d", client->out_remaining); 

	/*
	 * Write message to socket.
	 */
	buf = client->out_msg->data + (client->out_msg->length - client->out_remaining);
	debug3("made it here");
again:
	if ((n = write(obj->fd, buf, client->out_remaining)) < 0) {
		debug3("made it here too");
		if (errno == EINTR)
			goto again;
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
			debug3("_client_write returned EAGAIN");
			return SLURM_SUCCESS;
		}
		if (errno == EPIPE) {
			client->out_eof = true;
			_free_all_outgoing_msgs(client->msg_queue, client->job);
			return SLURM_SUCCESS;
		}
		error("Get error on write() in _client_write: %m");
		return SLURM_SUCCESS;
	}
	debug3("Wrote %d bytes to socket", n);
	client->out_remaining -= n;
	if (client->out_remaining > 0)
		return SLURM_SUCCESS;

	_free_outgoing_msg(client->out_msg, client->job);
	client->out_msg = NULL;

	return SLURM_SUCCESS;
}

/**********************************************************************
 * Task write functions
 **********************************************************************/
/*
 * Create an eio_obj_t for handling a task's stdin traffic
 */
static eio_obj_t *
_create_task_in_eio(int fd, slurmd_job_t *job)
{
	struct task_write_info *t = NULL;
	eio_obj_t *eio = NULL;

	t = (struct task_write_info *)xmalloc(sizeof(struct task_write_info));
#ifndef NDEBUG
	t->magic = TASK_IN_MAGIC;
#endif
	t->job = job;
	t->msg_queue = list_create(NULL); /* FIXME! Add destructor */
	t->msg = NULL;
	t->remaining = 0;

	eio = eio_obj_create(fd, &task_write_ops, (void *)t);

	return eio;
}

static bool 
_task_writable(eio_obj_t *obj)
{
	struct task_write_info *t = (struct task_write_info *) obj->arg;

	debug3("Called _task_writable");

	if (t->msg != NULL || list_count(t->msg_queue) > 0) {
		debug3("  true, list_count = %d", list_count(t->msg_queue));
		return true;
	}

	debug3("  false (list_count = %d)", list_count(t->msg_queue));
	return false;
}

static int
_task_write(eio_obj_t *obj, List objs)
{
	struct task_write_info *in = (struct task_write_info *) obj->arg;
	void *buf;
	int n;

	debug2("Entering _task_write");
	xassert(in->magic == TASK_IN_MAGIC);

	/*
	 * If we aren't already in the middle of sending a message, get the
	 * next message from the queue.
	 */
	if (in->msg == NULL) {
		in->msg = list_dequeue(in->msg_queue);
		if (in->msg == NULL) {
			debug3("_task_write: nothing in the queue");
			return SLURM_SUCCESS;
		}
		if (in->msg->length == 0) { /* eof message */
			close(obj->fd);
			obj->fd = -1;
			_free_incoming_msg(in->msg, in->job);
			in->msg = NULL;
			return SLURM_SUCCESS;
		}
		in->remaining = in->msg->length;
	}

	/*
	 * Write message to socket.
	 */
	buf = in->msg->data + (in->msg->length - in->remaining);
again:
	if ((n = write(obj->fd, buf, in->remaining)) < 0) {
		if (errno == EINTR)
			goto again;
		/* FIXME handle error */
		return SLURM_ERROR;
	}
	in->remaining -= n;
	if (in->remaining > 0)
		return SLURM_SUCCESS;

	_free_incoming_msg(in->msg, in->job);
	in->msg = NULL;

	return SLURM_SUCCESS;
}

/**********************************************************************
 * Task read functions
 **********************************************************************/
/*
 * Create an eio_obj_t for handling a task's stdout or stderr traffic
 */
static eio_obj_t *
_create_task_out_eio(int fd, uint16_t type,
		     slurmd_job_t *job, slurmd_task_info_t *task)
{
	struct task_read_info *out = NULL;
	eio_obj_t *eio = NULL;

	out = (struct task_read_info *)xmalloc(sizeof(struct task_read_info));
#ifndef NDEBUG
	out->magic = TASK_OUT_MAGIC;
#endif
	out->type = type;
	out->gtaskid = task->gtid;
	out->ltaskid = task->id;
	out->job = job;
	out->buf = cbuf_create(MAX_MSG_LEN, MAX_MSG_LEN*4);
	out->eof = false;
	out->eof_msg_sent = false;
	if (cbuf_opt_set(out->buf, CBUF_OPT_OVERWRITE, CBUF_NO_DROP) == -1)
		error("setting cbuf options");

	eio = eio_obj_create(fd, &task_read_ops, (void *)out);

	return eio;
}

static bool 
_task_readable(eio_obj_t *obj)
{
	struct task_read_info *out = (struct task_read_info *)obj->arg;

	debug3("Called _task_readable, task %d, %s", out->gtaskid,
	       out->type == SLURM_IO_STDOUT ? "STDOUT" : "STDERR");

	if (out->eof_msg_sent) {
		debug3("  false, eof message sent");
		return false;
	}
	if (cbuf_free(out->buf) > 0) {
		debug3("  cbuf_free = %d", cbuf_free(out->buf));
		return true;
	}

	debug3("  false");
	return false;
}

/*
 * Read output (stdout or stderr) from a task into a cbuf.  The cbuf
 * allows whole lines to be packed into messages if line buffering
 * is requested.
 */
static int
_task_read(eio_obj_t *obj, List objs)
{
	struct task_read_info *out = (struct task_read_info *)obj->arg;
	struct client_io_info *client;
	struct io_buf *msg = NULL;
	eio_obj_t *eio;
	ListIterator clients;
	int len;
	int rc = -1;

	xassert(out->magic == TASK_OUT_MAGIC);

	debug2("Entering _task_read");
	len = cbuf_free(out->buf);
	if (len > 0) {
again:
		if ((rc = cbuf_write_from_fd(out->buf, obj->fd, len, NULL))
		    < 0) {
			if (errno == EINTR)
				goto again;
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				debug3("_task_read returned EAGAIN");
				return SLURM_SUCCESS;
			}
			/* FIXME add error message */
			debug3("  error in _task_read");
			return SLURM_ERROR;
		}
		if (rc == 0) {  /* got eof */
			debug3("  got eof on task");
			out->eof = true;
		}
	}

	debug3("************************ %d bytes read from task %s", rc,
	       out->type == SLURM_IO_STDOUT ? "STDOUT" : "STDERR");

	/*
	 * Put the message in client outgoing queues
	 */
	_route_msg_task_to_client(obj);

	/*
	 * Send the eof message
	 */
	if (cbuf_used(out->buf) == 0 && out->eof) {
		_send_eof_msg(out);
	}
	
	return SLURM_SUCCESS;
}

/**********************************************************************
 * General fuctions
 **********************************************************************/
static char * 
_local_filename (char *fname, int taskid)
{
	int id;

	if (fname == NULL)
		return NULL;

	if ((id = fname_single_task_io(fname)) < 0) 
		return fname;

	if (id != taskid)
		return "/dev/null";

	return (NULL);
}

static int
_init_task_stdio_fds(slurmd_task_info_t *task, slurmd_job_t *job)
{
	char *name;
	int single;
	int fd;
	struct passwd *spwd = NULL;

	/*
	 *  Initialize stdin
	 */
	if (task->ifname != NULL) {
		/* open file on task's stdin */
		debug3("  stdin file name = %s", task->ifname);
		if ((task->stdin_fd = open(task->ifname, O_RDONLY)) == -1) {
			error("Could not open stdin file: %m");
			return SLURM_ERROR;
		}
		task->to_stdin = -1;  /* not used */
	} else {
		/* create pipe and eio object */
		int pin[2];
		debug3("  stdin uses an eio object");
		if (pipe(pin) < 0) {
			error("stdin pipe: %m");
			return SLURM_ERROR;
		}
		task->stdin_fd = pin[0];
		task->to_stdin = pin[1];
		fd_set_close_on_exec(task->to_stdin);
		fd_set_nonblocking(task->to_stdin);
		task->in = _create_task_in_eio(task->to_stdin, job);
		list_append(job->objs, (void *)task->in);
	}
	
	/*
	 *  Initialize stdout
	 */
	if (task->ofname != NULL) {
		/* open file on task's stdout */
		debug3("  stdout file name = %s", task->ofname);
		task->stdout_fd = open(task->ofname,
				    O_CREAT|O_WRONLY|O_TRUNC|O_APPEND, 0666);
		if (task->stdout_fd == -1) {
			error("Could not open stdout file: %m");
			return SLURM_ERROR;
		}
		task->from_stdout == -1; /* not used */
	} else {
		/* create pipe and eio object */
		int pout[2];
		debug3("  stdout uses an eio object");
		if (pipe(pout) < 0) {
			error("stdout pipe: %m");
			return SLURM_ERROR;
		}
		task->stdout_fd = pout[1];
		task->from_stdout = pout[0];
		fd_set_close_on_exec(task->from_stdout);
		fd_set_nonblocking(task->from_stdout);
		task->out = _create_task_out_eio(task->from_stdout,
						 SLURM_IO_STDOUT, job, task);
		list_append(job->objs, (void *)task->out);
		list_append(job->stdout_eio_objs, (void *)task->out);
	}

	/*
	 *  Initialize stderr
	 */
	if (task->efname != NULL) {
		/* open file on task's stdout */
		debug3("  stderr file name = %s", task->efname);
		task->stderr_fd = open(task->efname,
				    O_CREAT|O_WRONLY|O_TRUNC|O_APPEND, 0666);
		if (task->stderr_fd == -1) {
			error("Could not open stderr file: %m");
			return SLURM_ERROR;
		}
		task->from_stderr == -1; /* not used */
	} else {
		/* create pipe and eio object */
		int perr[2];
		debug3("  stderr uses an eio object");
		if (pipe(perr) < 0) {
			error("stderr pipe: %m");
			return SLURM_ERROR;
		}
		task->stderr_fd = perr[1];
		task->from_stderr = perr[0];
		fd_set_close_on_exec(task->from_stderr);
		fd_set_nonblocking(task->from_stderr);
		task->err = _create_task_out_eio(task->from_stderr,
						 SLURM_IO_STDERR, job, task);
		list_append(job->objs, (void *)task->err);
		list_append(job->stderr_eio_objs, (void *)task->err);
	}
}

int
io_init_tasks_stdio(slurmd_job_t *job)
{
	int i;

	for (i = 0; i < job->ntasks; i++) {
		_init_task_stdio_fds(job->task[i], job);
	}
}

int
io_thread_start(slurmd_job_t *job) 
{
	pthread_attr_t attr;

	slurm_attr_init(&attr);

	if (pthread_create(&job->ioid, &attr, &_io_thr, (void *)job) != 0)
		fatal("pthread_create: %m");
	
	/*fatal_add_cleanup(&_fatal_cleanup, (void *) job);*/

	return 0;
}

static int
_xclose(int fd)
{
	int rc;
	do {
		rc = close(fd);
	} while (rc == -1 && errno == EINTR);
	return rc;
}

void
_shrink_msg_cache(List cache, slurmd_job_t *job)
{
	struct io_buf *msg;
	int over = 0;
	int count;
	int i;

	count = list_count(cache);
	if (count > STDIO_MAX_MSG_CACHE)
		over = count - STDIO_MAX_MSG_CACHE;

	for (i = 0; i < over; i++) {
		msg = list_dequeue(cache);
		/* FIXME - following call MIGHT lead to too much recursion */
		_free_outgoing_msg(msg, job);
	}
}

static void
_route_msg_task_to_client(eio_obj_t *obj)
{
	struct task_read_info *out = (struct task_read_info *)obj->arg;
	struct client_io_info *client;
	struct io_buf *msg = NULL;
	eio_obj_t *eio;
	ListIterator clients;

	/* Pack task output into messages for transfer to a client */
	while (cbuf_used(out->buf) > 0
	       && !list_is_empty(out->job->free_outgoing)) {
		debug3("cbuf_used = %d", cbuf_used(out->buf));
		msg = _task_build_message(out, out->job, out->buf);
		if (msg == NULL)
			return;

/* 		debug3("\"%s\"", msg->data + io_hdr_packed_size()); */

		/* Add message to the msg_queue of all clients */
		clients = list_iterator_create(out->job->clients);
		while(eio = list_next(clients)) {
			client = (struct client_io_info *)eio->arg;
			if (client->out_eof == true)
				continue;
			debug3("======================== Enqueued message");
			xassert(client->magic == CLIENT_IO_MAGIC);
			if (list_enqueue(client->msg_queue, msg))
				msg->ref_count++;
		}
		list_iterator_destroy(clients);

		/* Update the outgoing message cache */
		if (list_enqueue(out->job->outgoing_cache, msg)) {
			msg->ref_count++;
			_shrink_msg_cache(out->job->outgoing_cache, out->job);
		}
	}
}

static void
_free_incoming_msg(struct io_buf *msg, slurmd_job_t *job)
{
	int i;

	msg->ref_count--;
	if (msg->ref_count == 0) {
		/* Put the message back on the free List */
		list_enqueue(job->free_incoming, msg);

		/* Kick the event IO engine */
		eio_signal_wakeup(job->eio);
	}
}

static void
_free_outgoing_msg(struct io_buf *msg, slurmd_job_t *job)
{
	int i;

	msg->ref_count--;
	if (msg->ref_count == 0) {
		/* Put the message back on the free List */
		list_enqueue(job->free_outgoing, msg);

		/* Try packing messages from tasks' output cbufs */
		if (job->task == NULL)
			return;
		for (i = 0; i < job->ntasks; i++) {
			if (job->task[i]->err != NULL) {
				_route_msg_task_to_client(job->task[i]->err);
				if (list_is_empty(job->free_outgoing))
					break;
			}
			if (job->task[i]->out != NULL) {
				_route_msg_task_to_client(job->task[i]->out);
				if (list_is_empty(job->free_outgoing))
					break;
			}
		}
		/* Kick the event IO engine */
		eio_signal_wakeup(job->eio);
	}
}

static void
_free_all_outgoing_msgs(List msg_queue, slurmd_job_t *job)
{
	ListIterator msgs;
	struct io_buf *msg;

	msgs = list_iterator_create(msg_queue);
	while(msg = list_next(msgs)) {
		_free_outgoing_msg(msg, job);
	}
	list_iterator_destroy(msgs);
}

extern void
io_close_task_fds(slurmd_job_t *job)
{
	int i;

	for (i = 0; i < job->ntasks; i++) {
		close(job->task[i]->stdin_fd);
		close(job->task[i]->stdout_fd);
		close(job->task[i]->stderr_fd);
	}
}

void 
io_close_all(slurmd_job_t *job)
{
	int i;

#if 0
	for (i = 0; i < job->ntasks; i++)
		_io_finalize(job->task[i]);
#endif

	/* No more debug info will be received by client after this point
	 */
	debug("Closing debug channel");
	close(STDERR_FILENO);

	/* Signal IO thread to close appropriate 
	 * client connections
	 */
	eio_signal_wakeup(job->eio);
}

static void *
_io_thr(void *arg)
{
	slurmd_job_t *job = (slurmd_job_t *) arg;
	sigset_t set;

	/* A SIGHUP signal signals a reattach to the mgr thread.  We need
	 * to block SIGHUP from being delivered to this thread so the mgr
	 * thread will see the signal.
	 */
	sigemptyset(&set);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	debug("IO handler started pid=%lu", (unsigned long) getpid());
	eio_handle_mainloop(job->eio);
	debug("IO handler exited");
	return (void *)1;
}

/* 
 * Initiate a TCP connection back to a waiting client (e.g. srun).
 *
 * Create a new eio client object and wake up the eio engine so that
 * it can see the new object.
 */
int
io_client_connect(srun_info_t *srun, slurmd_job_t *job)
{
	int i;
	int sock = -1;
	struct client_io_info *client;
	eio_obj_t *obj;

	debug2 ("adding IO connection (logical node rank %d)", job->nodeid);

	if (srun->ioaddr.sin_addr.s_addr) {
		char         host[256];
		uint16_t     port;
		slurmd_get_addr(&srun->ioaddr, &port, host, sizeof(host));
		debug2("connecting IO back to %s:%d", host, ntohs(port));
	} 

	if ((sock = (int) slurm_open_stream(&srun->ioaddr)) < 0) {
		error("connect io: %m");
		/* XXX retry or silently fail? 
		 *     fail for now.
		 */
		return SLURM_ERROR;
	}

	fd_set_blocking(sock);  /* just in case... */

	_send_io_init_msg(sock, srun->key, job);

	debug3("  back from _send_io_init_msg");
	fd_set_nonblocking(sock);
	fd_set_close_on_exec(sock);

	/* Now set up the eio object */
	client = xmalloc(sizeof(struct client_io_info));
#ifndef NDEBUG
	client->magic = CLIENT_IO_MAGIC;
#endif
	client->job = job;
	client->msg_queue = NULL; /* initialized in _client_writable */

	obj = eio_obj_create(sock, &client_ops, (void *)client);
	list_append(job->clients, (void *)obj);
	list_append(job->objs, (void *)obj);

	debug3("Now handling %d IO Client object(s)", list_count(job->clients));

	/* kick IO thread */
	eio_signal_wakeup(job->eio);

	return SLURM_SUCCESS;
}

static int
_send_io_init_msg(int sock, srun_key_t *key, slurmd_job_t *job)
{
	struct slurm_io_init_msg msg;

	memcpy(msg.cred_signature, key->data, SLURM_CRED_SIGLEN);
	msg.nodeid = job->nodeid;
	msg.stdout_objs = list_count(job->stdout_eio_objs);
	msg.stderr_objs = list_count(job->stderr_eio_objs);
	
	if (io_init_msg_write_to_fd(sock, &msg) != SLURM_SUCCESS) {
		error("Couldn't sent slurm_io_init_msg");
		return SLURM_ERROR;
	}
	
		
	return SLURM_SUCCESS;
}

/*
 * dup the appropriate file descriptors onto the task's
 * stdin, stdout, and stderr.
 *
 * Close the server's end of the stdio pipes.
 */
int
io_dup_stdio(slurmd_task_info_t *t)
{
	if (dup2(t->stdin_fd, STDIN_FILENO  ) < 0) {
		error("dup2(stdin): %m");
		return SLURM_FAILURE;
	}

	if (dup2(t->stdout_fd, STDOUT_FILENO) < 0) {
		error("dup2(stdout): %m");
		return SLURM_FAILURE;
	}

	if (dup2(t->stderr_fd, STDERR_FILENO) < 0) {
		error("dup2(stderr): %m");
		return SLURM_FAILURE;
	}

	/* ignore errors on close */
	close(t->to_stdin );
	close(t->from_stdout);
	close(t->from_stderr);
	return SLURM_SUCCESS;
}

static void
_send_eof_msg(struct task_read_info *out)
{
	struct client_io_info *client;
	struct io_buf *msg = NULL;
	eio_obj_t *eio;
	ListIterator clients;
	struct slurm_io_header header;
	Buf packbuf;

	debug2("Entering _send_eof_msg");
	msg = list_dequeue(out->job->free_outgoing);
	if (msg == NULL) {
		debug3("  free_outgoing msg list empty, can't send eof_msg");
		return;
	}

	header.type = out->type;
	header.ltaskid = out->ltaskid;
	header.gtaskid = out->gtaskid;
	header.length = 0; /* eof */

	packbuf = create_buf(msg->data, io_hdr_packed_size());
	io_hdr_pack(&header, packbuf);
	msg->length = io_hdr_packed_size() + header.length;
	msg->ref_count = 0; /* make certain it is initialized */

	/* Add eof message to the msg_queue of all clients */
	clients = list_iterator_create(out->job->clients);
	while(eio = list_next(clients)) {
		client = (struct client_io_info *)eio->arg;
		debug3("======================== Enqueued message");
		xassert(client->magic == CLIENT_IO_MAGIC);
		if (list_enqueue(client->msg_queue, msg))
			msg->ref_count++;
	}
	list_iterator_destroy(clients);

	out->eof_msg_sent = true;
	debug2("Leaving  _send_eof_msg");
}


static struct io_buf *
_task_build_message(struct task_read_info *out, slurmd_job_t *job, cbuf_t cbuf)
{
	struct io_buf *msg;
	char *ptr;
	Buf packbuf;
	bool must_truncate = false;
	int avail;
	struct slurm_io_header header;
	int n;

	debug2("Entering _task_build_message");
	msg = list_dequeue(job->free_outgoing);
	if (msg == NULL)
		return NULL;
	ptr = msg->data + io_hdr_packed_size();

	if (job->buffered_stdio) {
		avail = cbuf_peek_line(cbuf, ptr, MAX_MSG_LEN, 1);
		if (avail >= MAX_MSG_LEN)
			must_truncate = true;
	}

	debug3("  buffered_stdio is %s", job->buffered_stdio ? "true" : "false");
	debug3("  must_truncate  is %s", must_truncate ? "true" : "false");

	if (must_truncate || !job->buffered_stdio) {
		n = cbuf_read(cbuf, ptr, MAX_MSG_LEN);
	} else {
		n = cbuf_read_line(cbuf, ptr, MAX_MSG_LEN, -1);
		if (n == 0) {
			debug3("  partial line in buffer, ignoring");
			debug2("Leaving  _task_build_message");
			list_enqueue(job->free_outgoing, msg);
			return NULL;
		}
	}

	header.type = out->type;
	header.ltaskid = out->ltaskid;
	header.gtaskid = out->gtaskid;
	header.length = n;

	debug3("  header.length = %d", n);
	packbuf = create_buf(msg->data, io_hdr_packed_size());
	io_hdr_pack(&header, packbuf);
	msg->length = io_hdr_packed_size() + header.length;
	msg->ref_count = 0; /* make certain it is initialized */

	/* free the Buf packbuf, but not the memory to which it points */
	packbuf->head = NULL;
	free_buf(packbuf);
	
	debug2("Leaving  _task_build_message");
	return msg;
}

struct io_buf *
alloc_io_buf(void)
{
	struct io_buf *buf;

	buf = (struct io_buf *)xmalloc(sizeof(struct io_buf));
	if (!buf)
		return NULL;
	buf->ref_count = 0;
	buf->length = 0;
	/* The following "+ 1" is just temporary so I can stick a \0 at
	   the end and do a printf of the data pointer */
	buf->data = xmalloc(MAX_MSG_LEN + io_hdr_packed_size() + 1);
	if (!buf->data) {
		xfree(buf);
		return NULL;
	}

	return buf;
}

void
free_io_buf(struct io_buf *buf)
{
	if (buf) {
		if (buf->data)
			xfree(buf->data);
		xfree(buf);
	}
}
