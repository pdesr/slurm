/*****************************************************************************
 *  gang.c - Gang scheduler functions.
 *****************************************************************************
 *  Copyright (C) 2008 Hewlett-Packard Development Company, L.P.
 *  Written by Chris Holmes
 *  LLNL-CODE-402394.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

/*
 * gang scheduler plugin for SLURM
 */

#include <pthread.h>
#include <unistd.h>

#include "./gang.h"
#include "slurm/slurm.h"
#include "src/common/bitstring.h"
#include "src/common/list.h"
#include "src/common/node_select.h"
#include "src/common/slurm_protocol_defs.h"
#include "src/common/xstring.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"

/* global timeslicer thread variables */
static bool thread_running = false;
static bool thread_shutdown = false;
static pthread_mutex_t thread_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t timeslicer_thread_id;

/* timeslicer flags and structures */
enum entity_type {
	GS_NODE,
	GS_SOCKET,
	GS_CORE,
	GS_CPU
};

enum gs_flags {
	GS_SUSPEND,
	GS_RESUME,
	GS_NO_PART,
	GS_SUCCESS,
	GS_ACTIVE,
	GS_NO_ACTIVE,
	GS_FILLER
};

struct gs_job {
	uint32_t job_id;
	uint16_t sig_state;
	uint16_t row_state;
	bitstr_t *resmap;
	uint16_t *alloc_cpus;
};

struct gs_part {
	char *part_name;
	uint16_t priority;
	uint32_t num_jobs;
	struct gs_job **job_list;
	uint32_t job_list_size;
	uint32_t num_shadows;
	struct gs_job **shadow;  /* see '"Shadow" Design' below */
	uint32_t shadow_size;
	uint32_t jobs_active;
	bitstr_t *active_resmap;
	uint16_t *active_cpus;
	uint16_t array_size;
	struct gs_part *next;
};

/******************************************
 *
 *       SUMMARY OF DATA MANAGEMENT
 *
 * For GS_NODE and GS_CPU:    bits in resmaps represent nodes
 * For GS_SOCKET and GS_CORE: bits in resmaps represent sockets
 * GS_NODE and GS_SOCKET ignore the CPU array
 * GS_CPU and GS_CORE use the CPU array to help resolve conflict
 *
 *         EVALUATION ALGORITHM
 *
 * For GS_NODE and GS_SOCKET: bits CANNOT conflict
 * For GS_CPUS and GS_CORE:  if bits conflict, make sure sum of CPUs per
 *                           resource don't exceed physical resource count
 *
 *
 * The j_ptr->alloc_cpus array is a collection of allocated values ONLY.
 * For every bit set in j_ptr->resmap, there is a corresponding element
 * (with an equal-to or less-than index value) in j_ptr->alloc_cpus. 
 *
 ******************************************
 *
 *        "Shadow" Design to support Preemption
 *
 * Jobs in higher priority partitions "cast shadows" on the active
 * rows of lower priority partitions. The effect is that jobs that
 * are "caught" in these shadows are preempted (suspended)
 * indefinitely until the "shadow" disappears. When constructing
 * the active row of a partition, any jobs in the 'shadow' array
 * are applied first.
 *
 ******************************************
 */


/* global variables */
static uint32_t timeslicer_seconds = 0;
static uint16_t gr_type = GS_NODE;
static uint16_t gs_fast_schedule = 0;
static struct gs_part *gs_part_list = NULL;
static uint32_t default_job_list_size = 64;
static uint32_t gs_resmap_size = 0;
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t gs_num_groups = 0;
static uint16_t *gs_cpus_per_res = NULL;
static uint32_t *gs_cpu_count_reps = NULL;

static struct gs_part **gs_part_sorted = NULL;
static uint32_t num_sorted_part = 0;

#define GS_CPU_ARRAY_INCREMENT 8

/* function declarations */
static void *_timeslicer_thread();


char *_print_flag(int flag) {
	switch (flag) {
	case GS_SUSPEND:   return "GS_SUSPEND";
	case GS_RESUME:    return "GS_RESUME";
	case GS_NO_PART:   return "GS_NO_PART";
	case GS_SUCCESS:   return "GS_SUCCESS";
	case GS_ACTIVE:    return "GS_ACTIVE";
	case GS_NO_ACTIVE: return "GS_NO_ACTIVE";
	case GS_FILLER:    return "GS_FILLER";
	default:           return "unknown";
	}
	return "unknown";
}


void _print_jobs(struct gs_part *p_ptr)
{
	int i;
	debug3("sched/gang:  part %s has %u jobs, %u shadows:",
		p_ptr->part_name, p_ptr->num_jobs, p_ptr->num_shadows);
	for (i = 0; i < p_ptr->num_shadows; i++) {
		debug3("sched/gang:   shadow job %u row_s %s, sig_s %s",
			p_ptr->shadow[i]->job_id,
			_print_flag(p_ptr->shadow[i]->row_state),
			_print_flag(p_ptr->shadow[i]->sig_state));
	}
	for (i = 0; i < p_ptr->num_jobs; i++) {
		debug3("sched/gang:   job %u row_s %s, sig_s %s",
			p_ptr->job_list[i]->job_id,
			_print_flag(p_ptr->job_list[i]->row_state),
			_print_flag(p_ptr->job_list[i]->sig_state));
	}
	if (p_ptr->active_resmap) {
		int s = bit_size(p_ptr->active_resmap);
		i = bit_set_count(p_ptr->active_resmap);
		debug3("sched/gang:  active resmap has %d of %d bits set", i, s);
	}
}

static uint16_t
_get_gr_type() {

	switch (slurmctld_conf.select_type_param) {
	case CR_CORE:
	case CR_CORE_MEMORY:
		return GS_CORE;
	case CR_CPU:
	case CR_CPU_MEMORY:
		return GS_CPU;
	case CR_SOCKET:
	case CR_SOCKET_MEMORY:
		return GS_SOCKET;
	}
	/* note that CR_MEMORY is node-level scheduling with
	 * memory management */
	return GS_NODE;
}

