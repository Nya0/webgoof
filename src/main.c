#include "cache.h"
#include "http.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT "3030"
#define DEFAULT_WEB_ROOT "./public"
#define DEFAULT_THREAD_COUNT "4"
#define DEFAULT_VERBOSITY "4" // LOG_DEBUG

#define QUEUE_SIZE 4096
#define CLIENT_REQ_SIZE 1024 * 4

struct worker {
	int listen_fd;
	int epfd;
	char *web_root;
};

void *handle_client(struct http_client *client) {
	if (client->fd < 0) {
		LOG(LOG_ERROR, "accept: %s", strerror(errno));
		goto cleanup;
	}

	char client_ip[INET_ADDRSTRLEN] = {0};
	inet_ntop(AF_INET, &client->addr.sin_addr, client_ip, sizeof(client_ip));

	in_port_t client_port = ntohs(client->addr.sin_port);

	char raw_request[CLIENT_REQ_SIZE] = {0};

	// ssize_t total_read = 0;
	// for (;;) {
	// 	ssize_t n = read(client->fd, raw_request + total_read, (size_t)CLIENT_REQ_SIZE - 1 - total_read);
	// 	LOG(LOG_ERROR, "total read: %u", total_read);
	// 	LOG(LOG_ERROR, "n: %d", n);
	// 	if (n > 0) {
	// 		total_read += n;
	// 		if (total_read >= CLIENT_REQ_SIZE - 1)
	// 			break;
	// 		continue;
	// 	}

	// 	if (n == 0) {
	// 		goto cleanup;
	// 	}
	// 	if (n < 0) {
	// 		LOG(LOG_ERROR, "read error: %d", n);
	// 		if (errno == EAGAIN || errno == EWOULDBLOCK)
	// 			break;    // drained
	// 		goto cleanup; // real error
	// 	};
	// }

	ssize_t n = read(client->fd, raw_request, (size_t)CLIENT_REQ_SIZE - 1);
	if (n < 0) {
		LOG(LOG_ERROR, "read error: %d", n);
		goto cleanup; // real error
	};
	struct http_request request = {0}; // zero init so logging is safe even if parse fails
	struct http_response response = {0};

	int file_fd = -1;
	off_t file_size = 0;

	if (parse_request(raw_request, &request) < 0) {
		response.status_code = HTTP_STATUS_BAD_REQUEST;
		goto finish;
	}

	for (int i = 0; i < request.header_count; i++) {
		LOG(LOG_DEBUG, "  %s: %s", request.headers[i].key, request.headers[i].value);
	}

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

	// cache handling
	struct cached_file *file = cache_lookup(file_path);
	LOG(LOG_DEBUG, "file pointer: %p", file);
	if (file == NULL) { // cache miss
		LOG(LOG_ERROR, "cache miss: %s", file_path);

		file_fd = open(file_path, O_RDONLY);
		if (file_fd < 0) {

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

		int cs = cache_insert(file_path, file_fd, file_size);
		if (cs < 0) {
			LOG(LOG_ERROR, "cache insert failed: %s", file_path);
			close(file_fd);
			file_fd = -1;
		}
	} else {
		file_fd = file->fd;
		file_size = file->size;
	}

	// file_content = calloc(1, file_size);
	// read(file_fd, file_content, file_size);

	// fill response
	response.status_code = HTTP_STATUS_OK;

	response.body = NULL;             // we will stream it rather than pre-buffer it
	response.body_length = file_size; // = file_size

	const char *content_type = get_content_type(file_path);
	add_header(&response, "Content-Type", content_type);

	char lenbuf[32];
	snprintf(lenbuf, sizeof lenbuf, "%lld", (long long)response.body_length);
	add_header(&response, "Content-Length", lenbuf);

	size_t ser_len;

finish:
	// write status
	strcpy(response.status_text, get_status_text(response.status_code));

	// serialize and send response
	char *serialized = serialize_response_header(&response, &ser_len);
	if (serialized) {
		struct iovec iov[1] = {
		    {.iov_base = serialized, .iov_len = ser_len},
		};
		writev(client->fd, iov, 1);

		if (file_fd != -1) { // make sure there is a body so we dont serve trash
			off_t offset = 0;
			for (;;) {

				ssize_t n = sendfile(client->fd, file_fd, &offset, file_size);
				if (n > 0) {
					if (offset >= file_size)
						break;
					continue;
				}

				if (n < 0) {
					if (errno == EAGAIN) {
						struct pollfd p = {.fd = client->fd, .events = POLLOUT};
						poll(&p, 1, -1);
						continue;
					};
					if (errno == EINTR)
						continue;

					break; // real error
				};
			}
		}

		free(serialized);
	}

cleanup:
	close(client->fd);
	free(client);
	LOG(LOG_WARN, "finished with client");
	return 0;
}

int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void accept_all(struct worker *w) {
	for (;;) {
		struct http_client *client = calloc(1, sizeof(struct http_client));
		client->web_root = w->web_root;
		client->addr_len = sizeof(client->addr);
		client->fd = accept4(w->listen_fd, (struct sockaddr *)&client->addr, &client->addr_len, SOCK_NONBLOCK);
		if (client->fd < 0) {
			free(client);
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break; // queue drained
			if (errno == EINTR)
				continue; // retry
			break;
		}
		struct epoll_event ev;
		ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
		ev.data.ptr = client;
		epoll_ctl(w->epfd, EPOLL_CTL_ADD, client->fd, &ev);
	}
}

void *worker_thread(void *arg) {
	struct worker *w = arg;
	struct epoll_event events[64];

	struct epoll_event lev = {.events = EPOLLIN, .data.ptr = NULL}; // NULL tags "it's the listener"
	epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listen_fd, &lev);

	while (1) {
		int n = epoll_wait(w->epfd, events, 64, -1);
		for (int i = 0; i < n; i++) {
			if (events[i].data.ptr == NULL) {
				accept_all(w);
			} else {
				handle_client(events[i].data.ptr);
			}
		}
	}
}

