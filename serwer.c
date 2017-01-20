

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <pthread.h>
#include <string.h>

#include "kom.h"
#include "err.h"

#ifdef NO_LOGGING
#  undef log
#  define log(...)
#  undef log_first
#  define log_first(...)
#  define log_middle(...)
#  define log_last(...)
#endif // NO_LOGGING


#define MAX_OWNED 			10
#define MAX_PID_SUPPORTED	32768
#define NCLIENTS			(MAX_PID_SUPPORTED + 1)

typedef array_index_type i_type;
typedef array_value_type v_type;
typedef struct array_msg_from_client msg_from_client;
typedef struct array_msg_from_server msg_from_server;

// all threads will stop (won't wait on anything) and main thread will clean up
// set to 1 in signal handler only
bool interrupt;

struct client_entry {
	size_t owned_cnt;
	int owned[MAX_OWNED]; 		// owned indices in array
	pthread_t thread_id;
	_Bool is_active;		    // whether thread is alive
	pthread_cond_t on_message;
	_Bool done;					// true to inform thread that it should terminate ASAP
} clients[NCLIENTS];

// last_msg.pid == 0 means that there's no message for anybody
// last_msg.pid = sth  means that there's a message for somebody
// main thread sets last_msg.pid to some pid and signals clients[pid].on_message
// last_msg.pid is set to 0 by thread immediately after receiving the message
// on_last_msg_copied is signalled shortly after, to tell main thread that it can proceed
msg_from_client last_msg;
pthread_cond_t on_last_msg_copied;

// every access or write to any shared global is synchronized using this mutex
pthread_mutex_t mut_globals;

// counter, so that there's no need to iterate over "clients" array on cleanup when there are no threads
int workers_to_kill;

int n;
struct array_entry {
	int value;
	pid_t owner;
	pthread_cond_t on_access;
} *array;

/**
 * On error, nothing is cleaned - "syserr" from err.h is called (it's modified version of commonly used err.h).
 * Functions either return void, or return 0/1 - 1 means that thread (worker/main) should clean itself
 * 	and terminate ASAP.
 * 	If such thing happens, thread absolutely can't hang on anything (especially condition variables!).
 *
 * Legend:
 * worker_* - methods of worker threads that use global variables
 * 	they usually require mut_globals to be locked, or lock it themselves
 * main_* - methods of main thread that use global variables
 *  they also usually require mut_globals to be locked, or lock it themselves
 * * - all other functions are helper functions and don't use global state at all
 */

/*
 * Two helper wrappers for pthread methods - we need them because they lock mutex,
 * 	and we need to check whether we should clean stuff up when this happens
 */
static
int worker_cond_wait(pid_t pid, pthread_cond_t *cond) {
	pthread_cond_wait(cond, &mut_globals);
	return interrupt || clients[pid].done ? 1 : 0;
}

static
int worker_mutex_lock(pid_t pid) {
	pthread_mutex_lock(&mut_globals);
	return interrupt || clients[pid].done ? 1 : 0;
}

/**
 * Two helper functions used in handling array fields' ownership.
 */
static
void insert_into_sorted(int size, int *vs, int v) {
	int pos = 0;
	while (pos < size && v >= vs[pos])
		pos++;
	const int msize = (size - pos) * sizeof (int);
	if (msize)
		memmove((void *) &vs[pos + 1], (void *) &vs[pos], msize);
	vs[pos] = v;
}

static
void check_idxs(int idxsn, const int *idxs, int max_idx) {
	if (idxsn >= MAX_OWNED)
		syserr("idxs' count %d too big.", idxsn);
	for (int i = 0; i < idxsn; i++) {
		if (i != 0 && idxs[i-1] > idxs[i])
			syserr("Bad idxs order: idxs[%d]=%d, idxs[%d]=%d.",
					i-1, idxs[i-1], i, idxs[i]);
		if (idxs[i] < 0 || idxs[i] > max_idx)
			syserr("index %d not in range [0,%d].", idxs[i], max_idx);
	}
}

/**
 * Two functions for handling array fields' ownership.
 * When worker needs some fields, it waits for them (with pthread_cond_wait) in fixed order
 * 	- from the smaller to the bigger index. This way, there will be no deadlocks.
 *
 * These functions require mut_globals to be locked.
 */