/* Return resource data for the given node */
static uint16_t
_compute_resources(int i, char socket_count)
{
	if (gr_type == GS_NODE)
		return 1;

	if (gr_type == GS_CPU) {
		if (socket_count)
			return 1;
		if (gs_fast_schedule)
			return node_record_table_ptr[i].config_ptr->cpus;
		return node_record_table_ptr[i].cpus;
	}
	
	if (socket_count || gr_type == GS_SOCKET) {
		if (gs_fast_schedule)
			return node_record_table_ptr[i].config_ptr->sockets;
		return node_record_table_ptr[i].sockets;
	}

	/* gr_type == GS_CORE */
	if (gs_fast_schedule)
		return node_record_table_ptr[i].config_ptr->cores;
	return node_record_table_ptr[i].cores;
}

/* For GS_CPU  the gs_phys_res_cnt is the total number of CPUs per node.
 * For GS_CORE the gs_phys_res_cnt is the total number of cores per socket per
 * node (currently no nodes are made with different core counts per socket) */
static void
_load_phys_res_cnt()
{
	int i, array_size = GS_CPU_ARRAY_INCREMENT;
	uint32_t adder;

	xfree(gs_cpus_per_res);
	xfree(gs_cpu_count_reps);
	gs_num_groups = 0;
	if (gr_type == GS_NODE || gr_type == GS_SOCKET)
		return;

	gs_cpus_per_res   = xmalloc(array_size * sizeof(uint16_t));
	gs_cpu_count_reps = xmalloc(array_size * sizeof(uint32_t));
	for (i = 0; i < node_record_count; i++) {
		uint16_t res = _compute_resources(i, 0);
		if (gs_cpus_per_res[gs_num_groups] == res) {
			adder = 1;
			if (gr_type == GS_CORE)
				adder = _compute_resources(i, 1);
			gs_cpu_count_reps[gs_num_groups] += adder;
			continue;
		}
		if (gs_cpus_per_res[gs_num_groups] != 0) {
			gs_num_groups++;
			if (gs_num_groups >= array_size) {
				array_size += GS_CPU_ARRAY_INCREMENT;
				xrealloc(gs_cpus_per_res,
					 array_size * sizeof(uint16_t));
				xrealloc(gs_cpu_count_reps,
					 array_size * sizeof(uint32_t));
			}
		}
		gs_cpus_per_res[gs_num_groups] = res;
		adder = 1;
		if (gr_type == GS_CORE)
			adder = _compute_resources(i, 1);
		gs_cpu_count_reps[gs_num_groups] = adder;
	}
	gs_num_groups++;
	for (i = 0; i < gs_num_groups; i++) {
		debug3("sched/gang: _load_phys_res_cnt: grp %d cpus %u reps %u",
			i, gs_cpus_per_res[i], gs_cpu_count_reps[i]);
	}
	return;
}

static uint16_t
_get_phys_res_cnt(int res_index)
{
	int i = 0;
	int pos = gs_cpu_count_reps[i++];
	while (res_index >= pos) {
		pos += gs_cpu_count_reps[i++];
	}
	return gs_cpus_per_res[i-1];
}


/* The gs_part_list is a single large array of gs_part entities.
 * To destroy it, step down the array and destroy the pieces of
 * each gs_part entity, and then delete the whole array.
 * To destroy a gs_part entity, you need to delete the name, the
 * list of jobs, the shadow list, and the active_resmap. Each
 * job has a resmap that must be deleted also.
 */
static void
_destroy_parts() {
	int i;
	struct gs_part *tmp, *ptr = gs_part_list;
	struct gs_job *j_ptr;

	while (ptr) {
		tmp = ptr;
		ptr = ptr->next;

		xfree(tmp->part_name);
		for (i = 0; i < tmp->num_jobs; i++) {
			j_ptr = tmp->job_list[i];
			if (j_ptr->resmap)
				bit_free(j_ptr->resmap);
			xfree(j_ptr->alloc_cpus);
			xfree(j_ptr);
		}
		xfree(tmp->shadow);
		if (tmp->active_resmap)
			bit_free(tmp->active_resmap);
		xfree(tmp->active_cpus);
		xfree(tmp->job_list);
	}
	xfree(gs_part_list);
}

/* Build the gs_part_list. The job_list will be created later,
 * once a job is added. */
static void
_build_parts() {
	ListIterator part_iterator;
	struct part_record *p_ptr;
	int i, num_parts;

	if (gs_part_list)
		_destroy_parts();

	/* reset the sorted list, since it's currently
	 * pointing to partitions we just destroyed */
	num_sorted_part = 0;

	num_parts = list_count(part_list);
	if (!num_parts)
		return;

	part_iterator = list_iterator_create(part_list);
	if (part_iterator == NULL)
		fatal ("memory allocation failure");

	gs_part_list = xmalloc(num_parts * sizeof(struct gs_part));
	i = 0;
	while ((p_ptr = (struct part_record *) list_next(part_iterator))) {
		gs_part_list[i].part_name = xstrdup(p_ptr->name);
		gs_part_list[i].priority = p_ptr->priority;
		/* everything else is already set to zero/NULL */
		gs_part_list[i].next = &(gs_part_list[i+1]);
		i++;
	}
	gs_part_list[--i].next = NULL;
	list_iterator_destroy(part_iterator);
}

/* Find the gs_part entity with the given name */
static struct gs_part *
_find_gs_part(char *name)
{
	struct gs_part *p_ptr = gs_part_list;
	for (; p_ptr; p_ptr = p_ptr->next) {
		if (strcmp(name, p_ptr->part_name) == 0)
			return p_ptr;
	}
	return NULL;
}

/* Find the job_list index of the given job_id in the given partition */
static int
_find_job_index(struct gs_part *p_ptr, uint32_t job_id) {
	int i;
	for (i = 0; i < p_ptr->num_jobs; i++) {
		if (p_ptr->job_list[i]->job_id == job_id)
			return i;
	}
	return -1;
}

