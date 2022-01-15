/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 *
 * mod_pipe_stream.c -- Local Streaming Audio
 *
 */
#include <switch.h>
#include <sys/wait.h>

#define MY_BUF_LEN 1024 * 32
#define MY_BLOCK_SIZE MY_BUF_LEN


SWITCH_MODULE_LOAD_FUNCTION(mod_pipe_stream_load);

SWITCH_MODULE_DEFINITION(mod_pipe_stream, mod_pipe_stream_load, NULL, NULL);

struct pipe_stream_context {
	int fds[2];
	int pid;
	char *command;
	switch_buffer_t *audio_buffer;
	switch_mutex_t *mutex;
	switch_thread_rwlock_t *rwlock;
	int running;
	switch_thread_t *thread;
	int fd; 
};

typedef struct pipe_stream_context pipe_stream_context_t;

static void *SWITCH_THREAD_FUNC pipe_stream_buffer_thread_run(switch_thread_t *thread, void *obj)
{
	pipe_stream_context_t *context = (pipe_stream_context_t *) obj;
	switch_byte_t data[MY_BUF_LEN];
	ssize_t rlen;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_buffer_thread_run entry !\n");

	context->running = 1;

	if (switch_thread_rwlock_tryrdlock(context->rwlock) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Read Lock Fail\n");
		goto end;
	}

	while (context->running) {

		rlen = read(context->fd, data, MY_BUF_LEN);

		if (rlen <= 3) {
			break;
		}

		switch_mutex_lock(context->mutex);
		switch_buffer_write(context->audio_buffer, data, rlen);
		switch_mutex_unlock(context->mutex);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_buffer_thread_run exit !\n");

	switch_thread_rwlock_unlock(context->rwlock);

  end:
    remove(context->command);

	context->running = 0;

	return NULL;
}

static switch_status_t pipe_stream_file_open(switch_file_handle_t *handle, const char *path)
{
	pipe_stream_context_t *context;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_threadattr_t *thd_attr = NULL;

	if (switch_test_flag(handle, SWITCH_FILE_FLAG_WRITE)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "This format does not support writing!\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_open path=%s !\n",path);

	handle->channels = 1;

	context = switch_core_alloc(handle->memory_pool, sizeof(*context));

	context->fd = open(path, O_RDONLY);

	if (context->fd<0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_open fd open failed! %d\n",context->fd);
		goto error;
	} else {
		handle->private_info = context;
		status = SWITCH_STATUS_SUCCESS;

		if (switch_buffer_create_dynamic(&context->audio_buffer, MY_BLOCK_SIZE, MY_BUF_LEN, 0) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Write Buffer Failed!\n");
			goto error;
		}

		context->command = switch_core_sprintf(handle->memory_pool, "%s", path);

		switch_thread_rwlock_create(&context->rwlock, handle->memory_pool);

		switch_mutex_init(&context->mutex, SWITCH_MUTEX_NESTED, handle->memory_pool);

		switch_threadattr_create(&thd_attr, handle->memory_pool);

		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_open start pipe_stream_buffer_thread_run !\n");

		switch_thread_create(&context->thread, thd_attr, pipe_stream_buffer_thread_run, context, handle->memory_pool);

		context->running = 2;

		while (context->running == 2) {
			switch_cond_next();
		}

		goto end;
	}

	error:
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_open exit with error SWITCH_STATUS_FALSE !\n");
	status = SWITCH_STATUS_FALSE;

	end:
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_open exit with end: status=%d !\n",status);
	return status;
}

static switch_status_t pipe_stream_file_close(switch_file_handle_t *handle)
{
	pipe_stream_context_t *context = handle->private_info;
	switch_status_t st;

	context->running = 0;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_close entry !\n");

	if (context->fds[0] > -1) {
		close(context->fds[0]);
	}

	if (context->thread) {
		switch_thread_join(&st, context->thread);
	}

	if (context->audio_buffer) {
		switch_buffer_destroy(&context->audio_buffer);
	}

	switch_thread_rwlock_wrlock(context->rwlock);
	switch_thread_rwlock_unlock(context->rwlock);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_close exit !\n");

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t pipe_stream_file_read(switch_file_handle_t *handle, void *data, size_t *len)
{
	pipe_stream_context_t *context = handle->private_info;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_size_t rlen = *len * 2;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_read entry rlen=%ld !\n",rlen);

	while (context->running && switch_buffer_inuse(context->audio_buffer) < rlen) {
		switch_cond_next();
	}

	switch_mutex_lock(context->mutex);
	*len = switch_buffer_read(context->audio_buffer, data, rlen) / 2;
	switch_mutex_unlock(context->mutex);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "pipe_stream_file_read entry return len=%ld !\n",*len);

	return status;
}

/* Registration */

static char *supported_formats[SWITCH_MAX_CODECS] = { 0 };

SWITCH_MODULE_LOAD_FUNCTION(mod_pipe_stream_load)
{
	switch_file_interface_t *file_interface;
	supported_formats[0] = "pipe_stream";

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_pipe_stream_load! entry\n");

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	file_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_FILE_INTERFACE);
	file_interface->interface_name = modname;
	file_interface->extens = supported_formats;
	file_interface->file_open = pipe_stream_file_open;
	file_interface->file_close = pipe_stream_file_close;
	file_interface->file_read = pipe_stream_file_read;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_pipe_stream_load! exit\n");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
