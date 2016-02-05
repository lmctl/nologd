/* nologd: consume all the logs without any processing */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/epoll.h>

#define NELEMS(arr) (sizeof(arr)/sizeof(arr[0]))

/* Server context */
struct Server {
     int epoll_fd;

     int dev_log_fd;
     int journal_fd;
     int stdout_fd;

     int log_fd;
};


static char *progname;

void epoll_addwatch(struct Server *s, int fd)
{
     struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
     epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

void fd_set_nonblock(int fd)
{
     fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

int unix_open(struct Server *s, int type, const char *path)
{
     int r;
     int fd;
     struct sockaddr_un sa = { .sun_family = AF_UNIX };

     fd = socket(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
     if (fd < 0)
	  return -errno;

     unlink(path);

     strncpy(&sa.sun_path[0], path, NELEMS(sa.sun_path));

     r = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
     if (r < 0) {
	  close(fd);
	  return -errno;
     }

     r = (type == SOCK_STREAM) ? listen(fd, SOMAXCONN) : 0;
     if (r < 0) {
	  close(fd);
	  return -errno;
     }

     return fd;
}

int unix_accept(struct Server *s, int stdout_fd)
{
     int fd;
     struct sockaddr_un sa;
     socklen_t slen;

     fd = accept4(stdout_fd, (struct sockaddr *)&sa, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
     if (fd < 0) {
	  fprintf(stderr, "accept failed: %s\n", strerror(errno));
	  return;
     }

     return fd;
}

void consume(struct Server *s, int fd, int do_close)
{
     int r;
     static char buf[2048];

     do {
	  r = read(fd, &buf[0], NELEMS(buf) - 1);
	  if (r > 0 && s->log_fd != -1) {
	       buf[r] = '\n';
	       write(s->log_fd, &buf[0], r+1);
	  }

     } while (r > 0);

     if (do_close && r == 0)
	  epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

void usage(void)
{
     printf("usage: %s [-d] [-f FILE] [-h]\n"
	    " -d        daemonize\n"
	    " -f FILE   drop logs to FILE\n"
	    " -h        this help screen\n",
	  progname);
}

int main(int argc, char *argv[])
{
     int c;
     int r;
     struct Server s = {
	  .log_fd = -1,
	  .dev_log_fd = -1,
	  .journal_fd = -1,
	  .stdout_fd = -1,
     };
     struct epoll_event ev;
     int do_daemonize = 0;

     progname = argv[0];

     do {
	  c = getopt(argc, argv, "dhf:");

	  if (c == 'd')
	       do_daemonize = 1;
	  else if (c == 'f') {
	       int fd = open(optarg, O_WRONLY | O_CREAT | O_APPEND, 0640);

	       if (fd < 0) {
		    fprintf(stderr, "Unable to open %s: %m\n", optarg);
		    exit(EXIT_FAILURE);
	       }
	       s.log_fd = fd;
	  } else if (c == 'h' || c == '?') {
	       usage();
	       exit(c == 'h' ? EXIT_SUCCESS : EXIT_FAILURE);
	  }

     } while (c != -1);

     if (do_daemonize)
	  daemon(0, 0);

     s.epoll_fd = epoll_create1(EPOLL_CLOEXEC);

     mkdir("/run/systemd", 0755);
     mkdir("/run/systemd/journal", 0755);

     s.dev_log_fd = unix_open(&s, SOCK_DGRAM, "/dev/log");
     s.journal_fd = unix_open(&s, SOCK_DGRAM, "/run/systemd/journal/socket");
     s.stdout_fd = unix_open(&s, SOCK_STREAM, "/run/systemd/journal/stdout");
     if (s.dev_log_fd >= 0) {
	  fd_set_nonblock(s.dev_log_fd);
	  epoll_addwatch(&s, s.dev_log_fd);
     }

     if (s.journal_fd >= 0) {
	  fd_set_nonblock(s.journal_fd);
	  epoll_addwatch(&s, s.journal_fd);
     }

     if (s.stdout_fd >= 0) {
	  epoll_addwatch(&s, s.stdout_fd);
     }

     while (1) {
	  r = epoll_wait(s.epoll_fd, &ev, 1, -1);
	  if (r < 0 && errno != EINTR) {
	       fprintf(stderr, "epoll_wait failed: %s\n", strerror(errno));
	       exit(EXIT_FAILURE);
	  }

	  if (ev.data.fd == s.dev_log_fd) {
	       consume(&s, s.dev_log_fd, 0);
	  } else if (ev.data.fd == s.journal_fd) {
	       consume(&s, s.journal_fd, 0);
	  } else if (ev.data.fd == s.stdout_fd) {
	       int newfd = unix_accept(&s, s.stdout_fd);
	       fd_set_nonblock(newfd);
	       epoll_addwatch(&s, newfd);
	  } else {
	       /* pre-opened stdout fd */
	       consume(&s, ev.data.fd, 1);
	  }
     }

     return EXIT_SUCCESS;
}
