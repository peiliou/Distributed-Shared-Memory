#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define errExit(msg)                \
	do                          \
	{                           \
		perror(msg);        \
		exit(EXIT_FAILURE); \
	} while (0)

#define BUFF_SIZE 4096

static int page_size;
int sock;
char *addr;	   /* Start of region handled by userfaultfd */
unsigned long len; /* Length of region handled by userfaultfd */
char *msi;
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t condition_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition_cond = PTHREAD_COND_INITIALIZER;

enum
{
	INVALID = 'I',
	SHARED = 'S',
	MODIFIED = 'M',
};

/* request type */
enum
{
	PAGE_CREATE,
	PAGE_INVALIDATE,
	PAGE_FETCH
};

static void *
fault_handler_thread(void *arg)
{
	static struct uffd_msg msg; /* Data read from userfaultfd */
	long uffd;		    /* userfaultfd file descriptor */
	static char *page = NULL;
	struct uffdio_copy uffdio_copy;
	ssize_t nread;
	char buffer[BUFF_SIZE];
	int page_index;

	uffd = (long)arg;

	if (page == NULL)
	{
		page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page == MAP_FAILED)
			errExit("mmap");
	}

	for (;;)
	{
		struct pollfd pollfd;
		int nready;

		pollfd.fd = uffd;
		pollfd.events = POLLIN;
		nready = poll(&pollfd, 1, -1);
		if (nready == -1)
			errExit("poll");
		nread = read(uffd, &msg, sizeof(msg));
		if (nread == 0)
		{
			printf("EOF on userfaultfd!\n");
			exit(EXIT_FAILURE);
		}
		if (nread == -1)
			errExit("read");
		if (msg.event != UFFD_EVENT_PAGEFAULT)
		{
			fprintf(stderr, "Unexpected event on userfaultfd\n");
			exit(EXIT_FAILURE);
		}
		printf("    [x] PAGEFAULT\n");
		memset(buffer, 0, sizeof(buffer));
		page_index = ((char *)msg.arg.pagefault.address - addr) / page_size;

		/* write fault */
		if (msg.arg.pagefault.flags == UFFD_PAGEFAULT_FLAG_WRITE)
			switch (msi[page_index])
			{
			case INVALID:
				break;
			case SHARED:
				break;
			case MODIFIED:
				break;
			}
		/* read fault */
		else
			switch (msi[page_index])
			{
			case INVALID:
				sprintf(buffer, "%d %d", PAGE_FETCH, page_index);
				send(sock, buffer, sizeof(buffer), 0);

				if (read(sock, buffer, sizeof(buffer)) < 0)
					errExit("fault handler read");

				/* other side is also invalid */
				if ((int)buffer[0] == 1)
					memset(page, 0, page_size);
				else
					memcpy(page, buffer, page_size);

				msi[page_index] = SHARED;
				break;
			case SHARED:
				break;
			case MODIFIED:
				break;
			}

		uffdio_copy.src = (unsigned long)page;
		uffdio_copy.dst = (unsigned long)msg.arg.pagefault.address &
				  ~(page_size - 1);
		uffdio_copy.len = page_size;
		uffdio_copy.mode = 0;
		uffdio_copy.copy = 0;

		if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1)
			errExit("ioctl-UFFDIO_COPY");
	}
}

