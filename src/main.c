#include "cache.h"
#include "http.h"

#include "log.h"

#include <arpa/inet.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define DEFAULT_PORT "3030"
#define DEFAULT_WEB_ROOT "./public"
#define DEFAULT_THREAD_COUNT "4"
#define DEFAULT_VERBOSITY "4" // LOG_DEBUG

#define CLIENT_REQ_SIZE 4096

#define QUEUE_SIZE 256
#define QUEUE_DEPTH 512

#define BUF_COUNT 512
#define BUF_SIZE CLIENT_REQ_SIZE

// user_data tags for CQEs if they dont carry a connection pointer
#define UD_ACCEPT 0 // multishot accept
#define UD_SEND_FAIL 1 // failed send (its linked recv got canceled and does the teardown)


struct worker {
	int listen_fd;
};

struct connection {
	int fd;

	// request
	struct http_request request;
	char *frag_buf;
	size_t frag_len;

	// response
	struct http_response response;
	struct iovec iov[2]; // [0] = serialized header, [1] = body
	int iovcnt;
	struct msghdr msg;
};

char *global_web_root;

int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static struct io_uring_sqe *get_sqe(struct io_uring *r) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(r);

	return sqe;
}

static void queue_accept(struct io_uring *r, struct worker *w) {
	struct io_uring_sqe *sqe = get_sqe(r);
	io_uring_prep_multishot_accept(sqe, w->listen_fd, NULL, NULL, 0);
	io_uring_sqe_set_data64(sqe, UD_ACCEPT);
}

static void queue_read(struct io_uring *r, struct connection *c) {
	struct io_uring_sqe *sqe = get_sqe(r);
	io_uring_prep_recv_multishot(sqe, c->fd, NULL, 0, 0);
	sqe->flags |= IOSQE_BUFFER_SELECT;
	sqe->buf_group = 0;

	io_uring_sqe_set_data(sqe, c);
}

static void queue_write(struct io_uring *r, struct connection *c) {
	// the caller queues a linked recv right after
	// land in the same submission or the link chain would be split
	if (io_uring_sq_space_left(r) < 2)
		io_uring_submit(r);

	struct io_uring_sqe *sqe = get_sqe(r);

	c->msg.msg_iov = c->iov;
	c->msg.msg_iovlen = c->iovcnt;

	io_uring_prep_sendmsg(sqe, c->fd, &c->msg, MSG_WAITALL | MSG_NOSIGNAL);
	// -ECANCELED completion tears the connection down
	io_uring_sqe_set_flags(sqe, IOSQE_CQE_SKIP_SUCCESS);
	io_uring_sqe_set_data64(sqe, UD_SEND_FAIL);
}

// loads a file from cache or caches if it isnt. returns HTTP status
// on ok *out points to cache owned mem
static int load_file(const char *path, char **out, off_t *size) {
	struct cached_file *hit = cache_lookup(path);
	if (hit != NULL) {
		*out = hit->content;
		*size = hit->size;
		return HTTP_STATUS_OK;
	}

	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		LOG(LOG_WARN, "open %s: %s", path, strerror(errno));
		return HTTP_STATUS_NOT_FOUND;
	}

	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return HTTP_STATUS_NOT_FOUND;
	}

	char *buf = malloc(st.st_size);
	if (buf == NULL) {
		LOG(LOG_ERROR, "alloc %lld for %s: %s", (long long)st.st_size, path, strerror(errno));
		close(fd);
		return HTTP_STATUS_INTERNAL_ERROR;
	}

	off_t total = 0;
	while (total < st.st_size) {
		ssize_t r = read(fd, buf + total, st.st_size - total);
		if (r == 0)
			break;
		if (r == -1) {
			if (errno == EINTR)
				continue;
			break;
		}
		total += r;
	}
	close(fd);

	if (total != st.st_size || cache_insert(path, buf, st.st_size) == -1) {
		LOG(LOG_ERROR, "read/cache failed: %s", path);
		free(buf);
		return HTTP_STATUS_INTERNAL_ERROR;
	}

	*out = buf;
	*size = st.st_size;
	return HTTP_STATUS_OK;
}

