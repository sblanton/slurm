/*****************************************************************************\
 *  burst_buffer_common.c - Common logic for managing burst_buffers
 *****************************************************************************
 *  Copyright (C) 2014 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
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

#if     HAVE_CONFIG_H
#  include "config.h"
#endif

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include "slurm/slurm.h"

#include "src/common/list.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"

#include "burst_buffer_common.h"


/* Translate colon delimitted list of users into a UID array,
 * Return value must be xfreed */
static uid_t *_parse_users(char *buf)
{
	char *delim, *tmp, *tok, *save_ptr = NULL;
	int inx = 0, array_size;
	uid_t *user_array = NULL;

	if (!buf)
		return user_array;
	tmp = xstrdup(buf);
	delim = strchr(tmp, ',');
	if (delim)
		delim[0] = '\0';
	array_size = 1;
	user_array = xmalloc(sizeof(uid_t) * array_size);
	tok = strtok_r(tmp, ":", &save_ptr);
	while (tok) {
		if ((uid_from_string(tok, user_array + inx) == -1) ||
		    (user_array[inx] == 0)) {
			error("%s: ignoring invalid user: %s", __func__, tok);
		} else {
			if (++inx >= array_size) {
				array_size *= 2;
				user_array = xrealloc(user_array,
						      sizeof(uid_t)*array_size);
			}
		}
		tok = strtok_r(NULL, ":", &save_ptr);
	}
	xfree(tmp);
	return user_array;
}

/* Translate an array of (zero terminated) UIDs into a string with colon
 * delimited UIDs
 * Return value must be xfreed */
static char *_print_users(uid_t *buf)
{
	char *user_elem, *user_str = NULL;
	int i;

	if (!buf)
		return user_str;
	for (i = 0; buf[i]; i++) {
		user_elem = uid_to_string(buf[i]);
		if (!user_elem)
			continue;
		if (user_str)
			xstrcat(user_str, ":");
		xstrcat(user_str, user_elem);
		xfree(user_elem);
	}
	return user_str;
}

/* Allocate burst buffer hash tables */
extern void bb_alloc_cache(bb_alloc_t ***bb_hash_ptr, bb_user_t ***bb_uhash_ptr)
{
	*bb_hash_ptr  = xmalloc(sizeof(bb_alloc_t *) * BB_HASH_SIZE);
	*bb_uhash_ptr = xmalloc(sizeof(bb_user_t *)  * BB_HASH_SIZE);
}

/* Clear all cached burst buffer records, freeing all memory. */
extern void bb_clear_cache(bb_alloc_t ***bb_hash_ptr, bb_user_t ***bb_uhash_ptr)
{
	bb_alloc_t **bb_hash,   *bb_current,   *bb_next;
	bb_user_t  **user_hash, *user_current, *user_next;
	int i;

	bb_hash = *bb_hash_ptr;
	if (bb_hash) {
		for (i = 0; i < BB_HASH_SIZE; i++) {
			bb_current = bb_hash[i];
			while (bb_current) {
				bb_next = bb_current->next;
				xfree(bb_current);
				bb_current = bb_next;
			}
		}
		xfree(bb_hash);
		*bb_hash_ptr = NULL;
	}

	user_hash = *bb_uhash_ptr;
	if (user_hash) {
		for (i = 0; i < BB_HASH_SIZE; i++) {
			user_current = user_hash[i];
			while (user_current) {
				user_next = user_current->next;
				xfree(user_current);
				user_current = user_next;
			}
		}
		xfree(user_hash);
		*bb_uhash_ptr = NULL;
	}
}

/* Clear configuration parameters, free memory */
extern void bb_clear_config(bb_config_t *config_ptr)
{
	xassert(config_ptr);
	xfree(config_ptr->allow_users);
	xfree(config_ptr->allow_users_str);
	config_ptr->debug_flag = false;
	xfree(config_ptr->deny_users);
	xfree(config_ptr->deny_users_str);
	xfree(config_ptr->get_sys_state);
	config_ptr->job_size_limit = NO_VAL;
	config_ptr->stage_in_timeout = 0;
	config_ptr->stage_out_timeout = 0;
	config_ptr->prio_boost_alloc = 0;
	config_ptr->prio_boost_use = 0;
	xfree(config_ptr->start_stage_in);
	xfree(config_ptr->start_stage_out);
	xfree(config_ptr->stop_stage_in);
	xfree(config_ptr->stop_stage_out);
	config_ptr->user_size_limit = NO_VAL;
}