int make_listener(in_port_t port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return fd;
	}

	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt);

	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = INADDR_ANY;
	a.sin_port = htons(port);

	set_nonblocking(fd);

	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		LOG(LOG_ERROR, "bind to port %d: %s", port, strerror(errno));
		return -1;
	}

	if (listen(fd, QUEUE_SIZE) < 0) {
		LOG(LOG_ERROR, "listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int listen_and_serve(in_port_t port, char *web_root, int thread_count) {
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < thread_count; i++) {
		struct worker *w = calloc(1, sizeof *w);
		pthread_t thread;
		w->epfd = epoll_create1(0);
		w->listen_fd = make_listener(port);
		if (w->listen_fd < 0) {
			LOG(LOG_ERROR, "thread %d socket: %s", i, strerror(errno));
			return -1;
		}
		w->web_root = web_root;

		pthread_create(&thread, NULL, worker_thread, w);
		pthread_detach(thread);
		LOG(LOG_DEBUG, "thread (%d) ready", i);
	}

	LOG(LOG_INFO, "listening on http://localhost:%d (serving %s with %d threads)", port, web_root, thread_count);
	pause();

	return 0;
}

typedef struct {
	char *port;
	char *web_root;
	char *thread_count;
	char *verbosity;
} options;

options parse_args(int argc, char *argv[]) {
	options opts = {0};
	int c;

	// Set defaults
	opts.port = DEFAULT_PORT;
	opts.web_root = DEFAULT_WEB_ROOT;
	opts.thread_count = DEFAULT_THREAD_COUNT;
	opts.verbosity = DEFAULT_VERBOSITY;

	while ((c = getopt(argc, argv, "hp:w:t:v:")) != -1) {
		switch (c) {
		case 'p':
			opts.port = optarg;
			break;
		case 'w':
			opts.web_root = optarg;
			break;
		case 't':
			opts.thread_count = optarg;
			break;
		case 'v':
			opts.verbosity = optarg;
			break;
		case 'h':
			printf("Usage: %s [-w web_root] [-p port] [-t threads] [-v level]\n", argv[0]);
			printf("  -w web_root  Web root directory (default: %s)\n", DEFAULT_WEB_ROOT);
			printf("  -p port      Server port (default: %s)\n", DEFAULT_PORT);
			printf("  -t threads   Thread Count (default: %s)\n", DEFAULT_THREAD_COUNT);
			printf("  -v           Verbosity level (default: %s) (0:NONE, 1:ERROR, 2:WARN, 3:INFO, 4:DEBUG)\n", DEFAULT_VERBOSITY);
			printf("  -h           Show this help\n");
			exit(EXIT_SUCCESS);
		case '?':
		default:
			fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	return opts;
}

int main(int argc, char *argv[]) {
	log_init(stderr);
	cache_init(100);

	options opts = parse_args(argc, argv);

	in_port_t port_num = atoi(opts.port);
	if (port_num < 1 || port_num > 65535) {
		LOG(LOG_ERROR, "invalid port: %s", opts.port);
		exit(1);
	}

	int thread_count = atoi(opts.thread_count);
	if (thread_count < 0) {
		LOG(LOG_ERROR, "cant have 0 threads");
		exit(1);
	}

	int verbosity = atoi(opts.verbosity);
	if (verbosity < LOG_NONE || verbosity > LOG_DEBUG) {
		LOG(LOG_ERROR, "log level must be within range (0:NONE, 1:ERROR, 2:WARN, 3:INFO, 4:DEBUG)");
		exit(1);
	}
	log_set_verbosity((log_level_t)verbosity);

	if (access(opts.web_root, R_OK | X_OK) < 0) { // list dir | open files inside
		LOG(LOG_ERROR, "web_root not accessible: %s", opts.web_root);
		exit(1);
	}

	if (listen_and_serve(port_num, opts.web_root, thread_count) < 0) {
		LOG(LOG_ERROR, "server failed to start on port %d", opts.port);
		exit(1);
	}

	return 0;
}
