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
     int kmsg_fd;
};

int unix_open(struct Server *s, int type, const char *path)
{
     int r;
     int fd;
     struct sockaddr_un sa = { .sun_family = AF_UNIX };
     struct epoll_event ev = { .events = EPOLLIN };

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

     fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

     ev.data.fd = fd;
     epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

     return fd;
}

void unix_accept(struct Server *s, int stdout_fd)
{
     int fd;
     struct epoll_event ev = { .events = EPOLLIN };
     struct sockaddr_un sa;
     socklen_t slen;

     fd = accept4(stdout_fd, (struct sockaddr *)&sa, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
     if (fd < 0) {
	  fprintf(stderr, "accept failed: %s\n", strerror(errno));
	  return;
     }

     ev.data.fd = fd;
     epoll_ctl(s->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

void consume(struct Server *s, int fd, int do_close)
{
     int r;
     static char buf[2048];

     /*
     r = ioctl(fd, SIOCINQ, &len);
     if (r < 0)
	  return;
     */

     do {
	  r = read(fd, &buf[0], NELEMS(buf));
     } while (r > 0);

     if (do_close && r == 0)
	  epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
}

int main(int argc, char *argv[])
{
     int r;
     struct Server s;
     struct epoll_event ev;

     s.epoll_fd = epoll_create1(EPOLL_CLOEXEC);

     mkdir("/run/systemd", 0755);
     mkdir("/run/systemd/journal", 0755);

     s.dev_log_fd = unix_open(&s, SOCK_DGRAM, "/dev/log");
     s.journal_fd = unix_open(&s, SOCK_DGRAM, "/run/systemd/journal/socket");
     s.stdout_fd = unix_open(&s, SOCK_STREAM, "/run/systemd/journal/stdout");

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
	       unix_accept(&s, s.stdout_fd);
	  } else {
	       /* pre-opened stdout fd */
	       consume(&s, ev.data.fd, 1);
	  }
     }

     return EXIT_SUCCESS;
}