/* Find a per-job burst buffer record for a specific job.
 * If not found, return NULL. */
extern bb_alloc_t *bb_find_job_rec(struct job_record *job_ptr,
				   bb_alloc_t **bb_hash)
{
	bb_alloc_t *bb_ptr = NULL;

	xassert(job_ptr);
	bb_ptr = bb_hash[job_ptr->user_id % BB_HASH_SIZE];
	while (bb_ptr) {
		if (bb_ptr->job_id == job_ptr->job_id) {
			if (bb_ptr->user_id == job_ptr->user_id)
				return bb_ptr;
			error("%s: Slurm state inconsistent with burst "
			      "buffer. JobID %u has UserID mismatch (%u != %u)",
			      __func__, job_ptr->user_id, bb_ptr->user_id,
			      job_ptr->user_id);
			/* This has been observed when slurmctld crashed and
			 * the job state recovered was missing some jobs
			 * which already had burst buffers configured. */
		}
		bb_ptr = bb_ptr->next;
	}
	return bb_ptr;
}

/* Find a per-user burst buffer record for a specific user ID */
extern bb_user_t *bb_find_user_rec(uint32_t user_id, bb_user_t **bb_uhash)
{
	int inx = user_id % BB_HASH_SIZE;
	bb_user_t *user_ptr;

	xassert(bb_uhash);
	user_ptr = bb_uhash[inx];
	while (user_ptr) {
		if (user_ptr->user_id == user_id)
			return user_ptr;
		user_ptr = user_ptr->next;
	}
	user_ptr = xmalloc(sizeof(bb_user_t));
	user_ptr->next = bb_uhash[inx];
	/* user_ptr->size = 0;	initialized by xmalloc */
	user_ptr->user_id = user_id;
	bb_uhash[inx] = user_ptr;
	return user_ptr;
}

/* Remove a burst buffer allocation from a user's load */
extern void bb_remove_user_load(bb_alloc_t *bb_ptr, uint32_t *used_space,
				bb_user_t **bb_uhash)
{
	bb_user_t *user_ptr;
	uint32_t tmp_u, tmp_j;

	if (*used_space >= bb_ptr->size) {
		*used_space -= bb_ptr->size;
	} else {
		error("%s: used space underflow releasing buffer for job %u",
		      __func__, bb_ptr->job_id);
		*used_space = 0;
	}

	user_ptr = bb_find_user_rec(bb_ptr->user_id, bb_uhash);
	if ((user_ptr->size & BB_SIZE_IN_NODES) ||
	    (bb_ptr->size   & BB_SIZE_IN_NODES)) {
		tmp_u = user_ptr->size & (~BB_SIZE_IN_NODES);
		tmp_j = bb_ptr->size   & (~BB_SIZE_IN_NODES);
		if (tmp_u > tmp_j) {
			user_ptr->size = tmp_u + tmp_j;
			user_ptr->size |= BB_SIZE_IN_NODES;
		} else {
			error("%s: user %u table underflow",
			      __func__, user_ptr->user_id);
			user_ptr->size = BB_SIZE_IN_NODES;
		}
	} else if (user_ptr->size >= bb_ptr->size) {
		user_ptr->size -= bb_ptr->size;
	} else {
		error("%s: user %u table underflow",
		      __func__, user_ptr->user_id);
		user_ptr->size = 0;
	}
}