static
int worker_acquire_idxs(pid_t pid, int size, const int *idxs) {
	for (int i = 0; i < size; i++) {
		if (array[idxs[i]].owner == pid)
			continue;
		while (array[idxs[i]].owner) {
			log("[%d] Waiting for index idx=%d, because %d owns it.", pid, idxs[i], array[idxs[i]].owner);
			if (1 == worker_cond_wait(pid, &array[idxs[i]].on_access))
				return 1;
		}
		array[idxs[i]].owner = pid;
		clients[pid].owned[clients[pid].owned_cnt++] = idxs[i];
		if (clients[pid].owned_cnt > MAX_OWNED)
			syserr("Too many indices owned by %d.", pid);
	}
	return 0;
}

static
void worker_giveback_idxs(pid_t pid) {
	for (size_t i = 0; i < clients[pid].owned_cnt; i++)
		array[clients[pid].owned[i]].owner = 0;
	for (size_t i = 0; i < clients[pid].owned_cnt; i++)
		pthread_cond_signal(&array[clients[pid].owned[i]].on_access);
	clients[pid].owned_cnt = 0;
}

/**
 * Handling messages getvalues/setvalues in following order:
 * 1. Check message's correctness, log useful information etc.
 * 2. Take ownership of required indices, if necessary
 * 3. Read/write values from/to array
 * 4. Prepare answer to client (OK or values), send it
 *
 * These functions require mut_globals to be locked.
 */
static
int worker_msg_getvalues(msg_from_client *msg) {
	// aliases
	const pid_t pid = msg->pid;
	const int size = msg->args.getvalues.count;
	const i_type *const idxs = msg->args.getvalues.indices;
	const _Bool lock = msg->args.getvalues.lock;
	const _Bool rvlock = msg->args.getvalues.rvlock;
	const int rv_index = msg->args.getvalues.rv_index;

	// logging and checking
	log_first("msg_getvalues(pid=%d): size=%d, lock=%d, rvlock=%d, rv_index=%d. tuples (idx,v,owner):",
			pid, size, lock, rvlock, rv_index);
	check_idxs(size, idxs, n - 1);
	for (int i = 0; i < size; i++)
		log_middle("(%d,%d,%d)", idxs[i], array[idxs[i]].value, array[idxs[i]].owner);
	log_last("");

	// locking
	if (lock) {
		log_first("msg_getvalues(pid=%d): Locking idxs.");
		int acquired_idxs[MAX_OWNED];
		memcpy(acquired_idxs, idxs, size * sizeof(int));
		int acquired_cnt = size;
		if (rvlock) {
			log_middle(" Locking %d too.", rv_index);
			insert_into_sorted(acquired_cnt, acquired_idxs, rv_index);
			acquired_cnt++;
		}
		log_last("");
		if (1 == worker_acquire_idxs(pid, acquired_cnt, acquired_idxs))
			return 1;
	}

	// responding with values
	msg_from_server resp = {
			.type = ArrayMsgtypeFromServerValues,
			.args.values.count = size,
	};
	for (int i = 0; i < size; i++)
		resp.args.values.values[i] = array[idxs[i]].value;
	pthread_mutex_unlock(&mut_globals);
	if (1 == server_sendmsg(pid, &resp))
		return 1;
	if (1 == worker_mutex_lock(pid))
		return 1;
	log("msg_getvalues(pid=%d) finished");
	return 0;
}

static
int worker_msg_setvalues(msg_from_client *msg) {
	// aliases
	const pid_t pid = msg->pid;
	const int size = msg->args.setvalues.count;
	const i_type *const idxs = msg->args.setvalues.indices;
	const v_type *const vals = msg->args.setvalues.values;

	// checks and logging
	log_first("msg_setvalues(pid=%d): size=%d. tuples (idx,oldv,newv,owner):", pid, size);
	check_idxs(size, idxs, n - 1);
	for (int i = 0; i < size; i++)
		log_middle("(%d,%d,%d,%d)", idxs[i], array[idxs[i]].value, vals[i], array[idxs[i]].owner);
	log_last("");

	// setting values
	if (1 == worker_acquire_idxs(pid, size, idxs))
		return 1;
	for (int i = 0; i < size; i++)
		array[idxs[i]].value = vals[i];
	worker_giveback_idxs(pid);

	// responding Ok
	msg_from_server resp = {
			.type = ArrayMsgtypeFromServerOk
	};
	pthread_mutex_unlock(&mut_globals);
	if (1 == server_sendmsg(pid, &resp))
		return 1;
	if (1 == worker_mutex_lock(pid))
		return 1;
	log("msg_setvalues(pid=%d) finished", pid);
	return 0;
}

