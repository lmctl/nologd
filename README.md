# nologd(8) - ligthtweight non-logger

# Synopsis

nologd [-d] [-f FILE] [-h]

# Description

nologd is simple utility that consumes data sent to well-known log
sockets.  Following sources of data can be consumed:

* /dev/log - used by POSIX's syslog(3)
* /run/systemd/journal/socket - "explicit" systemd logging (sd_journal functions)
* /run/systemd/journal/stdout - "implicit" systemd logging (stdout/stderr of service processes)

By default all the received data is discarded.

# Options

* -d        daemonize
* -f FILE   drop logs to FILE
* -h        usage information

# See also

syslog(3), sd_journal_sendv(3), sd_journal_printv(3), systemd-journald(8)

# Bugs

This program was created as a benchmark, not real journald replacement.

Most of journald features are unimplemented, including but not limited to:

* systemd watchdog interface (WatchdogSec can not be specified as an
  configuration option)

* ability to receive log messages from attached fd (used for "big
  messages")
