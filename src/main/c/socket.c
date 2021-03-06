/**
 *
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#ifdef LINUX
#include <sys/socket.h>  // To please Eclipse
#endif

#define APR_DECLARE_STATIC
#include "apr_general.h"
#include "apr_file_io.h"
#include "apr_strings.h"
#include "apr_network_io.h"
#include "apr_poll.h"

#include "codes.h"
#include "server.h"
#include "client.h"
#include "serialize.h"

// Default listen port number
#define DEF_LISTEN_PORT		7876

// Default socket backlog number. SOMAXCONN is a system default value.
#define DEF_SOCKET_BACKLOG	SOMAXCONN

// TODO Comment
#define DEF_POLLSET_NUM		32

// Default socket timeout
#define DEF_POLL_TIMEOUT	(APR_USEC_PER_SEC * 30)

// Default buffer size
typedef struct _serviceContext_t serviceContext_t;

// Network event callback function type
typedef int (*socket_callback_t)(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset);

// Service context
struct _serviceContext_t {
	enum {
		RECV_MESSAGE, SEND_MESSAGE,
	} status;
	apr_socket_t *socket;
	socket_callback_t cbFunc;
	apr_pool_t *serverTxPool;
	apr_pool_t *clientTxPool;
	apr_thread_cond_t *clientCond;
	apr_thread_mutex_t *clientMutex;
	clientTransaction_t *clientTx;
	serverTransaction_t *serverTx;
};

static serviceContext_t *context;

static apr_socket_t* createListenSocket(apr_pool_t *listenPool);
static int doAccept(apr_pollset_t *pollset, apr_socket_t *lsock, apr_pool_t *socketPool);
static int receiveMessage(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset);
static int sendResponse(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset);
static int receiveData(char *portId, char *buf, int len);

int poll(apr_pool_t * socketPool, apr_pollset_t *pollset, apr_socket_t *lsock) {
	apr_status_t rv;
	int r;
	apr_int32_t num;
	const apr_pollfd_t *descriptors;

	while (TRUE) {
		rv = apr_pollset_poll(pollset, DEF_POLL_TIMEOUT, &num, &descriptors);
		if (rv == APR_SUCCESS) {
			int i;
			assert(num > 0);
			/* scan the active sockets */
			for (i = 0; i < num; i++) {
				if (descriptors[i].desc.s == lsock) {
					/* the listen socket is readable. that indicates we accepted a new connection */
					doAccept(pollset, lsock, socketPool);
				} else {
					serviceContext_t *context = descriptors[i].client_data;
					socket_callback_t cbFunc = context->cbFunc;
					r = cbFunc(socketPool, context, pollset);
					if(r == R_SHUTDOWN_REQUESTED) { 
						return r;
					} 
				}
			}
		} else {
//			char errBuf[256];
//			printf("apr_pollset_poll() returned %d, '%s'\n", rv, apr_strerror(rv, errBuf, 256));
			if(rv != APR_TIMEUP) return R_INTERN_ERROR;
		}
	}
}

/**
 *
 */
int runServer() {
	apr_pool_t *listenPool, *socketPool;
	apr_socket_t *lsock;/* listening socket */
	apr_pollset_t *pollset;

	apr_pool_create(&listenPool, NULL);
	apr_pool_create(&socketPool, NULL);

	lsock = createListenSocket(listenPool);

	apr_pollset_create(&pollset, DEF_POLLSET_NUM, listenPool, 0);
	{
		// Monitor with pollset the listen socket can read without blocking
		apr_pollfd_t pfd = { listenPool, APR_POLL_SOCKET, APR_POLLIN, 0, { NULL }, NULL };
		pfd.desc.s = lsock;
		apr_pollset_add(pollset, &pfd);
	}

	poll(socketPool, pollset, lsock);

	apr_pollset_destroy(pollset);
	apr_pool_destroy(socketPool);
	apr_pool_destroy(listenPool);
	return R_SUCCESS;
}

static apr_socket_t* createListenSocket(apr_pool_t *listenPool) {
	apr_status_t rv;
	apr_socket_t *s;
	apr_sockaddr_t *sa;

	rv = apr_sockaddr_info_get(&sa, NULL, APR_INET, DEF_LISTEN_PORT, 0, listenPool);
	if (rv != APR_SUCCESS) {
		goto error;
	}

	rv = apr_socket_create(&s, sa->family, SOCK_STREAM, APR_PROTO_TCP, listenPool);
	if (rv != APR_SUCCESS) {
		goto error;
	}

	/* non-blocking socket */
	apr_socket_opt_set(s, APR_SO_NONBLOCK, 1);
	apr_socket_timeout_set(s, 0);
	apr_socket_opt_set(s, APR_SO_REUSEADDR, 1);/* this is useful for a server(socket listening) process */

	rv = apr_socket_bind(s, sa);
	if (rv != APR_SUCCESS) {
		goto error;
	}
	rv = apr_socket_listen(s, DEF_SOCKET_BACKLOG);
	if (rv != APR_SUCCESS) {
		goto error;
	}

	return s;

	error: return NULL;
}

