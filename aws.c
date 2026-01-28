// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#define MAX_EVENTS 40

static const char IP[] = "127.0.0.1";

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

/* ctx used for asynchronous I/O */
static io_context_t ctx;

/**
 * Callback used by the parser to write the request_path into the connection
 * and the have_path variable
 */
static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

/**
 * Prepare send_buffer by placing the header inside it and setting
 * the connection to STATE_SENDING_HEADER
 */
static void connection_prepare_send_reply_header(struct connection *conn)
{
	char *reply_header = "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n";

	DIE(strlen(reply_header) > BUFSIZ, "BUFSIZ too small");
	sprintf(conn->send_buffer, reply_header, conn->file_size);
	conn->send_len = strlen(conn->send_buffer);
	conn->state = STATE_SENDING_HEADER;
}

/**
 * Initialize the send_buffer of the connection to the 404 error
 */
static void connection_prepare_send_404(struct connection *conn)
{
	char *error_404 = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
	int len = strlen(error_404);

	DIE(len > BUFSIZ, "not enough space in buffer");

	strcpy(conn->send_buffer, error_404);
	conn->send_len = len;
	conn->state = STATE_SENDING_404;
}

/**
 * If the requested file doesn't exist or isn't in one of the static or dynamic folder
 * the function returns RESOURCE_TYPE_NONE.
 * Otherwise it places the name of the filename in conn->filename and returns
 * RESOURCE_TYPE_STATIC or RESOURCE_TYPE_DYNAMIC
 */
static enum resource_type connection_get_resource_type(struct connection *conn)
{
	// Check file exists
	char cwd[BUFSIZ];
	int result = 0;

	DIE(getcwd(cwd, sizeof(cwd)) == NULL, "GETCWD");

	strcat(cwd, conn->request_path);
	result = access(cwd, F_OK);
	if (result != 0)
		return RESOURCE_TYPE_NONE;

	if (strncmp(conn->request_path, "/static/", 8) == 0) {
		snprintf(conn->filename, BUFSIZ, "%s%s", AWS_ABS_STATIC_FOLDER, conn->request_path + 8);
		return RESOURCE_TYPE_STATIC;
	}
	if (strncmp(conn->request_path, "/dynamic/", 9) == 0) {
		snprintf(conn->filename, BUFSIZ, "%s%s", AWS_ABS_DYNAMIC_FOLDER, conn->request_path + 9);
		return RESOURCE_TYPE_DYNAMIC;
	}
	return RESOURCE_TYPE_NONE;
}

// Initializes a connection structure on the given socket
struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(struct connection));

	if (conn == NULL)
		return NULL;
	memset(conn, 0, sizeof(struct connection));
	conn->sockfd = sockfd;
	conn->state = STATE_INITIAL;
	return conn;
}

/**
 * This function initiatess the asynchronous reading from the requested file
 */
void connection_start_async_io(struct connection *conn)
{
	int ret = 0;

	memset(&conn->iocb, 0, sizeof(conn->iocb));
	io_prep_pread(&(conn->iocb), conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);
	conn->piocb[0] = &conn->iocb;
	// Start async reading
	ret = io_submit(ctx, 1, conn->piocb);
	DIE(ret < 0, "io_submit failure");
	conn->state = STATE_ASYNC_ONGOING;
}

// Remove the connection from epoll
void connection_remove(struct connection *conn)
{
	DIE(epoll_ctl(epollfd, EPOLL_CTL_DEL, conn->sockfd, NULL) == -1, "epoll_ctl fail");
	close(conn->sockfd);
	free(conn);
}

/**
 * This function creates a new connection on the listening socket,
 * sets the newly created socket to non-blocking mode, initializes the
 * HTTP request parser, and adds the client socket to epoll for
 * multiplexing the I/O events.
 */