/* Return 1 if job fits in this row, else return 0 */
static int
_can_cpus_fit(bitstr_t *setmap, struct gs_job *j_ptr, struct gs_part *p_ptr)
{
	int i, size, a = 0;
	uint16_t *p_cpus, *j_cpus;

	size = bit_size(setmap);
	p_cpus = p_ptr->active_cpus;
	j_cpus = j_ptr->alloc_cpus;

	if (!p_cpus || !j_cpus)
		return 0;

	for (i = 0; i < size; i++) {
		if (bit_test(setmap, i)) {
			if (p_cpus[i]+j_cpus[a] > _get_phys_res_cnt(i))
				return 0;
		}
		if (bit_test(j_ptr->resmap, i))
			a++;
	}
	return 1;
}


/* Return 1 if job fits in this row, else return 0 */
static int
_job_fits_in_active_row(struct gs_job *j_ptr, struct gs_part *p_ptr)
{
	int count;
	bitstr_t *tmpmap;

	if (p_ptr->active_resmap == NULL || p_ptr->jobs_active == 0)
		return 1;

	tmpmap = bit_copy(j_ptr->resmap);
	if (!tmpmap)
		fatal("sched/gang: memory allocation error");
	
	bit_and(tmpmap, p_ptr->active_resmap);
	/* any set bits indicate contention for the same resource */
	count = bit_set_count(tmpmap);
	debug3("sched/gang: _job_fits_in_active_row: %d bits conflict", count);

	if (count == 0) {
		bit_free(tmpmap);
		return 1;
	}
	if (gr_type == GS_NODE || gr_type == GS_SOCKET) {
		bit_free(tmpmap);
		return 0;
	}

	/* for GS_CPU and GS_CORE, we need to compare CPU arrays and
	 * see if the sum of CPUs on any one resource exceed the total
	 * of physical resources available */
	count = _can_cpus_fit(tmpmap, j_ptr, p_ptr);
	bit_free(tmpmap);
	return count;
}

/* Add the given job to the "active" structures of
 * the given partition and increment the run count */
static void
_add_job_to_active(struct gs_job *j_ptr, struct gs_part *p_ptr)
{
	int i, a, sz;

	/* add job to active_resmap */
	if (!p_ptr->active_resmap) {
		/* allocate the active resmap */
		debug3("sched/gang: _add_job_to_active: using job %u as active base",
			j_ptr->job_id);
		p_ptr->active_resmap = bit_copy(j_ptr->resmap);
	} else if (p_ptr->jobs_active == 0) {
		/* if the active_resmap exists but jobs_active is '0',
		 * this means to overwrite the bitmap memory */
		debug3("sched/gang: _add_job_to_active: copying job %u into active base",
			j_ptr->job_id);
		bit_copybits(p_ptr->active_resmap, j_ptr->resmap);
	} else {
		/* add job to existing jobs in the active resmap */
		debug3("sched/gang: _add_job_to_active: merging job %u into active resmap",
			j_ptr->job_id);
		bit_or(p_ptr->active_resmap, j_ptr->resmap);
	}
	
	/* add job to the active_cpus array */
	if (gr_type == GS_CPU || gr_type == GS_CORE) {
		sz = bit_size(p_ptr->active_resmap);
		if (!p_ptr->active_cpus) {
			/* create active_cpus array */
			p_ptr->active_cpus = xmalloc(sz * sizeof(uint16_t));
		}
		if (p_ptr->jobs_active == 0) {
			/* overwrite the existing values in active_cpus */
			a = 0;
			for (i = 0; i < sz; i++) {
				if (bit_test(j_ptr->resmap, i)) {
					p_ptr->active_cpus[i] =
						j_ptr->alloc_cpus[a++];
				} else {
					p_ptr->active_cpus[i] = 0;
				}
			}
		} else {
			/* add job to existing jobs in the active cpus */
			a = 0;
			for (i = 0; i < sz; i++) {
				if (bit_test(j_ptr->resmap, i)) {
					uint16_t limit = _get_phys_res_cnt(i);
					p_ptr->active_cpus[i] +=
						j_ptr->alloc_cpus[a++];
					/* when adding shadows, the resources
					 * may get overcommitted */
					if (p_ptr->active_cpus[i] > limit)
						p_ptr->active_cpus[i] = limit;
				}
			}
		}
	}
	p_ptr->jobs_active += 1;
}

static void
_signal_job(uint32_t job_id, int sig)
{
	int rc;
	suspend_msg_t msg;
	
	msg.job_id = job_id;
	if (sig == GS_SUSPEND) {
		debug3("sched/gang: suspending %u", job_id);
		msg.op = SUSPEND_JOB;
	} else {
		debug3("sched/gang: resuming %u", job_id);
		msg.op = RESUME_JOB;
	}
	rc = job_suspend(&msg, 0, -1);
	if (rc)
		error("sched/gang: error (%d) signaling(%d) job %u", rc, sig,
		      job_id);
}

static uint32_t
_get_resmap_size()
{
	int i;
	uint32_t count = 0;
	/* if GS_NODE or GS_CPU, then size is the number of nodes */
	if (gr_type == GS_NODE || gr_type == GS_CPU)
		return node_record_count;
	/* else the size is the total number of sockets on all nodes */
	for (i = 0; i < node_record_count; i++) {
		count += _compute_resources(i, 1);
	}
	return count;
}

/* Load the gs_job struct with the correct
 * resmap and CPU array information
 */
static void
_load_alloc_cpus(struct gs_job *j_ptr, bitstr_t *nodemap)
{
	int i, a, alloc_index, sz;

	xfree(j_ptr->alloc_cpus);
	sz = bit_set_count(j_ptr->resmap);
	j_ptr->alloc_cpus = xmalloc(sz * sizeof(uint16_t));

	a = 0;
	alloc_index = 0;
	for (i = 0; i < node_record_count; i++) {
		uint16_t j, cores, sockets = _compute_resources(i, 1);
		
		if (bit_test(nodemap, i)) {
			for (j = 0; j < sockets; j++) {
				cores = select_g_get_job_cores(j_ptr->job_id,
								alloc_index,
								j);
				if (cores > 0)
					j_ptr->alloc_cpus[a++] = cores;
			}
			alloc_index++;
		}
	}
}

