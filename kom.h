#ifndef KOM_H_
#define KOM_H_

/**
 * Messaging device library for parallel math operation on int array.

 * 1 server, N clients. Max N may vary between implementations.

 * It may override SIGUSR1/SIGUSR2.
 *
 * It can't be initialized twice.
 */

#define	MSG_KEY_FROM_CLIENT						3164L
#define	MSG_KEY_FROM_SERVER						3165L
#define SHM_KEY									3166L
#define SEM_KEY1								"/kstanislawek_kom_h_1"
#define SEM_KEY2								"/kstanislawek_kom_h_2"
#define SEM_KEY3								"/kstanislawek_kom_h_3"
#define SEM_KEY4								"/kstanislawek_kom_h_4"


#include <sys/types.h>
#include <stdbool.h>

#define ARRAY_GET_LIMIT				9u  /* 1 needed for "set", 2 needed for "swap", 2-9 needed for "sum" */
#define ARRAY_SET_LIMIT				2u  /* 2 needed for "swap", 1 needed for "set" */

typedef int array_value_type;
typedef int array_index_type;

struct array_msg_from_client {
	enum msg_type_from_client {
		ArrayMsgtypeFromClientHello			= 11,
		ArrayMsgtypeFromClientGoodbye,
		ArrayMsgtypeFromClientGetvalues,
		ArrayMsgtypeFromClientSetvalues,
	} type;
	pid_t pid;
	union args_from_client {
		struct args_from_client_getvalues {
			int8_t count;
			_Bool lock; /* true => indices should be LOCKED for this client from now */
			_Bool rvlock; /* true => rv_index will be locked too */
			array_index_type indices[ARRAY_GET_LIMIT];
			array_index_type rv_index; /* important when rvlock is set to true */
		} getvalues;
		struct args_from_client_setvalues {
			int8_t count;
			array_index_type indices[ARRAY_SET_LIMIT];
			array_value_type values[ARRAY_SET_LIMIT];
		} setvalues;
	} args;
};

struct array_msg_from_server {
	enum msg_type_from_server {
		ArrayMsgtypeFromServerOk 			= 21,
		ArrayMsgtypeFromServerValues
	} type;
	struct args_from_server {
		struct args_from_server_values {
			int8_t count;
			array_value_type values[ARRAY_GET_LIMIT];
		} values;
	} args;
};

/**
 * Sends a message to server, waits for response and returns it.
 *
 * Blocking only if messaging device was not terminated.
 *
 * @param [in] msg - message to send
 * @param [out] resp - response
 * @returns 0 on no error, 1 when messaging device was terminated.
 */
int client_communicate(const struct array_msg_from_client *msg,
		struct array_msg_from_server *resp);

/**
 * Waits for a message from specific client.
 *
 * Blocking only if server_signal_termination was not called.
 *
 * Only main thread should use it.
 *
 * @param [in] pid - pid of client
 * @param [out] msg - message from client
 * @returns 0 on no error, 1 if server_signal_termination was called.
 */
int server_getmsg(struct array_msg_from_client *msg);

/**
 * Sends message to client.
 *
 * Might be blocking a bit, because it hangs on mutex that guards access to the shared memory
 *  and other semaphore that synchronizes with client, but with correct implementation of client
 *  and server, it should be only as long as synchronization demands.
 *
 * Not blocking if server_signal_termination was called (but it doesn't say whether it was or not).
 *
 * Multiple threads can use it.
 *
 * @param [in] pid - pid of client
 * @param [out] msg - message to client
 */
int server_sendmsg(pid_t pid, const struct array_msg_from_server *msg);

/**
 * Finalizes communication with the server.
 *
 * Not required if client_communicate(...) returned 1.
 */
void client_disconnect_server(pid_t pid);

/**
 * It tells the specific client about termination of the device.
 *
 * Some implementations might not require calling it.
 *
 * If it's required, it should be called before server_disconnect_client
 * 	for every remaining client.
 *
 * Safe to call in signal handler.
 *
 * @param [in] pid - pid of client
 * @returns 0 on no error, 1 when client's not alive, -1 on other errors
 */
int server_disconnect_client(pid_t pid);

/**
 * Informs library that it should terminate itself anytime soon.
 *
 * Server will know that termination happened through return value of server_getmsg,
 *  and client, analogically, through client_communicate.
 *
 * Safe to call in signal handler.
 */
void server_signal_termination();

/**
 * If there are any resources on client's side, this function frees it.
 *
 * Should be called only when client_communicate(...) returned 1;
 * 	client_disconnect_server(...) already calls it.
 */
void client_cleanup();

/**
 * Cleans resources that couldn't be cleaned in server_signal_termination().
 *
 * Not safe to call in signal handler.
 */
void server_cleanup();

/**
 * Initializes all things.
 */
void server_init();

/**
 * Initializes communication with the server.
 *
 * Might override SIGUSR1 and SIGUSR2 handlers.
 *
 * Server must be alive, or function will abort.
 */
void client_init(pid_t pid);


#endif /* KOM_H_ */









