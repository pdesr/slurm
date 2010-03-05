/*****************************************************************************\
 *  slurm_protocol_common.h - slurm communications definitions common to 
 *	all protocols
 *****************************************************************************
 *  Copyright (C) 2002 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Kevin Tew <tew1@llnl.gov>, et. al.
 *  UCRL-CODE-217948.
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

#ifndef _SLURM_PROTOCOL_COMMON_H
#define _SLURM_PROTOCOL_COMMON_H

#if HAVE_CONFIG_H
#  include "config.h"
#endif

#include <sys/time.h>
#include <time.h>

#include <slurm/slurm_errno.h>

/* for sendto and recvfrom commands */
#define SLURM_PROTOCOL_NO_SEND_RECV_FLAGS 0

/* for accpet commands */
#define SLURM_PROTOCOL_DEFAULT_LISTEN_BACKLOG 128

/* used in interface methods */
#define SLURM_PROTOCOL_FUNCTION_NOT_IMPLEMENTED -2 

/* max slurm message send and receive buffer size
 * this may need to be increased to 350k-512k */
#define SLURM_PROTOCOL_MAX_MESSAGE_BUFFER_SIZE (512*1024)

/* slurm protocol header defines, based upon config.h, 16 bits */ 
#define SLURM_PROTOCOL_VERSION ((SLURM_API_MAJOR << 8) | SLURM_API_AGE)

/* used to set flags to empty */
#define SLURM_PROTOCOL_NO_FLAGS 0 

#if MONGO_IMPLEMENTATION
#  include <src/common/slurm_protocol_mongo_common.h>
#else
#  include <src/common/slurm_protocol_socket_common.h>
#endif

#endif