/* return an appropriate resmap given the granularity (GS_NODE/GS_CORE/etc.) */
/* This code fails if the bitmap size has changed. */
static bitstr_t *
_get_resmap(bitstr_t *origmap, uint32_t job_id)
{
	int i, alloc_index = 0, map_index = 0;
	bitstr_t *newmap;
	
	if (bit_size(origmap) != node_record_count) {
		error("sched/gang: bitmap size has changed from %d for %u",
			node_record_count, job_id);
		fatal("sched/gang: inconsistent bitmap size error");
	}
	if (gr_type == GS_NODE || gr_type == GS_CPU) {
		newmap = bit_copy(origmap);
		return newmap;
	}
	
	/* for GS_SOCKET and GS_CORE the resmap represents sockets */
	newmap = bit_alloc(gs_resmap_size);
	if (!newmap) {
		fatal("sched/gang: memory error creating newmap");
	}
	for (i = 0; i < node_record_count; i++) {
		uint16_t j, cores, sockets = _compute_resources(i, 1);
		
		if (bit_test(origmap, i)) {
			for (j = 0; j < sockets; j++) {
				cores = select_g_get_job_cores(job_id,
								alloc_index,
								j);
				if (cores > 0)
					bit_set(newmap, map_index);
				map_index++;
			}
			alloc_index++;
		} else {
			/* no cores allocated on this node */
			map_index += sockets;
		}
	}
	return newmap;
}

/* construct gs_part_sorted as a sorted list of the current partitions */
static void
_sort_partitions()
{
	struct gs_part *p_ptr;
	int i, j, size = 0;

	/* sort all partitions by priority */
	for (p_ptr = gs_part_list; p_ptr; p_ptr = p_ptr->next, size++);

	/* sorted array is new, or number of partitions has changed */
	if (size != num_sorted_part) {
		xfree(gs_part_sorted);
		gs_part_sorted = xmalloc(size * sizeof(struct gs_part *));
		num_sorted_part = size;
		/* load the array */
		i = 0;
		for (p_ptr = gs_part_list; p_ptr; p_ptr = p_ptr->next)
			gs_part_sorted[i++] = p_ptr;
	}

	if (size <= 1) {
		gs_part_sorted[0] = gs_part_list;
		return;
	}

	/* sort array (new array or priorities may have changed) */
	for (j = 0; j < size; j++) {
		for (i = j+1; i < size; i++) {
			if (gs_part_sorted[i]->priority >
				gs_part_sorted[j]->priority) {
				struct gs_part *tmp_ptr;
				tmp_ptr = gs_part_sorted[j];
				gs_part_sorted[j] = gs_part_sorted[i];
				gs_part_sorted[i] = tmp_ptr;
			}
		}
	}
}

/* Scan the partition list. Add the given job as a "shadow" to every
 * partition with a lower priority than the given partition */
static void
_cast_shadow(struct gs_job *j_ptr, uint16_t priority)
{
	struct gs_part *p_ptr;
	int i;
	
	for (p_ptr = gs_part_list; p_ptr; p_ptr = p_ptr->next) {
		if (p_ptr->priority >= priority)
			continue;
		
		/* This partition has a lower priority, so add
		 * the job as a "Shadow" */
		if (!p_ptr->shadow) {
			p_ptr->shadow_size = default_job_list_size;
			p_ptr->shadow = xmalloc(p_ptr->shadow_size *
						sizeof(struct gs_job *));
			/* 'shadow' is initialized to be NULL filled */
		} else {
			/* does this shadow already exist? */
			for (i = 0; i < p_ptr->num_shadows; i++) {
				if (p_ptr->shadow[i] == j_ptr)
					break;
			}
			if (i < p_ptr->num_shadows)
				continue;
		}
		
		if (p_ptr->num_shadows+1 >= p_ptr->shadow_size) {
			p_ptr->shadow_size *= 2;
			xrealloc(p_ptr->shadow, p_ptr->shadow_size *
						sizeof(struct gs_job *));
		}
		p_ptr->shadow[p_ptr->num_shadows++] = j_ptr;
	}
}

/* Remove the given job as a "shadow" from all partitions */
static void
_clear_shadow(struct gs_job *j_ptr)
{
	struct gs_part *p_ptr;
	int i;
	
	for (p_ptr = gs_part_list; p_ptr; p_ptr = p_ptr->next) {

		if (!p_ptr->shadow)
			continue;

		for (i = 0; i < p_ptr->num_shadows; i++) {
			if (p_ptr->shadow[i] == j_ptr)
				break;
		}
		if (i >= p_ptr->num_shadows)
			/* job not found */
			continue;

		p_ptr->num_shadows--;

		/* shift all other jobs down */
		for (; i < p_ptr->num_shadows; i++)
			p_ptr->shadow[i] = p_ptr->shadow[i+1];
		p_ptr->shadow[p_ptr->num_shadows] = NULL;
	}
}

/* Rebuild the active row BUT preserve the order of existing jobs.
 * This is called after one or more jobs have been removed from
 * the partition or if a higher priority "shadow" has been added
 * which could preempt running jobs.
 */
