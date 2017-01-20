/*
 * kom-s.c
 *
 * Implements kom.h using shared memory & semaphores
 */
#include <assert.h>
#include <errno.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <semaphore.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>


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

union semun {
  int val;						/* value for SETVAL                */
  struct semid_ds *buf;			/* buffer for IPC_STAT & IPC_SET   */
  unsigned short int *array;	/* array for GETALL & SETALL       */
  struct seminfo *__buf;		/* buffer for IPC_INFO             */
};


static int shm_id = -1;
static sem_t on_threads; // local, server-only
static sem_t *on_client;
static sem_t *on_client_resp;
static sem_t *on_server;
static sem_t *on_memory;
static bool sigusr1_got = false, sigusr2_got = false; // client-only
static bool terminating = false;	  // server-only

struct array_full_shm {
	union array_msg_union {
		struct array_msg_from_client from_client;
		struct array_msg_from_server from_server;
	} msg;
} *shm_p;

#define SHM_SIZE 			sizeof(struct array_full_shm)

int sem_wait_(sem_t *sem) {
	if (-1 == sem_wait(sem)) {
		if (EINTR == errno)
			errno = 0;
		else
			return -1;
	}
	return 0;
}

int sem_post_(sem_t *sem) {
	if (-1 == sem_post(sem)) {
		if (EINTR == errno)
			errno = 0;
		else
			return -1;
	}
	return 0;
}


void client_send_(const struct array_msg_from_client *msg) {
	log("[%d]: sending the message type %d", msg->pid, msg->type);
	if (-1 == sem_wait_(on_memory))
		syserr("sem_wait_ on_memory");
	log("[%d]: step 2/5", msg->pid);
	shm_p->msg.from_client = *msg;
	if (-1 == sem_post_(on_client))
		syserr("sem_post_ on_client");
	log("[%d]: step 3/5", msg->pid);
	if (-1 == sem_wait_(on_server))
		syserr("sem_wait_ on_server");
	log("[%d]: step 4/5", msg->pid);
	if (-1 == sem_post_(on_memory))
		syserr("sem_post_ on_memory");
	log("[%d]: sending finished", msg->pid);
}

/**
 * 1 indicates that it's cleanup time (as always)
 * 0 that everything's OK
 * syserr() on error as always
 */
static
int client_signalwait_() {
	sigset_t new_blocked_signals;
	if (-1 == sigprocmask(0, NULL, &new_blocked_signals))
		syserr("sigprocmask (getting blocked signals");
	if (-1 == sigdelset(&new_blocked_signals, SIGUSR1))
		syserr("sigdelset %d", SIGUSR1);
	if (-1 == sigdelset(&new_blocked_signals, SIGUSR2))
		syserr("sigdelset %d", SIGUSR2);
	// TODO we could easily add CTRL-C here, and it would work as a notification from server and would let us safely quit

	sigsuspend(&new_blocked_signals);
	if (EINTR != errno)
		syserr("sigsuspend");
	errno = 0;

	if (sigusr1_got) {
		sigusr1_got = false;
		return 0;
	}
	if (!sigusr2_got)
		log("[pid=%d] Unrecognized signal. Gonna pretend that it's terminating signal from server.", getpid());
	return 1;
}

int client_communicate(const struct array_msg_from_client *msg,
		struct array_msg_from_server *resp) {
	client_send_(msg);
	log("[%d]: waiting for response", msg->pid);
	if (1 == client_signalwait_())
		return 1;
	log("[%d]: step 2/4", msg->pid);
	if (-1 == sem_wait_(on_server))
		syserr("sem_wait_ on_server");
	log("[%d]: step 3/4", msg->pid);
	*resp = shm_p->msg.from_server;
	if (-1 == sem_post_(on_client_resp))
		syserr("sem_post_ on_client_resp");
	log("[%d]: sent type %d, received type %d", msg->pid, msg->type, resp->type);
	return 0;
}

int server_getmsg(struct array_msg_from_client *msg) {
	log("[server] getting the message");
	if (-1 == sem_wait_(on_client))
		syserr("sem_wait_ on_client");
//	if (-1 == sem_wait_(&on_threads))
//		syserr("sem_wait_ on_threads");
	*msg = shm_p->msg.from_client;
	if (terminating)
		return 1;
	log("[server]: step 2/3");
//	if (-1 == sem_post_(&on_threads))
//		syserr("sem_post_ on_threads");
	if (-1 == sem_post_(on_server))
		syserr("sem_post_ on_server");
	log("[server]: got type %d from %d", msg->type, msg->pid);
	return 0;
}