/* Load and process configuration parameters */
extern void bb_load_config(bb_config_t *config_ptr)
{
	s_p_hashtbl_t *bb_hashtbl = NULL;
	char *bb_conf, *tmp = NULL, *value;
	static s_p_options_t bb_options[] = {
		{"AllowUsers", S_P_STRING},
		{"DenyUsers", S_P_STRING},
		{"GetSysState", S_P_STRING},
		{"JobSizeLimit", S_P_STRING},
		{"PrioBoostAlloc", S_P_UINT32},
		{"PrioBoostUse", S_P_UINT32},
		{"StageInTimeout", S_P_UINT32},
		{"StageOutTimeout", S_P_UINT32},
		{"StartStageIn", S_P_STRING},
		{"StartStageOut", S_P_STRING},
		{"StopStageIn", S_P_STRING},
		{"StopStageOut", S_P_STRING},
		{"UserSizeLimit", S_P_STRING},
		{NULL}
	};

	bb_clear_config(config_ptr);
	if (slurm_get_debug_flags() & DEBUG_FLAG_BURST_BUF)
		config_ptr->debug_flag = true;

	bb_conf = get_extra_conf_path("burst_buffer.conf");
	bb_hashtbl = s_p_hashtbl_create(bb_options);
	if (s_p_parse_file(bb_hashtbl, NULL, bb_conf, false) == SLURM_ERROR) {
		fatal("%s: something wrong with opening/reading %s: %m",
		      __func__, bb_conf);
	}
	if (s_p_get_string(&config_ptr->allow_users_str, "AllowUsers",
			   bb_hashtbl)) {
		config_ptr->allow_users = _parse_users(config_ptr->
						       allow_users_str);
	}
	if (s_p_get_string(&config_ptr->deny_users_str, "DenyUsers",
			   bb_hashtbl)) {
		config_ptr->deny_users = _parse_users(config_ptr->
						      deny_users_str);
	}
	s_p_get_string(&config_ptr->get_sys_state, "GetSysState", bb_hashtbl);
	if (s_p_get_string(&tmp, "JobSizeLimit", bb_hashtbl)) {
		config_ptr->job_size_limit = bb_get_size_num(tmp);
		xfree(tmp);
	}
	if (s_p_get_uint32(&config_ptr->prio_boost_alloc, "PrioBoostAlloc",
			   bb_hashtbl) &&
	    (config_ptr->prio_boost_alloc > NICE_OFFSET)) {
		error("%s: PrioBoostAlloc can not exceed %u",
		      __func__, NICE_OFFSET);
		config_ptr->prio_boost_alloc = NICE_OFFSET;
	}
	if (s_p_get_uint32(&config_ptr->prio_boost_use, "PrioBoostUse",
			   bb_hashtbl) &&
	    (config_ptr->prio_boost_use > NICE_OFFSET)) {
		error("%s: PrioBoostUse can not exceed %u",
		      __func__, NICE_OFFSET);
		config_ptr->prio_boost_use = NICE_OFFSET;
	}
	s_p_get_uint32(&config_ptr->stage_in_timeout, "StageInTimeout",
		       bb_hashtbl);
	s_p_get_uint32(&config_ptr->stage_out_timeout, "StageOutTimeout",
		       bb_hashtbl);
	s_p_get_string(&config_ptr->start_stage_in, "StartStageIn", bb_hashtbl);
	s_p_get_string(&config_ptr->start_stage_out, "StartStageOut",
			    bb_hashtbl);
	s_p_get_string(&config_ptr->stop_stage_in, "StopStageIn", bb_hashtbl);
	s_p_get_string(&config_ptr->stop_stage_out, "StopStageOut", bb_hashtbl);
	if (s_p_get_string(&tmp, "UserSizeLimit", bb_hashtbl)) {
		config_ptr->user_size_limit = bb_get_size_num(tmp);
		xfree(tmp);
	}

	s_p_hashtbl_destroy(bb_hashtbl);
	xfree(bb_conf);

	if (config_ptr->debug_flag) {
		value = _print_users(config_ptr->allow_users);
		info("%s: AllowUsers:%s",  __func__, value);
		xfree(value);

		value = _print_users(config_ptr->deny_users);
		info("%s: DenyUsers:%s",  __func__, value);
		xfree(value);

		info("%s: GetSysState:%s",  __func__,
		     config_ptr->get_sys_state);
		info("%s: JobSizeLimit:%u",  __func__,
		     config_ptr->job_size_limit);
		info("%s: PrioBoostAlloc:%u", __func__,
		     config_ptr->prio_boost_alloc);
		info("%s: PrioBoostUse:%u", __func__,
		     config_ptr->prio_boost_use);
		info("%s: StageInTimeout:%u", __func__,
		     config_ptr->stage_in_timeout);
		info("%s: StageOutTimeout:%u", __func__,
		     config_ptr->stage_out_timeout);
		info("%s: StartStageIn:%s",  __func__,
		     config_ptr->start_stage_in);
		info("%s: StartStageOut:%s",  __func__,
		     config_ptr->start_stage_out);
		info("%s: StopStageIn:%s",  __func__,
		     config_ptr->stop_stage_in);
		info("%s: StopStageOut:%s",  __func__,
		     config_ptr->stop_stage_out);
		info("%s: UserSizeLimit:%u",  __func__,
		     config_ptr->user_size_limit);
	}
}