static void
_update_active_row(struct gs_part *p_ptr, int add_new_jobs)
{
	int i;
	struct gs_job *j_ptr;

	/* rebuild the active row, starting with any shadows */
	p_ptr->jobs_active = 0;
	for (i = 0; p_ptr->shadow && p_ptr->shadow[i]; i++) {
		_add_job_to_active(p_ptr->shadow[i], p_ptr);
	}
	
	/* attempt to add the existing 'active' jobs */
	for (i = 0; i < p_ptr->num_jobs; i++) {
		j_ptr = p_ptr->job_list[i];
		if (j_ptr->row_state != GS_ACTIVE)
			continue;
		if (_job_fits_in_active_row(j_ptr, p_ptr)) {
			_add_job_to_active(j_ptr, p_ptr);
			_cast_shadow(j_ptr, p_ptr->priority);
			
		} else {
			/* this job has been preempted by a shadow job.
			 * suspend it and preserve it's job_list order */
			if (j_ptr->sig_state != GS_SUSPEND) {
				_signal_job(j_ptr->job_id, GS_SUSPEND);
				j_ptr->sig_state = GS_SUSPEND;
				_clear_shadow(j_ptr);
			}
			j_ptr->row_state = GS_NO_ACTIVE;
		}
	}
	/* attempt to add the existing 'filler' jobs */
	for (i = 0; i < p_ptr->num_jobs; i++) {
		j_ptr = p_ptr->job_list[i];
		if (j_ptr->row_state != GS_FILLER)
			continue;
		if (_job_fits_in_active_row(j_ptr, p_ptr)) {
			_add_job_to_active(j_ptr, p_ptr);
			_cast_shadow(j_ptr, p_ptr->priority);
		} else {
			/* this job has been preempted by a shadow job.
			 * suspend it and preserve it's job_list order */
			if (j_ptr->sig_state != GS_SUSPEND) {
				_signal_job(j_ptr->job_id, GS_SUSPEND);
				j_ptr->sig_state = GS_SUSPEND;
				_clear_shadow(j_ptr);
			}
			j_ptr->row_state = GS_NO_ACTIVE;
		}
	}

	if (!add_new_jobs)
		return;

	/* attempt to add any new jobs */
	for (i = 0; i < p_ptr->num_jobs; i++) {
		j_ptr = p_ptr->job_list[i];
		if (j_ptr->row_state != GS_NO_ACTIVE)
			continue;
		if (_job_fits_in_active_row(j_ptr, p_ptr)) {
			_add_job_to_active(j_ptr, p_ptr);
			_cast_shadow(j_ptr, p_ptr->priority);
			/* note that this job is a "filler" for this row */
			j_ptr->row_state = GS_FILLER;
			/* resume the job */
			if (j_ptr->sig_state == GS_SUSPEND) {
				_signal_job(j_ptr->job_id, GS_RESUME);
				j_ptr->sig_state = GS_RESUME;
			}
		}
	}
}

/* rebuild all active rows without reordering jobs:
 * - attempt to preserve running jobs
 * - suspend any jobs that have been "shadowed" (preempted)
 * - resume any "filler" jobs that can be found
 */
static void
_update_all_active_rows()
{
	int i;
	
	/* Sort the partitions. This way the shadows of any high-priority
	 * jobs are appropriately adjusted before the lower priority
	 * partitions are updated */
	_sort_partitions();
	
	for (i = 0; i < num_sorted_part; i++) {
		_update_active_row(gs_part_sorted[i], 1);
	}
}

/* remove the given job from the given partition */
static void
_remove_job_from_part(uint32_t job_id, struct gs_part *p_ptr)
{
	int i;
	struct gs_job *j_ptr;

	if (!job_id || !p_ptr)
		return;

	debug3("sched/gang: _remove_job_from_part: removing job %u", job_id);
	/* find the job in the job_list */
	i = _find_job_index(p_ptr, job_id);
	if (i < 0)
		/* job not found */
		return;

	j_ptr = p_ptr->job_list[i];
	
	/* remove any shadow first */
	_clear_shadow(j_ptr);
	
	/* remove the job from the job_list by shifting everyone else down */
	p_ptr->num_jobs -= 1;
	for (; i < p_ptr->num_jobs; i++) {
		p_ptr->job_list[i] = p_ptr->job_list[i+1];
	}
	p_ptr->job_list[i] = NULL;
	
	/* make sure the job is not suspended, and then delete it */
	if (j_ptr->sig_state == GS_SUSPEND) {
		debug3("sched/gang: _remove_job_from_part: resuming suspended job %u",
			j_ptr->job_id);
		_signal_job(j_ptr->job_id, GS_RESUME);
	}
	bit_free(j_ptr->resmap);
	j_ptr->resmap = NULL;
	if (j_ptr->alloc_cpus)
		xfree(j_ptr->alloc_cpus);
	j_ptr->alloc_cpus = NULL;
	xfree(j_ptr);
	
	return;
}

/* Add the given job to the given partition, and if it remains running
 * then "cast it's shadow" over the active row of any partition with a
 * lower priority than the given partition. Return the sig state of the
 * job (GS_SUSPEND or GS_RESUME) */
static uint16_t
_add_job_to_part(struct gs_part *p_ptr, uint32_t job_id, bitstr_t *job_bitmap)
{
	int i;
	struct gs_job *j_ptr;

	xassert(p_ptr);
	xassert(job_id > 0);
	xassert(job_bitmap);

	debug3("sched/gang: _add_job_to_part: adding job %u", job_id);
	_print_jobs(p_ptr);
	
	/* take care of any memory needs */
	if (!p_ptr->job_list) {
		p_ptr->job_list_size = default_job_list_size;
		p_ptr->job_list = xmalloc(p_ptr->job_list_size *
						sizeof(struct gs_job *));
		/* job_list is initialized to be NULL filled */
	}
	
	/* protect against duplicates */
	i = _find_job_index(p_ptr, job_id);
	if (i >= 0) {
		/* This job already exists, but the resource allocation
		 * may have changed. In any case, remove the existing
		 * job before adding this new one.
		 */
		debug3("sched/gang: _add_job_to_part: duplicate job %u detected",
			job_id);
		_remove_job_from_part(job_id, p_ptr);
		_update_active_row(p_ptr, 0);
	}
	
	/* more memory management */
	if (p_ptr->num_jobs+1 == p_ptr->job_list_size) {
		p_ptr->job_list_size *= 2;
		xrealloc(p_ptr->job_list, p_ptr->job_list_size *
						sizeof(struct gs_job *));
		for (i = p_ptr->num_jobs+1; i < p_ptr->job_list_size; i++)
			p_ptr->job_list[i] = NULL;
	}
	j_ptr = xmalloc(sizeof(struct gs_job));
	
	/* gather job info */
	j_ptr->job_id    = job_id;
	j_ptr->sig_state = GS_RESUME;  /* all jobs are running initially */
	j_ptr->row_state = GS_NO_ACTIVE; /* job is not in the active row */
	j_ptr->resmap    = _get_resmap(job_bitmap, job_id);
	j_ptr->alloc_cpus = NULL;
	if (gr_type == GS_CORE || gr_type == GS_CPU) {
		_load_alloc_cpus(j_ptr, job_bitmap);
	}

	/* append this job to the job_list */
	p_ptr->job_list[p_ptr->num_jobs++] = j_ptr;
	
	/* determine the immediate fate of this job (run or suspend) */
	if (_job_fits_in_active_row(j_ptr, p_ptr)) {
		debug3("sched/gang: _add_job_to_part: adding job %u to active row", 
			job_id);
		_add_job_to_active(j_ptr, p_ptr);
		/* note that this job is a "filler" for this row */
		j_ptr->row_state = GS_FILLER;
		/* all jobs begin in the run state, so
		 * there's no need to signal this job */

		/* since this job is running we need to "cast it's shadow"
		 * over lower priority partitions */
		_cast_shadow(j_ptr, p_ptr->priority);

	} else {
		debug3("sched/gang: _add_job_to_part: suspending job %u",
			job_id);
		_signal_job(j_ptr->job_id, GS_SUSPEND);
		j_ptr->sig_state = GS_SUSPEND;
	}
	
	_print_jobs(p_ptr);
	
	return j_ptr->sig_state;
}