static int doAccept(apr_pollset_t *pollset, apr_socket_t *lsock, apr_pool_t *socketPool) {
	apr_pollfd_t descriptor = { socketPool, APR_POLL_SOCKET, APR_POLLIN, 0, { NULL }, NULL };

	// Create service context
	context = (serviceContext_t *) apr_palloc(socketPool, sizeof(serviceContext_t));
	apr_socket_accept(&context->socket, lsock, socketPool);
	descriptor.client_data = context;
	descriptor.desc.s = context->socket;
	context->status = RECV_MESSAGE;
	context->cbFunc = receiveMessage;

	// Create new transaction pools
	apr_pool_create(&context->serverTxPool, socketPool);
	apr_pool_create(&context->clientTxPool, socketPool);
	context->serverTx = NULL;
	context->clientTx = NULL;

	// Create client sync objects
	apr_thread_cond_create(&context->clientCond, socketPool);
	apr_thread_mutex_create(&context->clientMutex, APR_THREAD_MUTEX_DEFAULT, socketPool);

	// Blocking socket. Blocking timeout = 1s
	apr_socket_opt_set(context->socket, APR_SO_NONBLOCK, 0);
	apr_socket_timeout_set(context->socket, 1000000);

	/* monitor accepted socket */
	apr_pollset_add(pollset, &descriptor);
	return TRUE;
}

void closeSocket(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset, apr_int16_t reqevents) {
	// Stop monitoring 
	if(pollset != NULL) {
		apr_pollfd_t descriptor = { socketPool, APR_POLL_SOCKET, reqevents, 0, { NULL }, context };
		descriptor.desc.s = context->socket;
		apr_pollset_remove(pollset, &descriptor);
	}

	// Close socket
	apr_socket_close(context->socket);

	// Free associateded sync objects
	apr_thread_mutex_destroy(context->clientMutex);
	apr_thread_cond_destroy(context->clientCond);

	// Free socket pool
	apr_pool_clear(socketPool);
	context = NULL;
	printf("Closed socket\n");
}

int writeMessage(apr_socket_t *sock, message_t *message) {
	CHECK(writeHeader(sock, message));
	switch (message->code) {
	case ACK:
		CHECK(writeInt32(sock, &message->fields[0]))
		break;
	case NOTIFY:
		CHECK(writeString(sock, &message->fields[0]));
		CHECK(writeOctetString(sock, &message->fields[1]));
		break;
	}

	return R_SUCCESS;
}

static int receiveRequest(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset, char code) {
	int r;
	apr_pollfd_t descriptor = { socketPool, APR_POLL_SOCKET, APR_POLLIN, 0, { NULL }, context };

	// Create a new server transaction
	// TODO what happens if a transaction was already created?
	createServerTransaction(context->serverTxPool, &context->serverTx, receiveData);

	r = operateRequest(context->socket, context->serverTx, context->serverTxPool, code);

	// Stop monitoring requests 
	descriptor.desc.s = context->socket;
	apr_pollset_remove(pollset, &descriptor);

	// Check request was correctly operated
	if (r != R_SUCCESS) {
		closeSocket(socketPool, context, NULL, 0);
		return r;
	}

	// Start monitoring responses to send
	descriptor.reqevents = APR_POLLOUT;
	apr_pollset_add(pollset, &descriptor);
	context->status = SEND_MESSAGE;
	context->cbFunc = sendResponse;
	return R_SUCCESS;
}

static int receiveResponse(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset, char code) {
	int r;
	// Check if a transaction is running
	if (context->clientTx == NULL)
		return R_TX_NOT_FOUND;

	// Read response
	r = operateResponse(context->socket, context->clientTx, context->clientTxPool, code);
	if (r != R_SUCCESS) {
		closeSocket(socketPool, context, pollset, APR_POLLIN);
		return r;
	}

	// Synchronize with waiting client
	apr_thread_mutex_lock(context->clientMutex);
	apr_thread_cond_signal(context->clientCond);
	apr_thread_mutex_unlock(context->clientMutex);

	// Clear transaction
	clearClientTransaction(context->clientTxPool, &context->clientTx);
	return R_SUCCESS;
}

int receiveMessage(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset) {
	char code;
	messageTxType_t txType;
	int r;

	// Read message header
	r = checkInputMessage(context->socket, &code, &txType);
	if (r != R_SUCCESS) {
		closeSocket(socketPool,  context, pollset, APR_POLLIN);
		return r;
	}

	// Handle rest of message according to its transaction type
	if (txType == SERVER_TX) {
		return receiveRequest(socketPool, context, pollset, code);
	} else {
		return receiveResponse(socketPool, context, pollset, code);
	}
}

/**
 * Send a response to the client.
 */
static int sendResponse(apr_pool_t *socketPool, serviceContext_t *context, apr_pollset_t *pollset) {
	int out = R_SUCCESS;
	apr_pollfd_t descriptor = { socketPool, APR_POLL_SOCKET, APR_POLLOUT, 0, { NULL }, context };
	writeMessage(context->socket, context->serverTx->response);

	// Check if shutdown was requested
	if(context->serverTx->shutdown == TRUE) out = R_SHUTDOWN_REQUESTED;

	// Clear all memory related to request and response
	clearServerTransaction(context->serverTxPool, &context->serverTx);

	// Change context status from write to read
	descriptor.desc.s = context->socket;
	apr_pollset_remove(pollset, &descriptor);
	descriptor.reqevents = APR_POLLIN;
	apr_pollset_add(pollset, &descriptor);
	context->status = RECV_MESSAGE;
	context->cbFunc = receiveMessage;

	return out;
}

int receiveData(char *portId, char *buf, int len) {
	int r;
	if (context->clientTx != NULL)
		return R_TX_RUNNING;
	createClientTransaction(context->clientTxPool, &context->clientTx);

	CHECK(operatePortData(context->clientTx, context->clientTxPool, portId, buf, len))

	// Send message and synchronize with response if message sent
	apr_thread_mutex_lock(context->clientMutex);
	r = writeMessage(context->socket, context->clientTx->request);
	if (r == R_SUCCESS) {
		apr_thread_cond_timedwait(context->clientCond, context->clientMutex, 2000000);
	}
	apr_thread_mutex_unlock(context->clientMutex);
	return r;
}