/* Pack individual burst buffer records into a  buffer */
extern int bb_pack_bufs(bb_alloc_t **bb_hash, Buf buffer,
			uint16_t protocol_version)
{
	int i, rec_count = 0;
	struct bb_alloc *bb_next;

	if (!bb_hash)
		return rec_count;

	for (i = 0; i < BB_HASH_SIZE; i++) {
		bb_next = bb_hash[i];
		while (bb_next) {
			pack32(bb_next->array_job_id,  buffer);
			pack32(bb_next->array_task_id, buffer);
			pack32(bb_next->job_id,        buffer);
			packstr(bb_next->name,         buffer);
			pack32(bb_next->size,          buffer);
			pack16(bb_next->state,         buffer);
			pack_time(bb_next->state_time, buffer);
			pack32(bb_next->user_id,       buffer);
			rec_count++;
			bb_next = bb_next->next;
		}
	}

	return rec_count;
}

/* Pack configuration parameters into a buffer */
extern void bb_pack_config(bb_config_t *config_ptr, Buf buffer,
			   uint16_t protocol_version)
{
	packstr(config_ptr->allow_users_str, buffer);
	packstr(config_ptr->deny_users_str,  buffer);
	packstr(config_ptr->get_sys_state,   buffer);
	packstr(config_ptr->start_stage_in,  buffer);
	packstr(config_ptr->start_stage_out, buffer);
	packstr(config_ptr->stop_stage_in,   buffer);
	packstr(config_ptr->stop_stage_out,  buffer);
	pack32(config_ptr->job_size_limit,   buffer);
	pack32(config_ptr->prio_boost_alloc, buffer);
	pack32(config_ptr->prio_boost_use,   buffer);
	pack32(config_ptr->stage_in_timeout, buffer);
	pack32(config_ptr->stage_out_timeout,buffer);
	pack32(config_ptr->user_size_limit,  buffer);
}

/* Translate a burst buffer size specification in string form to numeric form,
 * recognizing various sufficies (MB, GB, TB, PB, and Nodes). */
extern uint32_t bb_get_size_num(char *tok)
{
	char *end_ptr = NULL;
	int32_t bb_size_i;
	uint32_t bb_size_u = 0;

	bb_size_i = strtol(tok, &end_ptr, 10);
	if (bb_size_i > 0) {
		bb_size_u = (uint32_t) bb_size_i;
		if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
			bb_size_u = (bb_size_u + 1023) / 1024;
		} else if ((end_ptr[0] == 'g') || (end_ptr[0] == 'G')) {
			;
		} else if ((end_ptr[0] == 't') || (end_ptr[0] == 'T')) {
			bb_size_u *= 1024;
		} else if ((end_ptr[0] == 'p') || (end_ptr[0] == 'P')) {
			bb_size_u *= (1024 * 1024);
		} else if ((end_ptr[0] == 'n') || (end_ptr[0] == 'N')) {
			bb_size_u |= BB_SIZE_IN_NODES;
		}
	}
	return bb_size_u;
}

extern void bb_job_queue_del(void *x)
{
	xfree(x);
}

/* Sort job queue by expected start time */
extern int bb_job_queue_sort(void *x, void *y)
{
	job_queue_rec_t *job_rec1 = *(job_queue_rec_t **) x;
	job_queue_rec_t *job_rec2 = *(job_queue_rec_t **) y;
	struct job_record *job_ptr1 = job_rec1->job_ptr;
	struct job_record *job_ptr2 = job_rec2->job_ptr;

	if (job_ptr1->start_time > job_ptr2->start_time)
		return 1;
	if (job_ptr1->start_time < job_ptr2->start_time)
		return -1;
	return 0;
}

/* Sort preempt_bb_recs in order of DECREASING use_time */
extern int bb_preempt_queue_sort(void *x, void *y)
{
	struct preempt_bb_recs *bb_ptr1 = *(struct preempt_bb_recs **) x;
	struct preempt_bb_recs *bb_ptr2 = *(struct preempt_bb_recs **) y;

	if (bb_ptr1->use_time > bb_ptr2->use_time)
		return -1;
	if (bb_ptr1->use_time < bb_ptr2->use_time)
		return 1;
	return 0;
};