static
int worker_send_ok(pid_t pid) {
	msg_from_server msg = {
			.type = ArrayMsgtypeFromServerOk
	};
	return server_sendmsg(pid, &msg);
}

/**
 * Worker's main loop.

 * It locks mut_globals itself, but doesn't unlock it.
 */
static
void worker_handle_client(pid_t pid) {
	log("[%d] Thread started.", pid);
	if (1 == worker_send_ok(pid))
		return;
	log("[%d] sent hello.", pid);
	if (1 == worker_mutex_lock(pid))
		return;
	while (true) {
		log("[%d]: hanging on on_message...", pid);
		while (last_msg.pid != pid)
			if (1 == worker_cond_wait(pid, &clients[pid].on_message))
				return;
		log("[%d]: hanging on on_message finished; setting last_msg.pid to 0", pid);
		msg_from_client msg = last_msg;
		last_msg.pid = 0;
		pthread_cond_signal(&on_last_msg_copied);

		// TODO maybe we unlock here?
		switch (msg.type) {
		case ArrayMsgtypeFromClientGetvalues:
			if (1 == worker_msg_getvalues(&msg))
				return;
			break;
		case ArrayMsgtypeFromClientSetvalues:
			if (1 == worker_msg_setvalues(&msg))
				return;
			break;
		default:
			syserr("[%d] Unknown msg.type: %d.", pid, msg.type);
		}
	}
}

/**
 * Worker thread's main function
 */
static
void* worker_main(void *data) {
	pid_t pid = *((pid_t *) data);
	free(data);
	worker_handle_client(pid);
	log("[%d] worker ends", pid);
	worker_giveback_idxs(pid);
	if (last_msg.pid == pid)
		pthread_cond_signal(&on_last_msg_copied);
	pthread_mutex_unlock(&mut_globals);
	return NULL;
}

/**
 * Cleanup function for specific PID
 *
 * Requires mut_globals to be locked
 */
static
void main_handle_msg_goodbye(pid_t pid) {
	log("msg_goodbye from pid %d.", pid);
	clients[pid].done = true;
	pthread_mutex_unlock(&mut_globals);
	if (0 != pthread_cond_signal(&clients[pid].on_message))
		syserr("pthread_cond_signal on %d", pid);
	if (0 != pthread_join(clients[pid].thread_id, 0))
		syserr("pthread_join on %d", pid);
	if (0 != pthread_cond_destroy(&clients[pid].on_message))
		syserr("pthread_cond_destroy on %d", pid);
	pthread_mutex_lock(&mut_globals);
	workers_to_kill--;
	clients[pid].is_active = false;
}


/**
 * Cleanup function for all clients and other global state
 * EXCEPT messaging device - messaging device is cleaned by signal handler
 * It works, because we assume that server terminates only by CTRL-C
 *
 * Can't be called from signal handler, because it uses pthread_cond_*
 *
 * Requires mut_globals to be locked
 */
static
void main_cleanup() {
	log("cleanup");

	interrupt = true;

	// waking up threads waiting for array access
	for (int i = 0; i < n; i++) {
		if (0 != pthread_cond_broadcast(&array[i].on_access))
			syserr("pthread_cond_signal for %d", i);
	}

	// threads and threads' messaging conds cleanup
	if (workers_to_kill) {
		// before joining threads, it's probably safer to give back mutex
		// we're only reading values, and clients[pid].is_active is used only by
		// main thread, so there're no dangerous races possible
		pthread_mutex_unlock(&mut_globals);
		for (pid_t pid = 0; pid < NCLIENTS; pid++) {
			if (clients[pid].is_active) {
				pthread_cond_signal(&clients[pid].on_message);
				pthread_join(clients[pid].thread_id, 0);
				pthread_cond_destroy(&clients[pid].on_message);
			}
		}
		pthread_mutex_lock(&mut_globals);
		for (pid_t pid = 0; pid < NCLIENTS; pid++) {
			if (clients[pid].is_active) {
				workers_to_kill--;
				clients[pid].is_active = false;
			}
		}
	}
	if (workers_to_kill)
		syserr("cleanup: threads_cnt not 0 after cleaning");

	// it can be done only after workers are done
	server_cleanup();

	// destroying remaining conds/mutex now that we know threads are dead
	for (int i = 0; i < n; i++) {
		if (0 != pthread_cond_destroy(&array[i].on_access))
			syserr("pthread_cond_destroy for %d", i);
	}
	pthread_mutex_unlock(&mut_globals);
	if (0 != pthread_mutex_destroy(&mut_globals))
		syserr("pthread_mutex_destroy(mut_globals)");

	// freeing allocated memory
	if (array) {
		free(array);
		array = NULL;
	}
}