/* ensure that all jobs running in SLURM are accounted for.
 * this procedure assumes that the gs data has already been
 * locked by the caller! 
 */
static void
_scan_slurm_job_list()
{
	struct job_record *job_ptr;
	struct gs_part *p_ptr;
	int i;
	ListIterator job_iterator;

	if (!job_list) {	/* no jobs */
		return;
	}
	debug3("sched/gang: _scan_slurm_job_list: job_list exists...");
	job_iterator = list_iterator_create(job_list);
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		debug3("sched/gang: _scan_slurm_job_list: checking job %u",
			job_ptr->job_id);		
		if (job_ptr->job_state == JOB_PENDING)
			continue;
		if (job_ptr->job_state == JOB_SUSPENDED ||
		    job_ptr->job_state == JOB_RUNNING) {
			/* are we tracking this job already? */
			p_ptr = _find_gs_part(job_ptr->partition);
			if (!p_ptr) /* no partition */
				continue;
			i = _find_job_index(p_ptr, job_ptr->job_id);
			if (i >= 0)
				/* we're tracking it, so continue */
				continue;
			
			/* We're not tracking this job. Resume it if it's
			 * suspended, and then add it to the job list. */
			
			if (job_ptr->job_state == JOB_SUSPENDED)
			/* The likely scenario here is that the slurmctld has
			 * failed over, and this is a job that the sched/gang
			 * plugin had previously suspended.
			 * It's not possible to determine the previous order
			 * of jobs without preserving sched/gang state, which
			 * is not worth the extra infrastructure. Just resume
			 * the job and then add it to the job list.
			 */
				_signal_job(job_ptr->job_id, GS_RESUME);
			
			_add_job_to_part(p_ptr, job_ptr->job_id,
					 job_ptr->node_bitmap);
			continue;
		}
		
		/* if the job is not pending, suspended, or running, then
		   it's completing or completed. Make sure we've released
		   this job */		
		p_ptr = _find_gs_part(job_ptr->partition);
		if (!p_ptr) /* no partition */
			continue;
		_remove_job_from_part(job_ptr->job_id, p_ptr);
	}
	list_iterator_destroy(job_iterator);

	/* now that all of the old jobs have been flushed out,
	 * update the active row of all partitions */
	_update_all_active_rows();

	return;
}


/****************************
 * SLURM Timeslicer Hooks
 *
 * Here is a summary of the primary activities that occur
 * within this plugin:
 *
 * gs_init: initialize plugin
 *
 * gs_job_start: a new allocation has been created
 * gs_job_scan: synchronize with master job list
 * gs_job_fini: an existing allocation has been cleared
 * gs_reconfig: refresh partition and job data
 * _cycle_job_list: timeslicer thread is rotating jobs
 *
 * gs_fini: terminate plugin
 *
 ***************************/

static void
_spawn_timeslicer_thread()
{
	pthread_attr_t thread_attr_msg;

	pthread_mutex_lock( &thread_flag_mutex );
	if (thread_running) {
		error("timeslicer thread already running, not starting another");
		pthread_mutex_unlock(&thread_flag_mutex);
		return;
	}

	slurm_attr_init(&thread_attr_msg);
	if (pthread_create(&timeslicer_thread_id, &thread_attr_msg, 
			_timeslicer_thread, NULL))
		fatal("pthread_create %m");

	slurm_attr_destroy(&thread_attr_msg);
	thread_running = true;
	pthread_mutex_unlock(&thread_flag_mutex);
}

extern int
gs_init()
{
	/* initialize global variables */
	debug3("sched/gang: entering gs_init");
	timeslicer_seconds = slurmctld_conf.sched_time_slice;
	gs_fast_schedule = slurm_get_fast_schedule();
	gr_type = _get_gr_type();
	gs_resmap_size = _get_resmap_size();

	/* load the physical resource count data */
	_load_phys_res_cnt();

	pthread_mutex_lock(&data_mutex);
	_build_parts();
	/* load any currently running jobs */
	_scan_slurm_job_list();
	pthread_mutex_unlock(&data_mutex);

	/* spawn the timeslicer thread */
	_spawn_timeslicer_thread();
	debug3("sched/gang: leaving gs_init");
	return SLURM_SUCCESS;
}

