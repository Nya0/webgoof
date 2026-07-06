#include "http.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_SIZE 10
#define DEFAULT_PORT "3030"
#define DEFAULT_WEB_ROOT "./public"

#define CLIENT_REQ_SIZE 1024 * 4

struct http_client {
	int fd;
	struct sockaddr_in addr;
	socklen_t addr_len;

	char *web_root;
};

void *handle_client(void *args) {
	struct http_client *client = args;
	LOG(LOG_INFO, "client accepted %d", client->fd);
	if (client->fd < 0) {
		LOG(LOG_ERROR, "accept: %s", strerror(errno));
		goto cleanup;
	}

	char client_ip[INET_ADDRSTRLEN] = {0};
	inet_ntop(AF_INET, &client->addr.sin_addr, client_ip, sizeof(client_ip));

	int client_port = ntohs(client->addr.sin_port);

	char raw_request[CLIENT_REQ_SIZE] = {0};

	ssize_t n = read(client->fd, raw_request, (size_t)CLIENT_REQ_SIZE - 1);
	if (n <= 0) { // connection closed or error
		goto cleanup;
	}

	struct http_request request = {0}; // zero init so logging is safe even if parse fails
	struct http_response response = {0};

	int file_fd = -1;
	off_t file_size = 0;

	if (parse_request(raw_request, &request) < 0) {
		response.status_code = HTTP_STATUS_BAD_REQUEST;
		goto finish;
	}

	LOG(LOG_INFO, "%s %s (%s:%d)", http_method_str(request.method), request.path, client_ip, client_port);
	for (int i = 0; i < request.header_count; i++) {
		// LOG(LOG_DEBUG, "  %s: %s", request.headers[i].key, request.headers[i].value);
	}

	// char* file_content = NULL;

	if (strstr(request.path, "..") != NULL) { // path traversal temp fix
		LOG(LOG_WARN, "blocked path traversal attempt: %s (%s:%d)", request.path, client_ip, client_port);

		response.status_code = HTTP_STATUS_FORBIDDEN;
		goto finish;
	}

	char file_path[256]; // NOLINT
	if (strcmp(request.path, "/") == 0) {
		snprintf(file_path, sizeof(file_path), "%s/index.html", client->web_root);
	} else {
		snprintf(file_path, sizeof(file_path), "%s%s", client->web_root, request.path);
	}

	// open requested file
	file_fd = open(file_path, O_RDONLY);
	if (file_fd < 0) {
		LOG(LOG_WARN, "open %s: %s", file_path, strerror(errno));

		response.status_code = HTTP_STATUS_NOT_FOUND;
		goto finish;
	}

	// read size
	struct stat file_stat;
	if (fstat(file_fd, &file_stat) < 0 || !S_ISREG(file_stat.st_mode)) {
		response.status_code = HTTP_STATUS_NOT_FOUND;
		goto finish;
	}
	file_size = file_stat.st_size;

	// file_content = calloc(1, file_size);
	// read(r_fd, file_content, file_size);
	// close(file_fd);

	// fill response
	response.status_code = HTTP_STATUS_OK;

	response.body = NULL;     // we will stream it rather than pre-buffer it
	response.body_length = 0; // = file_size

	const char *content_type = get_content_type(file_path);
	add_header(&response, "Content-Type", content_type);

	char lenbuf[32];
	snprintf(lenbuf, sizeof lenbuf, "%lld", (long long)file_size);
	add_header(&response, "Content-Length", lenbuf);

	size_t ser_len;

	off_t offset = 0;

finish:
	// write status
	strcpy(response.status_text, get_status_text(response.status_code));

	// serialize and send response
	char *serialized = serialize_response_header(&response, &ser_len);
	if (serialized) {
		write(client->fd, serialized, ser_len); // write header -- isnt guaranteed to write to len but wont happen
		free(serialized);

		if (file_fd != -1) {                                   // make sure there is a body so we dont serve trash
			sendfile(client->fd, file_fd, &offset, file_size); // write body
		}
	}

	close(file_fd);
cleanup:
	close(client->fd);
	free(client);
	LOG(LOG_WARN, "finished with client");
	return 0;
}

int listen_and_serve(int port, char *web_root) {
	signal(SIGPIPE, SIG_IGN);

	log_init(stderr);

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		LOG(LOG_ERROR, "socket: %s", strerror(errno));
		return -1;
	}

	// option to allow us to immediately reuse port by bypassing the wait period
	int opt = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in server_addr = {0};
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(port);

	if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		LOG(LOG_ERROR, "bind to port %d: %s", port, strerror(errno));
		return -1;
	}

	if (listen(sockfd, QUEUE_SIZE) < 0) {
		LOG(LOG_ERROR, "listen: %s", strerror(errno));
		close(sockfd);
		return -1;
	}

	LOG(LOG_INFO, "listening on http://localhost:%d (serving %s)", port, web_root);

	while (1) {
		pthread_t thread;

		struct http_client *client = calloc(1, sizeof(struct http_client));
		client->web_root = web_root;
		client->addr_len = sizeof(client->addr);
		client->fd = accept(sockfd, (struct sockaddr *)&client->addr, &client->addr_len);

		pthread_create(&thread, NULL, handle_client, client);
		pthread_detach(thread);
	};

	close(sockfd);
	return 0;
}

typedef struct {
	char *port;
	char *web_root;
} Options;

Options parse_args(int argc, char *argv[]) {
	Options opts = {0};
	int c;

	// Set defaults
	opts.port = DEFAULT_PORT;
	opts.web_root = DEFAULT_WEB_ROOT;

	while ((c = getopt(argc, argv, "hp:w:")) != -1) {
		switch (c) {
		case 'p':
			opts.port = optarg;
			break;
		case 'w':
			opts.web_root = optarg;
			break;
		case 'h':
			printf("Usage: %s [-p port] [-w web_root]\n", argv[0]);
			printf("  -p port      Server port (default: %s)\n", DEFAULT_PORT);
			printf("  -w web_root  Web root directory (default: %s)\n", DEFAULT_WEB_ROOT);
			printf("  -h           Show this help\n");
			exit(EXIT_SUCCESS);
		case '?':
		default:
			fprintf(stderr, "usage: %s [-p port] [-w web_root]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	return opts;
}

int main(int argc, char *argv[]) {
	Options opts = parse_args(argc, argv);

	int port_num = atoi(opts.port);
	if (port_num < 1 || port_num > 65535) {
		LOG(LOG_ERROR, "invalid port: %s", opts.port);
		exit(1);
	}

	if (access(opts.web_root, R_OK | X_OK) < 0) { // list dir | open files inside
		LOG(LOG_ERROR, "web_root not accessible: %s", opts.web_root);
		exit(1);
	}

	if (listen_and_serve(port_num, opts.web_root) < 0) {
		LOG(LOG_ERROR, "server failed to start on port %d", opts.port);
		exit(1);
	}

	return 0;
}