int server_sendmsg(pid_t pid, const struct array_msg_from_server *msg) {
	log("[server] sending the message to %d", pid);
	if (-1 == sem_wait_(&on_threads))
		syserr("sem_wait_ on_threads");
	log("[server]: step 2/8");
	if (-1 == sem_wait_(on_memory))
		syserr("sem_wait_ on_memory");
	shm_p->msg.from_server = *msg;
	log("[server]: step 3/8");
	if (-1 == sem_post_(on_server))
		syserr("sem_post_ on_server");
	log("[server]: step 4/8");
	if (-1 == kill(pid, SIGUSR1)) {
		if (ESRCH == errno)
			return 1;
		syserr("kill");
	}
	log("[server]: step 5/8");
	if (-1 == sem_wait_(on_client_resp))
		syserr("sem_wait_ on_client_resp");
	log("[server]: step 6/8");
	if (-1 == sem_post_(on_memory))
		syserr("sem_post_ on_memory");
	log("[server]: step 7/8");
	if (-1 == sem_post_(&on_threads))
		syserr("sem_post_ on_threads");
	log("[server]: sent type %d to %d", msg->type, pid);
	return 0;
}

/**
 * Only needed when server doesn't disconnect first
 */
void client_disconnect_server(pid_t pid) {
	if (-1 == shm_id)
		return;
	const struct array_msg_from_client msg = {
			.pid = pid,
			.type = ArrayMsgtypeFromClientGoodbye
	};
	client_send_(&msg);
	shm_id = -1;
}

int server_disconnect_client(pid_t pid) {
//	log("[server] Killing %d...", pid);
	if (-1 == kill(pid, SIGUSR2)) {
		if (ESRCH == errno) {
//			log("[server] ESRCH, which means that %d was already dead.", pid);
			return 1;
		}
		return -1;
//		syserr("kill %d", pid);
	}
//	log("[server] %d killed.", pid);
	return 0;
}

//
// TODO ignorowac EINTR WSZEDZIE
//


void server_signal_termination(size_t clients_count) {
	// let's remember that we assume that all clients already got notified, thanks to
	//  server_disconnect_client method

	// let's raise all semaphores, so that noone will be blocked until they get noticed about termination

	// 1 for every client and his worker, including potential new client that is not included in "clients_count" yet
	const size_t c_on_memory = 2 * (clients_count + 1) + 1;
	// only server threads hang on it, so excluding clients
	const size_t c_on_client = clients_count + 2;
	const size_t c_on_client_resp = c_on_client;
	const size_t c_on_threads = c_on_client;
	// only clients hang on it, so excluding server threads
	const size_t c_on_server = clients_count + 1;

	sem_t* semaphores[] = { on_memory, on_client, on_client_resp, &on_threads, on_server };
	size_t counts[] = {c_on_memory, c_on_client, c_on_client_resp, c_on_threads, c_on_server };
	for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
		for (size_t j = 0; j < counts[i]; j++)
			if (-1 == sem_post(semaphores[i]))
				syserr("sem_post: i=%d, j=%d", i, j); // TODO yes I know, this shouldn't be in signal handler, according to signal(7)

	terminating = true;
}

/**
 * TODO it's practically not required because everything is closed on terminate and cleaned by server,
 * BUT it would be good to have it forceable without terminating client's program
 */
void client_cleanup(pid_t pid) {
}

void server_cleanup() {
	if (-1 != shm_id) {
		// we can just remove shm here, because it's not going to be actually removed
		// before all processes detach from it
		if (-1 == shmctl(shm_id, IPC_RMID, 0))
			syserr("shmctl(IPC_RMID)");

		// the same about semaphores
		if (-1 == sem_unlink(SEM_KEY1))
			syserr("sem_unlink 1");
		if (-1 == sem_unlink(SEM_KEY2))
			syserr("sem_unlink 2");
		if (-1 == sem_unlink(SEM_KEY3))
			syserr("sem_unlink 3");
		if (-1 == sem_unlink(SEM_KEY4))
			syserr("sem_unlink 4");
		if (-1 == sem_destroy(&on_threads))
			syserr("sem_destroy");

		shm_id = -1;
	}
}

