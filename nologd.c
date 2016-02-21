/* nologd: consume all the logs without any processing
 *
 * Copyright (C) 2014, 2016  Karol Lewandowski
 * Licensed under terms of GNU GPL v2 (or later). See LICENSE.
 *
 * Compile:
 *    cc -o nologd nologd.c
 * with support for journald-like socket activation:
 *    cc -o nologd -DHAVE_SYSTEMD $(pkgconfig --cflags --libs libsystemd) nologd.c
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <sys/epoll.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define NELEMS(arr) (sizeof(arr)/sizeof(arr[0]))

/* Server context */
struct Server {
     int epoll_fd;

     int dev_log_fd;
     int journal_fd;
     int stdout_fd;

     int log_fd;
};

enum {
     SOCK_DEV_LOG = 0,
     SOCK_JOURNAL_SOCKET,
     SOCK_JOURNAL_STDOUT,
};

struct {
     int type;
     char *path;
} sockets[] = {
     [SOCK_DEV_LOG]        = { SOCK_DGRAM,  "/run/systemd/journal/dev-log" },
     [SOCK_JOURNAL_SOCKET] = { SOCK_DGRAM,  "/run/systemd/journal/socket"  },
     [SOCK_JOURNAL_STDOUT] = { SOCK_STREAM, "/run/systemd/journal/stdout"  },
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
     socklen_t slen = sizeof(sa);

     fd = accept4(stdout_fd, (struct sockaddr *)&sa, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
     if (fd < 0) /* XXX not printed at all if we were daemon()ized */
	  fprintf(stderr, "accept failed: %s\n", strerror(errno));

     return fd;
}


typedef int (*process_fn)(struct Server *, char *, int);

void consume(struct Server *s, int fd, int do_close, process_fn fn)
{
     int r;
     static char buf[2048];

     do {
	  r = read(fd, &buf[0], NELEMS(buf) - 1);
	  fn(s, buf, r);
     } while (r > 0);

     if (do_close && r == 0) {
	  close(fd);
	  epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
     }
}

/* Expected format:
 *   <13>Feb  7 23:34:43 MSG
 */
int process_syslog(struct Server *s, char *buf, int len)
{
     int start = 0;
     int end = len - 1;

     if (s->log_fd == -1 || len == -1)
	  return 0;

     /* Drop numerically coded log level and facility as we perform no
      * filtering based on this information.
      */
     if (start < len && buf[start] == '<') {

	  do {
	       ++start;
	  } while (start < len && isdigit(buf[start]));

	  if (buf[start] == '>')
	       ++start;
     }

     while (end > 0 && buf[end] == '\n')
	  buf[end--] = '\0';

     write(s->log_fd, buf + start, end - start);
     write(s->log_fd, "\n", 1);

     return end - start + 1;
}

int process_journal(struct Server *s, char *buf, int len)
{
     int pos;

     if (s->log_fd == -1 || len == -1)
	  return 0;

     for (pos = 0; pos < len; pos++)
	  if (buf[pos] == '\n')
	       buf[pos] = ' ';

     write(s->log_fd, buf, len);
     write(s->log_fd, "\n", 1);

     return len + 1;
}
int process_stream(struct Server *s, char *buf, int len)
{
     if (s->log_fd == -1 || len == -1)
	  return 0;

     /* To be implemented */

     return 0;
}

void usage(void)
{
     printf("usage: %s [-d] [-f FILE] [-h]\n"
	    " -d        daemonize\n"
	    " -f FILE   drop logs to FILE\n"
	    " -h        this help screen\n",
	  progname);
}

int systemd_sock_get(struct Server *s)
{
     int n = 0;

#ifdef HAVE_SYSTEMD
     int i;

     n = sd_listen_fds(1);
     if (n < 0)
	  return 0;

     for (i = SD_LISTEN_FDS_START; i < SD_LISTEN_FDS_START + n; i++) {

	  if (sd_is_socket_unix(i, sockets[SOCK_DEV_LOG].type, -1, sockets[SOCK_DEV_LOG].path, 0) > 0) {
	       s->dev_log_fd = i;
	       continue;
	  }

	  if (sd_is_socket_unix(i, sockets[SOCK_JOURNAL_SOCKET].type, -1, sockets[SOCK_JOURNAL_SOCKET].path, 0) > 0) {
	       s->journal_fd = i;
	       continue;
	  }

	  if (sd_is_socket_unix(i, sockets[SOCK_JOURNAL_STDOUT].type, 1, sockets[SOCK_JOURNAL_STDOUT].path, 0) > 0) {
	       s->stdout_fd = i;
	       continue;
	  }
     }
#endif

     return n;
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
     int nwatching = 0;

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

     /* Ignore flush request for time being */
     signal(SIGUSR1, SIG_IGN);

     s.epoll_fd = epoll_create1(EPOLL_CLOEXEC);

     mkdir("/run/systemd", 0755);
     mkdir("/run/systemd/journal", 0755);

     systemd_sock_get(&s);

     if (s.dev_log_fd < 0)
	  s.dev_log_fd = unix_open(&s, sockets[SOCK_DEV_LOG].type, sockets[SOCK_DEV_LOG].path);
     if (s.journal_fd < 0)
	  s.journal_fd = unix_open(&s, sockets[SOCK_JOURNAL_SOCKET].type, sockets[SOCK_JOURNAL_SOCKET].path);
     if (s.stdout_fd < 0)
	  s.stdout_fd = unix_open(&s, sockets[SOCK_JOURNAL_STDOUT].type, sockets[SOCK_JOURNAL_STDOUT].path);

     if (s.dev_log_fd >= 0) {
	  fd_set_nonblock(s.dev_log_fd);
	  epoll_addwatch(&s, s.dev_log_fd);

	  symlink(sockets[SOCK_DEV_LOG].path, "/dev/log");

	  ++ nwatching;
     }

     if (s.journal_fd >= 0) {
	  fd_set_nonblock(s.journal_fd);
	  epoll_addwatch(&s, s.journal_fd);
	  ++ nwatching;
     }

     if (s.stdout_fd >= 0) {
	  epoll_addwatch(&s, s.stdout_fd);
	  ++ nwatching;
     }

     if (!nwatching) {
	  fprintf(stderr, "%s: Unable to watch on any of defined sockets.  Exiting.\n", progname);
	  exit(EXIT_FAILURE);
     }

     if (do_daemonize)
	  daemon(0, 0);

     while (1) {
	  r = epoll_wait(s.epoll_fd, &ev, 1, -1);
	  if (r < 0 && errno != EINTR) {
	       fprintf(stderr, "epoll_wait failed: %s\n", strerror(errno));
	       exit(EXIT_FAILURE);
	  }

	  if (ev.data.fd == s.stdout_fd) {
	       int newfd = unix_accept(&s, s.stdout_fd);
	       if (newfd >= 0) {
		    fd_set_nonblock(newfd);
		    epoll_addwatch(&s, newfd);
	       }
	       continue;
	  }

	  if (ev.data.fd == s.dev_log_fd) {
	       consume(&s, s.dev_log_fd, 0, process_syslog);
	  } else if (ev.data.fd == s.journal_fd) {
	       consume(&s, s.journal_fd, 0, process_journal);
	  } else {
	       /* pre-opened stdout fd */
	       consume(&s, ev.data.fd, 1, process_stream);
	  }
     }

     return EXIT_SUCCESS;
}