/**
 * Main thread's handler for "hello" message from client
 * It spawns new thread, initializes resources for it etc.
 *
 * Requires mut_globals to be locked
 */
static
void main_handle_msg_hello(pid_t pid) {
	if (clients[pid].is_active)
		syserr("Client %d already marked as active.", pid);

	clients[pid].is_active = true;
	clients[pid].done = false;
	clients[pid].owned_cnt = 0;
	if (0 != pthread_cond_init(&clients[pid].on_message, NULL))
		syserr("pthread_cond_init");

	pthread_attr_t attr;
	if (0 != pthread_attr_init(&attr))
		syserr("pthread_attr_init");

	pid_t *arg = (pid_t *) malloc(sizeof(pid_t));
	if (!arg)
		syserr("malloc for thread arg");
	*arg = pid;

	workers_to_kill++;
	if (0 != pthread_create(&clients[pid].thread_id, &attr, worker_main, (void *) arg))
		syserr("pthread_create");
	if (0 != pthread_attr_destroy(&attr))
		syserr("pthread_attr_destroy");
}

/**
 * Passes message from main thread and worker thread
 * Requires mut_globals to be locked
 */
static
void main_pass_msg_to_thread(const msg_from_client *msg) {
	last_msg = *msg;
	pthread_cond_signal(&clients[last_msg.pid].on_message);
	log("main: hanging on on_last_msg_copied...");
	while (last_msg.pid)
		pthread_cond_wait(&on_last_msg_copied, &mut_globals);
	log("main: hanging on on_last_msg_copied ended");
}

/**
 * Helper method for parsing size_t value from char* string
 */
static
size_t parse_size(char *arg) {
	char *endptr;
	long lsize = strtol(arg, &endptr, 0);
	if (endptr == arg || *endptr != '\0' || ERANGE == errno)
		syserr("Cmdline arg conversion to int error: %d.", lsize);
	return (size_t) lsize;
}

static
void main_disconnect_clients() {
	for (int pid = 0; pid < NCLIENTS; pid++)
		if (clients[pid].is_active)
			server_disconnect_client(pid);
}

/**
 * Handler for ctrl-c
 */
static
void sigint_handler(int sig) {
	main_disconnect_clients();
	server_signal_termination(workers_to_kill);
}

/**
 * Helper function to override signal handler
 */
static
void main_override_sigint() {
	struct sigaction s;
	s.sa_handler = sigint_handler;
	sigemptyset(&s.sa_mask);
	s.sa_flags = SA_RESETHAND;
	if (-1 == sigaction(SIGINT, &s, 0))
		syserr("sigaction");
}

int main(int argc, char **argv) {
	if (argc != 2)
		syserr("Bad args: expected <N>");

	// globals initialization
	n = parse_size(argv[1]);
	array = calloc(n, sizeof(struct array_entry));

	// array access sync
	for (int i = 0; i < n; i++)
		if (0 != pthread_cond_init(&array[i].on_access, NULL))
			syserr("pthread_cond_init for %d", i);

	// messaging sync
	if (0 != pthread_mutex_init(&mut_globals, NULL))
		syserr("pthread_mutex_init(mut_globals)");

	// signals override
	main_override_sigint();

	// interprocess messaging initialization
	server_init();

	msg_from_client msg;
	while (true) {
		if (1 == server_getmsg(&msg)) // there's mess with pthread_mutex_[un]lock, because we can't have mutex here
			break;
		pthread_mutex_lock(&mut_globals);
		switch (msg.type) {
		case ArrayMsgtypeFromClientHello:
			main_handle_msg_hello(msg.pid);
			break;
		case ArrayMsgtypeFromClientGoodbye:
			main_handle_msg_goodbye(msg.pid);
			break;
		default:
			main_pass_msg_to_thread(&msg);
		}
		pthread_mutex_unlock(&mut_globals);
	}

	pthread_mutex_lock(&mut_globals);
	main_cleanup();
	return 0;
}