void handle_new_connection(void)
{
	struct sockaddr_in client_addr;
	struct epoll_event event;
	struct connection *conn;
	int clientfd = 0, flags = 0;

	socklen_t client_len = sizeof(client_addr);

	clientfd = accept(listenfd, (struct sockaddr *)&client_addr, &client_len);
	DIE(clientfd < 0, "accept error");

	flags = fcntl(clientfd, F_GETFL, 0);
	DIE(flags == -1, "fcntl fail");

	DIE(fcntl(clientfd, F_SETFL, flags | O_NONBLOCK) < 0, "FCTNL ERROR");

	/* Instantiate new connection handler. */
	conn = connection_create(clientfd);
	DIE(conn == NULL, "connection_creation error");

	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;

	/* Add socket to epoll. */
	memset(&event, 0, sizeof(struct epoll_event));
	event.events = EPOLLIN; // All data must be emptied when notification ocurrs
	event.data.ptr = conn;
	DIE(epoll_ctl(epollfd, EPOLL_CTL_ADD, clientfd, &event) == -1, "epoll_ctl fail");
}

// If conn would be a blockable type, it would block.
// Treat the error as though it has finished the receiving process
void handle_recv_error(struct connection *conn)
{
	if (errno == EWOULDBLOCK || errno == EAGAIN) {
		conn->state = STATE_REQUEST_RECEIVED;
		return;
	}
	ERR("other error not handled yet");
	exit(-1);
}

// Helper function
void receiving_ended(ssize_t bytes, struct connection *conn)
{
	if (bytes < 0)
		handle_recv_error(conn);
	else
		conn->state = STATE_REQUEST_RECEIVED;
}

/**
 * Receives data, not all at once, state changes to STATE_REQUEST_RECEIVED
 * upon completion, inside the helper function receiving_ended
 * Firstly the actual message is getting the data, and then the next data gets
 * peeked at without emptying the buffer to change the state since the event
 * doesn't trigger anymore if the buffer was emptied.
 */
void receive_data(struct connection *conn)
{
	ssize_t bytes_received = 0, peeked_bytes = 0;

	// Store message in recv_buffer in struct connection
	bytes_received = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
						  BUFSIZ - conn->recv_len, 0);
	if (bytes_received <= 0) {
		receiving_ended(bytes_received, conn);
		return;
	}
	conn->recv_len += bytes_received;
	peeked_bytes = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
						BUFSIZ - conn->recv_len, MSG_PEEK);
	if (peeked_bytes <= 0)
		receiving_ended(bytes_received, conn);
}

/**
 * Opens the field filename from the connection for reading,
 * also sets the file_size fields.
 * -1 is returned in case of failure and 0 on success
 */
int connection_open_file(struct connection *conn)
{
	struct stat file_stat;

	conn->fd = open(conn->filename, O_RDONLY);
	if (conn->fd < 0)
		return -1;

	DIE(fstat(conn->fd, &file_stat) < 0, "fstat failed");

	conn->file_size = file_stat.st_size;
	conn->file_pos = 0;
	return 0;
}

/* Parse the HTTP header and extract the file path. */
int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	ssize_t nparsed = http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	DIE(nparsed != conn->recv_len, "parsing error");

	http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
	if (conn->have_path)
		return 0;
	return -1;
}

/**
 * Uses sendfile as a zero-copy operation, if not all bytes are sent
 * the state is not changed and this function will keep getting called
 * untill all the bytes are sent.
 */
enum connection_state connection_send_static(struct connection *conn)
{
	ssize_t sent = sendfile(conn->sockfd, conn->fd, NULL, conn->file_size - conn->file_pos);

	DIE(sent < 0, "sendfile fail");
	conn->file_pos += sent;
	if (conn->file_pos >= conn->file_size)
		conn->state = STATE_DATA_SENT;
	return conn->state;
}

/* Send as much data as possible from the connection send buffer.
 * Returns the number of bytes sent or -1 if an error occurred
 */
int connection_send_data(struct connection *conn)
{
	int nr_bytes_to_send = conn->send_len - conn->send_pos;

	return send(conn->sockfd, conn->send_buffer + conn->send_pos, nr_bytes_to_send, 0);
}

/**
 * Send the data, update the send_position and if the entire buffer
 * was sent start another asynchronous reading sequence
 */
