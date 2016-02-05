# nologd(8) - ligthtweight non-logger

# Synopsis

nologd [-d] [-f FILE] [-h]

# Description

nologd is replacement for journald daemon that minimizes amount of log processing.

# Options

* -d        daemonize
* -f FILE   drop logs to FILE
* -h        display help screen

# See also

systemd-journald(8)

# Bugs

This program was created as a benchmark, not real journald replacement.

Most of journald features are unimplemented.

nologd(8) does not support systemd watchdog interface (WatchdogSec can not be specified as an option).