// fill client->response 
void route_request(struct connection *client) {
	if (strstr(client->request.path, "..") != NULL) { // path traversal guard
		client->response.status_code = HTTP_STATUS_FORBIDDEN;
		return;
	}

	char file_path[256];
	if (strcmp(client->request.path, "/") == 0)
		snprintf(file_path, sizeof file_path, "%s/index.html", global_web_root);
	else
		snprintf(file_path, sizeof file_path, "%s%s", global_web_root, client->request.path);

	char *body;
	off_t size;
	int status = load_file(file_path, &body, &size);
	client->response.status_code = status;
	if (status != HTTP_STATUS_OK)
		return;

	client->response.body = body;
	client->response.body_length = size;

	add_header(&client->response, "Content-Type", get_content_type(file_path));
}

void build_response(struct connection *client) {
	client->iovcnt = 0; // reset iter

	char lenbuf[32];
	snprintf(lenbuf, sizeof lenbuf, "%lld", (long long)client->response.body_length);
	add_header(&client->response, "Content-Length", lenbuf); // needed for keep-alive
	add_header(&client->response, "Connection", "keep-alive");

	strcpy(client->response.status_text, get_status_text(client->response.status_code));

	size_t ser_len;
	char *serialized = serialize_response_header(&client->response, &ser_len);
	client->iov[client->iovcnt++] = (struct iovec){.iov_base = serialized, .iov_len = ser_len};

	if (client->response.body != NULL) {
		client->iov[client->iovcnt++] =
		    (struct iovec){.iov_base = client->response.body, .iov_len = client->response.body_length};
	}

	client->msg.msg_iov = client->iov;
	client->msg.msg_iovlen = client->iovcnt;
}

static void close_conn(struct connection *c) {
	close(c->fd);
	free(c->iov[0].iov_base); // serialized header (NULL if response not built)
	free(c);
}

void handle_client_read(struct io_uring *r, struct connection *client, char *data, int res) { // TODO: deny requests with bodies until we implement methods that accept and stop request smuggling
	// free previous
	free(client->iov[0].iov_base);
	client->iov[0].iov_base = NULL;
	client->iovcnt = 0;

	char *term = memmem(data, res, "\r\n\r\n", 4);
	if (term == NULL) {
		// fragment path
		return;
	}

	term[2] = '\0'; // (\r\n\0\n) null term for parse request so it doesnt blow to infinity
	if (parse_request(data, &client->request) == -1)
		client->response.status_code = HTTP_STATUS_BAD_REQUEST;
	else
		route_request(client);

	build_response(client);
	queue_write(r, client);

	memset(&client->request, 0, sizeof client->request);
	memset(&client->response, 0, sizeof client->response);
}

void *worker_thread(void *arg) {
	struct worker *w = arg;

	// set up uring
	struct io_uring ring;
	io_uring_queue_init(QUEUE_DEPTH, &ring, IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN);
	io_uring_register_ring_fd(&ring);

	    // setup buffer ring
	    int err = 0;
	struct io_uring_buf_ring *buffer_ring = io_uring_setup_buf_ring(&ring, BUF_COUNT, 0, 0, &err);
	if (!buffer_ring) {
		LOG(LOG_ERROR, "failed to create buffer ring: %d", strerror(err));
		return NULL;
	}

	// create and hand buffers to the kernel
	char *slab = calloc(BUF_COUNT, BUF_SIZE);
	for (int i = 0; i < BUF_COUNT; i++)
		io_uring_buf_ring_add(buffer_ring, slab + (i * BUF_SIZE), BUF_SIZE, i, io_uring_buf_ring_mask(BUF_COUNT), i);

	io_uring_buf_ring_advance(buffer_ring, BUF_COUNT);

	// jump start
	queue_accept(&ring, w);

	while (1) {
		io_uring_submit_and_wait(&ring, 1); // flush what was queued last iteration

		// loop state
		uint head;
		uint n = 0, returned = 0; // returned for buf_ring how many were used and freed to advance later;

		struct io_uring_cqe *cqe;

		io_uring_for_each_cqe(&ring, head, cqe) {
			switch (cqe->user_data) {
			case UD_ACCEPT: {
				if (cqe->res >= 0) {
					struct connection *c = calloc(1, sizeof *c);
					c->fd = cqe->res;
					queue_read(&ring, c);
				}
				if (!(cqe->flags & IORING_CQE_F_MORE)) // multishot accept ended re-arm
					queue_accept(&ring, w);

				break;
			}

			case UD_SEND_FAIL:
				break;

			default: {
				struct connection *c = (void *)cqe->user_data;
				char *data = NULL;
				int bid = -1;

				if (cqe->flags & IORING_CQE_F_BUFFER) {          
					bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT; 
					data = slab + (size_t)bid * BUF_SIZE;        // buf_size is client_req_size
				}

				if (cqe->res > 0) {
					handle_client_read(&ring, c, data, cqe->res);
					if (!(cqe->flags & IORING_CQE_F_MORE))
						queue_read(&ring, c); // alive ended, re-arm
				} else if (cqe->res == -ENOBUFS) {
					queue_read(&ring, c); // pool empty, re-arm
				} else {
					close_conn(c); // 0/error teardown
				}

				if (bid >= 0) {
					io_uring_buf_ring_add(buffer_ring, data, BUF_SIZE, bid, io_uring_buf_ring_mask(BUF_COUNT), returned);
					returned++;
				}
			}
			}
			n++;
		}

		io_uring_buf_ring_advance(buffer_ring, returned);
		io_uring_cq_advance(&ring, n); // consume from cq
	}
}