int connection_send_dynamic(struct connection *conn)
{
	ssize_t bytes_sent = connection_send_data(conn);

	DIE(bytes_sent < 0, "send_data error");
	conn->send_pos += bytes_sent;
	DIE(conn->send_pos > conn->send_len, "send_pos error");
	if (conn->send_pos == conn->send_len)
		connection_start_async_io(conn);
	return 0;
}

/**
 * Initializes the sending buffer on every sequence of asynchronous reading,
 * updates the file position and sets the state to send data.
 * If the bytes_read are 0, it means the file has finished being read and
 * the connection can be closed.
 */
int connection_init_dynamic_send(struct connection *conn)
{
	ssize_t bytes_read = 0;
	struct io_event event[1];
	int num_events;
	struct timespec timeout = {0, 0};

	// Using this timeout the function will not block.
	// Also, if no events are found this
	// function will be called again

	num_events = io_getevents(ctx, 1, 1, event, &timeout);
	DIE(num_events < 0, "error io_getevents");
	if (num_events == 0)
		return 0;

	bytes_read = event->res;
	if (bytes_read == 0) {
		// The reading of the entire file has finished
		conn->state = STATE_CONNECTION_CLOSED;
		return 0;
	}
	// Initiate the sending buffer and update the file position
	conn->file_pos += bytes_read;
	conn->send_len = bytes_read;
	conn->send_pos = 0;
	// Start sending the data
	conn->state = STATE_SENDING_DATA;
	return bytes_read;
}

/**
 * This function decides what should happen next with
 * the connection depending on what the request is.
 * If the request is bad (the file doesn't exist) the connection
 * is set to send a 404 error. Otherwise, the connection will next
 * send a reply header.
 * The socket is set to EPOLLOUT to prepare it for sending.
 */
void set_reply_type(struct connection *conn)
{
	int rc = 0;

	if (parse_header(conn) != 0) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}
	// Get resource type and check if file exists
	conn->res_type = connection_get_resource_type(conn);
	if (conn->res_type == RESOURCE_TYPE_NONE) {
		// File doesn't exist
		connection_prepare_send_404(conn);
	} else {
		DIE(connection_open_file(conn) != 0, "File exists but couldn't be opened");
		connection_prepare_send_reply_header(conn);
	}

	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "EPOLL UPDATE PTR OUR ERROR");
}

/**
 * This function handles the requests and receives the data.
 * set_reply_type is used to decide on how the connection continues
 * after all data is received
 */
void handle_input(struct connection *conn)
{
	switch (conn->state) {
	case STATE_INITIAL:
		conn->state = STATE_RECEIVING_DATA;
		// KEEP GOING TO NEXT STATE
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		if (conn->state == STATE_REQUEST_RECEIVED)
			set_reply_type(conn);
		break;
	default:
		ERR("Case not handled");
		exit(-1);
	}
}

/**
 * This handles the sending of data and sets the according next state.
 *
 * STATE_SENDING_HEADER will keep getting switched to untill all the header is sent,
 * then the state is changed to STATE_SENDING_DATA, and depending on the resource
 * type, the asynchronous io is started or the data is sent using a zero-copy mechanism.
 *
 * STATE_ASYNC_ONGOING is the state in which we check if the data has been read
 * asynchronously and prepare the socket for sending it
 *
 * STATE_SENDING_404 works the same way as STATE_SENDING_HEADER but when
 * the data is sent the connection will be closed
 */