extern int
gs_fini()
{
	/* terminate the timeslicer thread */
	debug3("sched/gang: entering gs_fini");
	pthread_mutex_lock(&thread_flag_mutex);
	if (thread_running) {
		int i;
		thread_shutdown = true;
		for (i=0; i<4; i++) {
			if (pthread_cancel(timeslicer_thread_id)) {
				timeslicer_thread_id = 0;
				break;
			}
			usleep(1000);
		}
		if (timeslicer_thread_id)
			error("sched/gang: Cound not kill timeslicer pthread");
	}
	pthread_mutex_unlock(&thread_flag_mutex);
	
	pthread_mutex_lock(&data_mutex);
	_destroy_parts();
	xfree(gs_part_sorted);
	gs_part_sorted = NULL;
	xfree(gs_cpus_per_res);
	xfree(gs_cpu_count_reps);
	gs_num_groups = 0;
	pthread_mutex_unlock(&data_mutex);
	debug3("sched/gang: leaving gs_fini");

	return SLURM_SUCCESS;
}

extern int
gs_job_start(struct job_record *job_ptr)
{
	struct gs_part *p_ptr;
	uint16_t job_state;

	debug3("sched/gang: entering gs_job_start");
	/* add job to partition */
	pthread_mutex_lock(&data_mutex);
	p_ptr = _find_gs_part(job_ptr->partition);
	if (p_ptr) {
		job_state = _add_job_to_part(p_ptr, job_ptr->job_id,
						job_ptr->node_bitmap);
		/* if this job is running then check for preemption */
		if (job_state == GS_RESUME)
			_update_all_active_rows();
	}
	pthread_mutex_unlock(&data_mutex);

	if (!p_ptr) {
		/* No partition was found for this job, so let it run
		 * uninterupted (what else can we do?)
		 */
		error("sched_gang: could not find partition %s for job %u",
		      job_ptr->partition, job_ptr->job_id);
	}
	debug3("sched/gang: leaving gs_job_start");
	return SLURM_SUCCESS;
}

extern int
gs_job_scan(void)
{
	/* scan the master SLURM job list for any new
	 * jobs to add, or for any old jobs to remove
	 */
	debug3("sched/gang: entering gs_job_scan");
	pthread_mutex_lock(&data_mutex);
	_scan_slurm_job_list();
	pthread_mutex_unlock(&data_mutex);
	debug3("sched/gang: leaving gs_job_scan");

	return SLURM_SUCCESS;
}

extern int
gs_job_fini(struct job_record *job_ptr)
{
	struct gs_part *p_ptr;
	
	debug3("sched/gang: entering gs_job_fini");
	pthread_mutex_lock(&data_mutex);
	p_ptr = _find_gs_part(job_ptr->partition);
	if (!p_ptr) {
		pthread_mutex_unlock(&data_mutex);
		debug3("sched/gang: leaving gs_job_fini");
		return SLURM_SUCCESS;
	}

	/* remove job from the partition */
	_remove_job_from_part(job_ptr->job_id, p_ptr);
	/* this job may have preempted other jobs, so
	 * check by updating all active rows */
	_update_all_active_rows();
	pthread_mutex_unlock(&data_mutex);
	debug3("sched/gang: leaving gs_job_fini");
	
	return SLURM_SUCCESS;
}

/* rebuild from scratch */
/* A reconfigure can affect this plugin in these ways:
 * - partitions can be added or removed
 *   - this affects the gs_part_list
 * - nodes can be removed from a partition, or added to a partition
 *   - this affects the size of the active resmap
 *
 * If nodes have been added or removed, then the node_record_count
 * will be different from gs_resmap_size. In this case, we need
 * to resize the existing resmaps to prevent errors when comparing
 * them.
 *
 * Here's the plan:
 * 1. save a copy of the global structures, and then construct
 *    new ones.
 * 2. load the new partition structures with existing jobs, 
 *    confirming the job exists and resizing their resmaps
 *    (if necessary).
 * 3. make sure all partitions are accounted for. If a partition
 *    was removed, make sure any jobs that were in the queue and
 *    that were suspended are resumed. Conversely, if a partition
 *    was added, check for existing jobs that may be contending
 *    for resources that we could begin timeslicing.
 * 4. delete the old global structures and return.
 */
extern int
gs_reconfig()
{
	int i;
	struct gs_part *p_ptr, *old_part_list, *newp_ptr;
	struct job_record *job_ptr;

	debug3("sched/gang: entering gs_reconfig");
	pthread_mutex_lock(&data_mutex);

	old_part_list = gs_part_list;
	gs_part_list = NULL;
	_build_parts();
	
	/* scan the old part list and add existing jobs to the new list */
	for (p_ptr = old_part_list; p_ptr; p_ptr = p_ptr->next) {
		newp_ptr = _find_gs_part(p_ptr->part_name);
		if (!newp_ptr) {
			/* this partition was removed, so resume
			 * any suspended jobs and continue */
			for (i = 0; i < p_ptr->num_jobs; i++) {
				if (p_ptr->job_list[i]->sig_state == GS_SUSPEND) {
					_signal_job(p_ptr->job_list[i]->job_id,
						   GS_RESUME);
					p_ptr->job_list[i]->sig_state = GS_RESUME;
				}	
			}
			continue;
		}
		if (p_ptr->num_jobs == 0)
			/* no jobs to transfer */
			continue;
		/* we need to transfer the jobs from p_ptr to new_ptr and
		 * adjust their resmaps (if necessary). then we need to create
		 * the active resmap and adjust the state of each job (if
		 * necessary). NOTE: there could be jobs that only overlap
		 * on nodes that are no longer in the partition, but we're
		 * not going to worry about those cases.
		 */
		/* add the jobs from p_ptr into new_ptr in their current order
		 * to preserve the state of timeslicing.
		 */
		for (i = 0; i < p_ptr->num_jobs; i++) {
			job_ptr = find_job_record(p_ptr->job_list[i]->job_id);
			if (job_ptr == NULL) {
				/* job no longer exists in SLURM, so drop it */
				continue;
			}
			/* resume any job that is suspended */
			if (job_ptr->job_state == JOB_SUSPENDED)
				_signal_job(job_ptr->job_id, GS_RESUME);

			/* transfer the job as long as it is still active */
			if (job_ptr->job_state == JOB_SUSPENDED ||
			    job_ptr->job_state == JOB_RUNNING) {				
				_add_job_to_part(newp_ptr, job_ptr->job_id,
						 job_ptr->node_bitmap);
			}
		}
	}

	/* confirm all jobs. Scan the master job_list and confirm that we
	 * are tracking all jobs */
	_scan_slurm_job_list();

	/* Finally, destroy the old data */
	p_ptr = gs_part_list;
	gs_part_list = old_part_list;
	_destroy_parts();
	gs_part_list = p_ptr;

	pthread_mutex_unlock(&data_mutex);
	debug3("sched/gang: leaving gs_reconfig");
	return SLURM_SUCCESS;
}

