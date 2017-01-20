/*
 * kom-m.c
 *
 * Implements kom.h
 */


#include <assert.h>
#include <errno.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

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

static int msg_id_from_server = -1;
static int msg_id_from_client = -1;

#define MSGTYPE_SERVER			1

// TODO typedefs and shorter aliases for enums (macros)
// TODO rename 'msgtype' to 'msgcategory' to avoid confusion
// TODO clean resources on error

struct array_full_msg {
	long msg_type;
	union body {
		struct array_msg_from_client from_client;
		struct array_msg_from_server from_server;
	} body;
};

static
int init_msgqueue_(int msg_key, int *msg_id, int flags, const char *contextmsg) {
	if (-1 == (*msg_id = msgget(msg_key, flags))) {
		if (EIDRM == errno || EINTR == errno || ENOENT == errno)
			return 1;
		syserr("%s", contextmsg);
	}
	return 0;
}

static
int create_msgqueue_(int msg_key, int *msg_id, const char *contextmsg) {
	return init_msgqueue_(msg_key, msg_id, 0666 | IPC_CREAT, contextmsg);
}

void server_init() {
	if (1 == create_msgqueue_(MSG_KEY_FROM_CLIENT, &msg_id_from_client, "create queue from_client"))
		syserr("from_client");
	if (1 == create_msgqueue_(MSG_KEY_FROM_SERVER, &msg_id_from_server, "create queue from_server"))
		syserr("from_server");
	log("server init(from_client=%d,from_server=%d)", msg_id_from_client, msg_id_from_server);
}

static
int attach_msgqueue_(int msg_key, int *msg_id, const char *contextmsg) {
	return init_msgqueue_(msg_key, msg_id, IPC_EXCL, contextmsg);
}

static
int sendmsg_(int qid, int msgtype, const struct array_msg_from_client *from_client,
		const struct array_msg_from_server *from_server, const char *contextmsg) {
	assert((from_client == NULL) != (from_server == NULL));

	struct array_full_msg fullmsg = { .msg_type = msgtype };

	if (from_client)
		fullmsg.body.from_client = *from_client;
	else
		fullmsg.body.from_server = *from_server;

	if (0 != msgsnd(qid, (void *) &fullmsg, sizeof(fullmsg.body), 0)) {
		if (EIDRM == errno || EINTR == errno)
			return 1;
		syserr("%s", contextmsg);
	}
	return 0;
}

static
int recvmsg_(int qid, int msgtype, struct array_msg_from_client *from_client,
		struct array_msg_from_server *from_server, const char *contextmsg) {
	assert((from_client == NULL) != (from_server == NULL));

	struct array_full_msg fullmsg;

	if (0 >= msgrcv(qid, (void *) &fullmsg, sizeof(fullmsg.body), msgtype, 0)) {
		if (EIDRM == errno || EINTR == errno)
			return 1;
		syserr("%s", contextmsg);
	}

	if (from_client)
		*from_client = fullmsg.body.from_client;
	else
		*from_server = fullmsg.body.from_server;

	return 0;
}

void client_init(pid_t pid) {
	if (1 == attach_msgqueue_(MSG_KEY_FROM_CLIENT, &msg_id_from_client, "from_client"))
		syserr("from_client");
	if (1 == attach_msgqueue_(MSG_KEY_FROM_SERVER, &msg_id_from_server, "from_server"))
		syserr("from_server");

	struct array_msg_from_client msg = {
			.pid = pid,
			.type = ArrayMsgtypeFromClientHello
	};
	struct array_msg_from_server resp;
	if (1 == client_communicate(&msg, &resp))
		syserr("communicate");
	if (ArrayMsgtypeFromServerOk != resp.type)
		syserr("Bad response for Hello");

	log("client_init(from_client=%d,from_server=%d)", msg_id_from_client, msg_id_from_server);
}

int server_getmsg(struct array_msg_from_client *msg) {
	return recvmsg_(msg_id_from_client, MSGTYPE_SERVER, msg, NULL, "get some msg");
}

static
int server_clean_(int *qid) {
	if (-1 != *qid) {
		if (-1 == msgctl(*qid, IPC_RMID, 0)) {
			if (EIDRM == errno || ENOENT == errno)
				return 1;
			syserr("msgctl"); // TODO it shouldn't be here
		}
		*qid = -1;
	}
	return 0;
}

void client_cleanup() {
}

int server_disconnect_client(pid_t pid) {
	return 0;
}

void server_signal_termination() {
	int r1 = server_clean_(&msg_id_from_server);
	int r2 = server_clean_(&msg_id_from_client);
	if (r1 || r2)
		syserr("Something went wrong, but not very wrong");
}

void server_cleanup() {
	if (msg_id_from_client != -1)
		syserr("serwer already cleaned up");
}

void client_disconnect_server(pid_t pid) {
	if (-1 == msg_id_from_client)
		return;

	struct array_msg_from_client msg = {
			.pid = pid,
			.type = ArrayMsgtypeFromClientGoodbye
	};
	sendmsg_(msg_id_from_client, MSGTYPE_SERVER, &msg, NULL, "Client's goodbye");

	msg_id_from_client = -1;
	msg_id_from_server = -1;
}

int server_sendmsg(pid_t pid, const struct array_msg_from_server *msg) {
	return sendmsg_(msg_id_from_server, pid, NULL, msg, "server sends msg");
}

int client_communicate(const struct array_msg_from_client *msg,
		struct array_msg_from_server *resp) {
	if (1 == sendmsg_(msg_id_from_client, MSGTYPE_SERVER, msg, NULL, "client sends msg"))
		return 1;
	if (1 == recvmsg_(msg_id_from_server, msg->pid, NULL, resp, "client receives msg"))
		return 1;

	log("client_communicate: sent type %d, received type %d", msg->type, resp->type);
	return 0;
}