void handle_output(struct connection *conn)
{
	ssize_t bytes_sent = 0;

	switch (conn->state) {
	case STATE_SENDING_HEADER:
		bytes_sent = connection_send_data(conn);
		DIE(bytes_sent < 0, "send_data error");
		conn->send_pos += bytes_sent;
		if (conn->send_pos >= conn->send_len) {
			if (conn->res_type == RESOURCE_TYPE_STATIC)
				conn->state = STATE_SENDING_DATA;
			else // RESOURCE_TYPE_DYNAMIC
				connection_start_async_io(conn);
		}
	break;

	case STATE_SENDING_DATA:
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			conn->state = connection_send_static(conn);
		} else {
			// DYNAMIC
			connection_send_dynamic(conn);
		}
		break;

	case STATE_ASYNC_ONGOING:
		connection_init_dynamic_send(conn);
		break;

	case STATE_SENDING_404:
		bytes_sent = connection_send_data(conn);
		DIE(bytes_sent < 0, "send_data error");
		conn->send_pos += bytes_sent;
		if (conn->send_pos >= conn->send_len)
			conn->state = STATE_404_SENT;
		break;

	default:
		ERR("Case not handled");
		exit(-1);
	}
}

/**
 * Choose what type of event was trigerred
 * If after the handling of the connection, the state is part of the ones below
 * then it completed its purpose and it must be closed
 */
void handle_client(uint32_t event, struct connection *conn)
{
	DIE((event & EPOLLIN) && (event & EPOLLOUT), "Shouldn't have both");
	if (event & EPOLLIN)
		handle_input(conn);
	else // EPOLLOUT
		handle_output(conn);
	if (conn->state == STATE_404_SENT || conn->state == STATE_DATA_SENT || conn->state == STATE_CONNECTION_CLOSED)
		connection_remove(conn);
}

/**
 * This function initializes a `sockaddr_in` structure, which represents the
 * combination of the IPv4 address and port number for the communication. It sets up the address
 * family to AF_INET (IPv4), assigns the specified IP address, and converts the port
 * number to network byte order using `htons`.
 */
struct sockaddr_in get_sockaddr(const char *ip, const int port)
{
	struct sockaddr_in addr;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(ip);

	return addr;
}

/**
 * This function initializes the asynchronous I/O context;
 * Creates an epoll instance that monitors the events in the program;
 * Creates a non-blocking server socket (listenfd) and configures it for reuse since it
 * is made for rapid testing;
 * Binds the server socket to the localhost and the AWS_LISTEN_PORT;
 * Adds the server socket to the epoll instance to monitor for incoming connections;
 * In the infinite loop events are monitored and if there is a new connection request
 * it gets created, otherwise the already ongoing communication event gets handled.
 */
int main(void)
{
	int rc = 0, flags = 0, num_events = 0;
	int set_true = 1;
	struct sockaddr_in addr = get_sockaddr(IP, AWS_LISTEN_PORT);
	struct epoll_event event;

	/* Initialize asynchronous operations. */
	memset(&ctx, 0, sizeof(io_context_t));
	DIE(io_setup(1, &ctx), "io_setup error");

	/* Initialize multiplexing. */
	epollfd = epoll_create1(0);
	DIE(epollfd < 0, "epoll creation failure");

	/* Create server socket and add it to epoll object */
	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	DIE(listenfd < 0, "socket creation failure");

	rc = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &set_true, sizeof(set_true));
	DIE(rc < 0, "setsockopt failed");

	flags = fcntl(listenfd, F_GETFL, 0);
	DIE(flags == -1, "fcntl error");

	rc = fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);
	DIE(rc < 0, "fcntl error");

	rc = bind(listenfd, (struct sockaddr *)&addr, sizeof(addr));
	DIE(rc < 0, "bind error");

	rc = listen(listenfd, DEFAULT_LISTEN_BACKLOG);
	DIE(rc < 0, "listen error");

	memset(&event, 0, sizeof(struct epoll_event));
	event.events = EPOLLIN;
	event.data.fd = listenfd;

	rc = epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &event);
	DIE(rc < 0, "epoll_ctl error");

	while (1) {
		struct epoll_event rev[MAX_EVENTS];

		memset(&rev, 0, sizeof(struct epoll_event));

		num_events = epoll_wait(epollfd, rev, 10, 1000);
		for (int i = 0; i < num_events; i++)
			if (rev[i].data.fd == listenfd) // There is a new connection request, on the server sokcet
				handle_new_connection();
			else // Socket communication
				handle_client(rev[i].events, rev[i].data.ptr);
	}
	return 0;
}