/************************************
 * Timeslicer Functions
 ***********************************/

/* Build the active row from the job_list.
 * The job_list is assumed to be sorted */
static void
_build_active_row(struct gs_part *p_ptr)
{
	int i;
	
	debug3("sched/gang: entering _build_active_row");
	p_ptr->jobs_active = 0;
	if (p_ptr->num_jobs == 0)
		return;
	
	/* apply all shadow jobs first */
	for (i = 0; i < p_ptr->num_shadows; i++) {
		_add_job_to_active(p_ptr->shadow[i], p_ptr);
	}
	
	/* attempt to add jobs from the job_list in the current order */
	for (i = 0; i < p_ptr->num_jobs; i++) {
		if (_job_fits_in_active_row(p_ptr->job_list[i], p_ptr)) {
			_add_job_to_active(p_ptr->job_list[i], p_ptr);
			p_ptr->job_list[i]->row_state = GS_ACTIVE;
		}
	}
	debug3("sched/gang: leaving _build_active_row");
}

/* _cycle_job_list
 *
 * This is the heart of the timeslicer. The algorithm works as follows:
 *
 * 1. Each new job is added to the end of the job list, so the earliest job
 *    is at the front of the list.
 * 2. Any "shadow" jobs are first applied to the active_resmap. Then the
 *    active_resmap is filled out by starting with the first job in the list,
 *    and adding to it any job that doesn't conflict with the resources.
 * 3. When the timeslice has passed, all jobs that were added to the active
 *    resmap are moved to the back of the list (preserving their order among
 *    each other).
 * 4. Loop back to step 2, starting with the new "first job in the list".
 */
static void
_cycle_job_list(struct gs_part *p_ptr)
{
	int i, j;
	struct gs_job *j_ptr;
	
	debug3("sched/gang: entering _cycle_job_list");
	_print_jobs(p_ptr);
	/* re-prioritize the job_list and set all row_states to GS_NO_ACTIVE */
	for (i = 0; i < p_ptr->num_jobs; i++) {
		while (p_ptr->job_list[i]->row_state == GS_ACTIVE) {
			/* move this job to the back row and "de-activate" it */
			j_ptr = p_ptr->job_list[i];
			j_ptr->row_state = GS_NO_ACTIVE;
			for (j = i; j+1 < p_ptr->num_jobs; j++) {
				p_ptr->job_list[j] = p_ptr->job_list[j+1];
			}
			p_ptr->job_list[j] = j_ptr;
		}
		if (p_ptr->job_list[i]->row_state == GS_FILLER)
			p_ptr->job_list[i]->row_state = GS_NO_ACTIVE;
			
	}
	debug3("sched/gang: _cycle_job_list reordered job list:");
	_print_jobs(p_ptr);
	/* Rebuild the active row. */
	_build_active_row(p_ptr);
	debug3("sched/gang: _cycle_job_list new active job list:");
	_print_jobs(p_ptr);

	/* Suspend running jobs that are GS_NO_ACTIVE */
	for (i = 0; i < p_ptr->num_jobs; i++) {
		j_ptr = p_ptr->job_list[i];
		if (j_ptr->row_state == GS_NO_ACTIVE &&
		    j_ptr->sig_state == GS_RESUME) {
		    	debug3("sched/gang: _cycle_job_list: suspending job %u",
				j_ptr->job_id);
			_signal_job(j_ptr->job_id, GS_SUSPEND);
			j_ptr->sig_state = GS_SUSPEND;
			_clear_shadow(j_ptr);
		}
	}
	
	/* Resume suspended jobs that are GS_ACTIVE */
	for (i = 0; i < p_ptr->num_jobs; i++) {
		j_ptr = p_ptr->job_list[i];
		if (j_ptr->row_state == GS_ACTIVE &&
		    j_ptr->sig_state == GS_SUSPEND) {
		    	debug3("sched/gang: _cycle_job_list: resuming job %u",
				j_ptr->job_id);
			_signal_job(j_ptr->job_id, GS_RESUME);
			j_ptr->sig_state = GS_RESUME;
			_cast_shadow(j_ptr, p_ptr->priority);
		}
	}
	debug3("sched/gang: leaving _cycle_job_list");
}

/* The timeslicer thread */
static void *
_timeslicer_thread() {
	struct gs_part *p_ptr;
	int i;
	
	debug3("sched/gang: starting timeslicer loop");
	while (!thread_shutdown) {
		pthread_mutex_lock(&data_mutex);

		_sort_partitions();
		
		/* scan each partition... */
		debug3("sched/gang: _timeslicer_thread: scanning partitions");
		for (i = 0; i < num_sorted_part; i++) {
			p_ptr = gs_part_sorted[i];
			debug3("sched/gang: _timeslicer_thread: part %s: run %u total %u",
				p_ptr->part_name, p_ptr->jobs_active,
				p_ptr->num_jobs);
			if (p_ptr->jobs_active <
					p_ptr->num_jobs + p_ptr->num_shadows)
				_cycle_job_list(p_ptr);
		}
		pthread_mutex_unlock(&data_mutex);
		
		/* sleep AND check for thread termination requests */
		pthread_testcancel();
		debug3("sched/gang: _timeslicer_thread: preparing to sleep");
		sleep(timeslicer_seconds);
		debug3("sched/gang: _timeslicer_thread: waking up");
		pthread_testcancel();
	}
	pthread_exit((void *) 0);
	return NULL;
}