int make_listener(in_port_t port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		LOG(LOG_ERROR, "create socket (p%u): %s", port, strerror(errno));
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

	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1) {
		LOG(LOG_ERROR, "bind to port %d: %s", port, strerror(errno));
		return -1;
	}

	if (listen(fd, QUEUE_SIZE) == -1) {
		LOG(LOG_ERROR, "listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int listen_and_serve(in_port_t port, char *web_root, int thread_count) {
	signal(SIGPIPE, SIG_IGN); // stop a broken client from killing the server
	global_web_root = web_root;

	for (int i = 0; i < thread_count; i++) {
		pthread_t thread;

		struct worker *w = calloc(1, sizeof *w);
		w->listen_fd = make_listener(port);
		if (w->listen_fd == -1) {
			LOG(LOG_ERROR, "thread (%d) socket: %s", i, strerror(errno));
			return -1;
		}

		if (pthread_create(&thread, NULL, worker_thread, w) == -1) {
			LOG(LOG_DEBUG, "thread (%d) creation: %s", i, strerror(errno));
			exit(1);
		}
		pthread_detach(thread);

		LOG(LOG_DEBUG, "thread (%d) ready", i);
	}

	LOG(LOG_INFO, "listening on http://localhost:%d (serving %s using %d thread(s))", port, web_root, thread_count);
	pause();

	return 0;
}

typedef struct options {
	char *port;
	char *web_root;
	char *thread_count;
	char *verbosity;
} options;

options parse_args(int argc, char *argv[]) {
	options opts = {0};
	int c;

	opts.port = DEFAULT_PORT;
	opts.web_root = DEFAULT_WEB_ROOT;
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
			printf("  -t threads   Thread Count (default: number of online CPUs)\n");
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

	printf("\033[31m"); // red color for error area

	struct rlimit limit = {0};
	if (getrlimit(RLIMIT_NPROC, &limit) == -1) {
		printf("failed to get system thread limit");
		exit(1);
	}

	int port_input = atoi(opts.port);
	if (port_input < 1 || port_input > 65535) {
		printf("invalid port: %s\n", opts.port);
		fflush(stderr);
		exit(1);
	}
	in_port_t port_num = (in_port_t)port_input;

	// default to online CPU count
	uint16_t thread_count;
	if (opts.thread_count != NULL) {
		thread_count = atoi(opts.thread_count);
	} else {
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);
		thread_count = (cpus > 0) ? (uint16_t)cpus : (uint16_t)atoi(DEFAULT_THREAD_COUNT);
	}

	if (thread_count < 1 || thread_count > limit.rlim_cur) {
		printf("thread range 1-%lu\n", (unsigned long)limit.rlim_cur);
		exit(1);
	}

	int verbosity = atoi(opts.verbosity);
	if (verbosity < LOG_NONE || verbosity > LOG_DEBUG) {
		printf("log level must be within range (0:NONE, 1:ERROR, 2:WARN, 3:INFO, 4:DEBUG)\n");
		exit(1);
	}
	log_set_verbosity((log_level_t)verbosity);
	printf("LOG_LEVEL = %i\n", LOG_LEVEL);

	if (access(opts.web_root, R_OK | X_OK) == -1) { // can list dir | open files inside it
		printf("web_root not accessible: %s\n", opts.web_root);
		exit(1);
	}

	printf("\033[0m"); // reset term color

	if (listen_and_serve(port_num, opts.web_root, thread_count) == -1) {
		printf("\033[31m"); // red color for error area
		printf("server failed to start on port %u\n", port_num);
		printf("\033[0m"); // reset term color
		exit(1);
	}

	return 0;
}
