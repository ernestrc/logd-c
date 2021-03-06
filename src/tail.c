#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "./tail.h"
#include "./util.h"

static void tail_kill_tail(tail_t* tail);
static void tail_close_pipes(tail_t* tail);
static void tail_spawn(uv_timer_t*);

static void set_inmediate(uv_loop_t* loop, void* data, uv_timer_cb cb)
{
	static uv_timer_t timer;
	uv_timer_init(loop, &timer);

	timer.data = data;
	uv_timer_start(&timer, cb, 0, 0);
}

static void on_tail_stderr(uv_poll_t* req, int status, int events)
{
#define STDERR_BUF_SIZE 100
	char buf[STDERR_BUF_SIZE];
	if (status < 0) {
		errno = -status;
		perror("uv_poll_poll");
		return;
	}
	tail_t* tail = (tail_t*)req->data;

	errno = 0;
	int ret = read(tail->read_tail_stderr_fd, &buf, STDERR_BUF_SIZE);
	if (errno == EAGAIN) {
		DEBUG_LOG("stderr tail not ready, fd: %d", tail->read_data_fd);
		return;
	}
	if (ret < 0) {
		DEBUG_LOG("stderr tail read error, errno: %d", errno);
		return;
	}
	if (ret == 0) {
		DEBUG_LOG("stderr tail read EOF, ret: %d", ret);
		return;
	}

	fprintf(stderr, "%.*s", ret, (char*)&buf);
}

static void tail_call_exit_cb(tail_t* tail, int status)
{
	if (tail->exit_cb) {
		tail->exit_cb(status);
	}
}

static void on_tail_exit(
  uv_process_t* req, int64_t exit_status, int term_signal)
{
	tail_t* tail = req->data;

	DEBUG_LOG("tail subprocess with pid %d exited with status %ld and signal "
			  "%d, tail_state: %d",
	  req->pid, exit_status, term_signal, tail->state);

	uv_close((uv_handle_t*)req, NULL);

	switch (tail->state) {
	case CLOSING_FREEING_TSTATE:
		free(tail);
		return;
	case CLOSING_OPENING_TSTATE:
		set_inmediate(tail->loop, tail, tail_spawn);
		break;
	case OPEN_TSTATE:
		tail_close_pipes(tail);
		/* fallthrough */
	case CLOSING_TSTATE:
		break;
	case INIT_TSTATE:
		abort();
		break;
	}

	tail->state = INIT_TSTATE;
	tail_call_exit_cb(tail, exit_status ? exit_status : term_signal + 128);
}

tail_t* tail_create(uv_loop_t* loop, char* input_file)
{
	if (input_file == NULL || loop == NULL) {
		errno = EINVAL;
		return NULL;
	}
	tail_t* tail = calloc(1, sizeof(tail_t));
	if (tail == NULL) {
		errno = ENOMEM;
		return NULL;
	}
	tail->loop = loop;

	tail->proc_args[0] = "tail";
	tail->proc_args[1] = "-n0";
	tail->proc_args[2] = "-f";
	tail->proc_args[3] = input_file;
	tail->proc_args[4] = NULL;

	tail->proc_options.exit_cb = on_tail_exit;
	tail->proc_options.file = "tail";
	tail->proc_options.args = tail->proc_args;
	tail->proc_options.flags = 0;

	return tail;
}

static int tail_open_pipes(tail_t* tail)
{
	int pipefd[2];
	int ret;

	UNEINTR(pipe(pipefd));
	tail->read_data_fd = pipefd[0];
	tail->write_data_fd = pipefd[1];

	UNEINTR(pipe(pipefd));
	tail->read_tail_stderr_fd = pipefd[0];
	tail->write_tail_stderr_fd = pipefd[1];

	// gnu tail doesn't need nonblock mode
	// UNEINTR(fcntl(tail->write_tail_stderr_fd, F_SETFL, O_NONBLOCK));
	// UNEINTR(fcntl(tail->write_data_fd, F_SETFL, O_NONBLOCK));
	UNEINTR(fcntl(tail->read_data_fd, F_SETFL, O_NONBLOCK));
	UNEINTR(fcntl(tail->read_tail_stderr_fd, F_SETFL, O_NONBLOCK));

	if ((ret = uv_poll_init(tail->loop, &tail->uv_poll_err_req,
		   tail->read_tail_stderr_fd)) < 0 ||
	  (ret = uv_poll_start(
		 &tail->uv_poll_err_req, UV_READABLE, &on_tail_stderr)) < 0) {
		errno = -ret;
		perror("uv_poll");
		return 1;
	}

	DEBUG_LOG("opened tail pipes, write_tail_stderr_fd: %d, "
			  "read_tail_stderr_fd: %d, write_data_fd: %d, "
			  "read_data_fd: %d",
	  tail->write_tail_stderr_fd, tail->read_tail_stderr_fd,
	  tail->write_data_fd, tail->read_data_fd);

	tail->uv_poll_err_req.data = tail;

	return 0;
}