static void *
server_handler_thread(void *arg)
{
	/* for server */
	int opt = 1;
	struct sockaddr_in address;
	int addrlen = sizeof(address);
	int server_fd, new_socket_fd;
	unsigned long listen_port;
	char buffer[BUFF_SIZE];

	int request_t;
	int option;
	char error_char = 1;

	listen_port = (unsigned long)arg;

	if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		errExit("server socket failed");
	}

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
		       &opt, sizeof(opt)))
	{
		errExit("server setsockopt");
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(listen_port);

	if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
	{
		errExit("server bind failed");
	}

	if (listen(server_fd, 1) < 0)
	{
		errExit("server listen");
	}

	if ((new_socket_fd = accept(server_fd, (struct sockaddr *)&address,
				    (socklen_t *)&addrlen)) < 0)
	{
		errExit("server accept");
	}

	for (;;)
	{
		int total_bytes = 0, received_bytes = 0;
		memset(buffer, 0, sizeof(buffer));
		do
		{
			received_bytes = read(new_socket_fd, buffer, BUFF_SIZE);
			if (received_bytes < 0)
				errExit("server read");
			else
				total_bytes += received_bytes;
		} while (total_bytes < BUFF_SIZE);

		if (sscanf(buffer, "%d", &request_t) < 0)
			continue;

		switch (request_t)
		{
		case PAGE_CREATE:
			if (sscanf(buffer, "%*d %p %lu", &addr, &len) < 0)
				break;

			addr = mmap(addr, len, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (addr == MAP_FAILED)
				errExit("server mmap");

			printf("received address returned by mmap() = %p, mapped size = %lu \n", addr, len);

			pthread_mutex_lock(&condition_mutex);
			if (addr && len)
				pthread_cond_signal(&condition_cond);
			pthread_mutex_unlock(&condition_mutex);

			break;
		case PAGE_INVALIDATE:
			if (sscanf(buffer, "%*d %d", &option) < 0)
				break;

			if (madvise(addr + page_size * option, page_size, MADV_DONTNEED))
				errExit("fail to madvise");

			msi[option] = INVALID;

			/* debug("received %d", option); */

			break;
		case PAGE_FETCH:
			if (sscanf(buffer, "%*d %d", &option) < 0 || option < 0)
				break;

			switch (msi[option])
			{
			case MODIFIED:
				msi[option] = SHARED;
			case SHARED:
				if ((send(new_socket_fd, addr + page_size * option, page_size, 0)) == -1)
					errExit("fail to send data for fetch request");
				break;
			case INVALID:
				if ((send(new_socket_fd, &error_char, sizeof(error_char), 0)) == -1)
					errExit("fail to send data for fetch request");

				msi[option] = SHARED;
				break;
			}

			break;
		}
	}
}

int main(int argc, char *argv[])
{
	/* general vars */
	int res;
	pthread_t server_thr, fault_handler_thr;
	unsigned long listen_port, send_port;

	/* for client */
	int num_page = 0;
	struct sockaddr_in serv_addr;
	char buffer[BUFF_SIZE] = {0};

	long uffd; /* userfaultfd file descriptor */
	struct uffdio_api uffdio_api;
	struct uffdio_register uffdio_register;

	char *line = NULL;
	size_t input_buf_len = 0;

	int failed_cnt = 0;

	if (argc != 3)
	{
		fprintf(stderr, "Usage: %s listen_port send_port\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	listen_port = strtoul(argv[1], NULL, 0);
	send_port = strtoul(argv[2], NULL, 0);

	if (!listen_port || !send_port)
	{
		printf("\n Wrong input format \n");
		return -1;
	}

	if ((res = pthread_create(&server_thr, NULL, server_handler_thread, (void *)listen_port)))
	{
		errno = res;
		errExit("pthread_create");
	}

	/* client begin */
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		printf("\n Socket creation error \n");
		return -1;
	}

	memset(&serv_addr, '0', sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(send_port);

	if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0)
	{
		printf("\nInvalid address/ Address not supported \n");
		return -1;
	}

	while (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
	{
		failed_cnt++;
		sleep(1);
	}

	page_size = sysconf(_SC_PAGE_SIZE);

	if (failed_cnt)
	{
		printf("> How many pages would you like to allocate (greater than 0)? ");
		while (getline(&line, &input_buf_len, stdin) != -1 && sscanf(line, "%9d", &num_page) < 0)
			;

		len = num_page * page_size;

		addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (addr == MAP_FAILED)
			errExit("mmap");

		sprintf(buffer, "%d %p %lu", PAGE_CREATE, addr, len);
		send(sock, buffer, sizeof(buffer), 0);
		printf("sent address returned by mmap() = %p, mapped size = %lu \n", addr, len);
	}

	pthread_mutex_lock(&condition_mutex);
	while (addr == NULL || len == 0)
	{
		pthread_cond_wait(&condition_cond, &condition_mutex);
	}
	pthread_mutex_unlock(&condition_mutex);

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd == -1)
		errExit("userfaultfd");

	uffdio_api.api = UFFD_API;
	uffdio_api.features = 0;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
		errExit("ioctl-UFFDIO_API");

	uffdio_register.range.start = (unsigned long)addr;
	uffdio_register.range.len = len;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
		errExit("ioctl-UFFDIO_REGISTER");

	res = pthread_create(&fault_handler_thr, NULL, fault_handler_thread, (void *)uffd);
	if (res != 0)
	{
		errno = res;
		errExit("pthread_create");
	}

	if (!(msi = malloc(len / page_size + 1)))
		errExit("malloc");

	memset(msi, INVALID, len / page_size);

	for (;;)
	{
		char cmd;
		int page_index = -2;
		int l, start, end;
		int max_page_index = (int)(len / page_size) - 1;
		int char_read;

		printf("> Which command should I run? (r:read, w:write, v:view msi array): ");
		while (getline(&line, &input_buf_len, stdin) != -1 && sscanf(line, "%c", &cmd) < 0)
			;

		printf("For which page? (0-%i, or -1 for all): ", max_page_index);
		while (getline(&line, &input_buf_len, stdin) != -1 && sscanf(line, "%d", &page_index) < 0)
			;

		if (page_index < -1 || page_index > max_page_index)
			continue;

		start = (page_index == -1 ? 0x0 : (0x0 + page_size * page_index));
		end = (0x0 + page_size * ((page_index == -1 ? max_page_index : page_index) + 1));
		switch (cmd)
		{
		case 'r':
			for (l = start; l < end; l += page_size)
			{
				int page_index = (int)l / page_size;
				char null_char = addr[l + page_size - 1];

				printf(" [*] Page %i:\n%.*s%c\n", page_index, page_size - 1,
				       &addr[l], null_char);
			}
			break;
		case 'w':
			printf("> Type your new message: ");

			char_read = getline(&line, &input_buf_len, stdin);

			for (l = start; l < end; l += page_size)
			{
				int page_index = (int)l / page_size;

				memset(&addr[l], 0, page_size);
				memcpy(&addr[l], line, char_read < (page_size - 1) ? char_read : (page_size - 1));

				printf(" [*] Page %i:\n%.*s\n", page_index, page_size - 1, &addr[l]);

				memset(buffer, 0, sizeof(buffer));
				sprintf(buffer, "%d %d", PAGE_INVALIDATE, page_index);
				send(sock, buffer, sizeof(buffer), 0);

				msi[page_index] = MODIFIED;
			}
			break;
		case 'v':
			if (page_index < 0)
				printf("%s\n", msi);
			else
				printf("%c\n", msi[page_index]);
			break;
		}
	}

	exit(EXIT_SUCCESS);
}