// TODO porzadne czyszczenie zasobów, żeby "-1" nie powodowało, że zostawiamy śmieci, np. segmenty pamięci w systemie
// np. usuniecie pamieci dzielonej etc
void server_init() {
	// TODO mayble we should put sem_wait_ in while()

	if (-1 == (shm_id = shmget(SHM_KEY, SHM_SIZE, 0666 | IPC_CREAT)))
		syserr("shmget");
	if ((void*)-1 == (void*)(shm_p = (struct array_full_shm*)shmat(shm_id, NULL, 0)))
		syserr("shmat");

	if (NULL == (on_memory = sem_open(SEM_KEY1, O_CREAT | O_EXCL, 0644, 1)))
		syserr("sem_open(on_memory)");
	if (NULL == (on_server = sem_open(SEM_KEY2, O_CREAT | O_EXCL, 0644, 0)))
		syserr("sem_open(on_server)");
	if (NULL == (on_client = sem_open(SEM_KEY3, O_CREAT | O_EXCL, 0644, 0)))
		syserr("sem_open(on_client)");
	if (NULL == (on_client_resp = sem_open(SEM_KEY4, O_CREAT | O_EXCL, 0644, 0)))
		syserr("sem_open(on_client_resp)");
	if (-1 ==  sem_init(&on_threads, 0, 1))
		syserr("sem_open(on_threads)");

	log("server init(shm_id=%d)", shm_id);
}

/*** sig initialization helper functions ***/
static
void client_sighandler_sigusr1(int sig) {
	sigusr1_got = true;
}

static
void client_sighandler_sigusr2(int sig) {
	sigusr2_got = true;
}

static
void client_init_signals_addhandler(int sig, void (*handler)(int)) {
	struct sigaction act = { .sa_handler = handler };
	if (-1 == sigemptyset(&act.sa_mask))
		syserr("sigemptyset (lol)");
	if (-1 == sigaction(sig, &act, NULL))
		syserr("sigaction sig=%d", sig);
}

static
void client_init_signals_block() {
	sigset_t new_blocked_signals;
	if (-1 == sigaddset(&new_blocked_signals, SIGUSR1))
		syserr("sigaddset 1");
	if (-1 == sigaddset(&new_blocked_signals, SIGUSR2))
		syserr("sigaddset 2");
	if (-1 == sigprocmask(SIG_BLOCK, &new_blocked_signals, NULL))
		syserr("sigprocmask");
}
/*** end ***/

void client_init(pid_t pid) {
	sigusr1_got = false;
	sigusr2_got = false;

	if (-1 == (shm_id = shmget(SHM_KEY, SHM_SIZE, IPC_EXCL)))
		syserr("shmget");
	if ((void*)-1 == (void*)(shm_p = (struct array_full_shm*)shmat(shm_id, NULL, 0)))
		syserr("shmat");

	if (NULL == (on_memory = sem_open(SEM_KEY1, 0)))
		syserr("sem_open(on_memory)");
	if (NULL == (on_server = sem_open(SEM_KEY2, 0)))
		syserr("sem_open(on_server)");
	if (NULL == (on_client = sem_open(SEM_KEY3, 0)))
		syserr("sem_open(on_client)");
	if (NULL == (on_client_resp = sem_open(SEM_KEY4, 0)))
		syserr("sem_open(on_client_resp)");
	log("client init(pid=%d, shm_id=%d)", pid, shm_id);

	client_init_signals_block();
	client_init_signals_addhandler(SIGUSR1, client_sighandler_sigusr1);
	client_init_signals_addhandler(SIGUSR2, client_sighandler_sigusr2);

	// TODO lines below might as well be moved out of the library
	struct array_msg_from_client msg = { .pid = pid, .type = ArrayMsgtypeFromClientHello };
	struct array_msg_from_server resp;
	if (-1 == client_communicate(&msg, &resp))
		return;
	if (ArrayMsgtypeFromServerOk != resp.type)
		syserr("Bad response for Hello");
	log("[pid=%d]: communicated with server", pid);
}