static void tail_spawn(uv_timer_t* timer)
{
	tail_t* tail = (tail_t*)timer->data;

	tail->proc_child_stdio[0].flags = UV_IGNORE;
	tail->proc_child_stdio[1].flags = UV_INHERIT_FD;
	tail->proc_child_stdio[1].data.fd = tail->write_data_fd;
	tail->proc_child_stdio[2].flags = UV_INHERIT_FD;
	tail->proc_child_stdio[2].data.fd = tail->write_tail_stderr_fd;

	tail->proc_options.stdio_count = 3;
	tail->proc_options.stdio = tail->proc_child_stdio;
	tail->proc.data = tail;

	int r;
	if ((r = uv_spawn(tail->loop, &tail->proc, &tail->proc_options))) {
		errno = -r;
		perror("uv_spawn");
		tail_close_pipes(tail);
		tail_call_exit_cb(tail, 1);
		tail->state = INIT_TSTATE;
		return;
	}

	tail->state = OPEN_TSTATE;

	DEBUG_LOG("executed new tail subprocess, pid: %d", tail->proc.pid);
}

int tail_open(tail_t* tail, void (*exit_cb)(int))
{
	switch (tail->state) {
	case OPEN_TSTATE:
		tail_kill_tail(tail);
		/* fallthrough */
	case CLOSING_OPENING_TSTATE:
		tail_close_pipes(tail);
		/* fallthrough */
	case CLOSING_FREEING_TSTATE:
		/* fallthrough */
	case CLOSING_TSTATE:
		tail->state = CLOSING_OPENING_TSTATE;
		if (tail_open_pipes(tail) != 0) {
			perror("tail_open_pipes");
			return -1;
		}
		return tail->read_data_fd;
	case INIT_TSTATE:
		break;
	}
	DEBUG_ASSERT(tail != NULL);

	if (tail_open_pipes(tail) != 0) {
		perror("tail_open_pipes");
		return -1;
	}

	tail->exit_cb = exit_cb;

	// we need to spawn asynchronously because libuv calls uv__finish_close even
	// after calling proc exit callback so we run into some assertion failures
	set_inmediate(tail->loop, tail, tail_spawn);

	return tail->read_data_fd;
}

static void tail_close_pipes(tail_t* tail)
{
	DEBUG_LOG("closing pipes, write_tail_stderr_fd: %d, read_tail_stderr_fd: "
			  "%d, write_data_fd: %d, "
			  "read_data_fd: %d",
	  tail->write_tail_stderr_fd, tail->read_tail_stderr_fd,
	  tail->write_data_fd, tail->read_data_fd);

	UNEINTR2(close(tail->write_tail_stderr_fd));
	UNEINTR2(close(tail->read_tail_stderr_fd));
	UNEINTR2(close(tail->write_data_fd));
	UNEINTR2(close(tail->read_data_fd));
	uv_poll_stop(&tail->uv_poll_err_req);
}

static void tail_kill_tail(tail_t* tail)
{
	DEBUG_LOG("killing tail, pid: %d", tail->proc.pid);

	uv_process_kill(&tail->proc, SIGTERM);
	tail->state = CLOSING_TSTATE;
}

void tail_close(tail_t* tail)
{
	switch (tail->state) {
	case CLOSING_OPENING_TSTATE:
		tail_close_pipes(tail);
		/* fallthrough */
	case CLOSING_FREEING_TSTATE:
		tail->state = CLOSING_TSTATE;
		/* fallthrough */
	case CLOSING_TSTATE:
		/* fallthrough */
	case INIT_TSTATE:
		return;
	case OPEN_TSTATE:
		break;
	}

	tail_close_pipes(tail);
	tail_kill_tail(tail);
}

void tail_free(tail_t* tail)
{
	if (tail == NULL)
		return;

	switch (tail->state) {
	case INIT_TSTATE:
		free(tail);
		return;
	case OPEN_TSTATE:
		tail_kill_tail(tail);
		/* fallthrough */
	case CLOSING_OPENING_TSTATE:
		tail_close_pipes(tail);
		/* fallthrough */
	case CLOSING_FREEING_TSTATE:
		/* fallthrough */
	case CLOSING_TSTATE:
		break;
	}

	tail->state = CLOSING_FREEING_TSTATE;
}
