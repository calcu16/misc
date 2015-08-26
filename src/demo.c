#define MAIN
/*
 * Copyright (c) 1980, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef __linux__
#define _BSD_SOURCE 1
#define _POSIX_C_SOURCE 3
#endif

#ifndef __FBSDID
#define __FBSDID(x)  static const char freebsd_id[] = { x }
#endif

#include <sys/cdefs.h>

#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#if !defined(__APPLE__) && !defined(__linux__)
#include <libutil.h>
#endif /* !__APPLE__ */
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#endif /* __APPLE__ */
#ifdef __linux__
#include <utmp.h>
#include <pty.h>
#endif

#ifndef __dead2
#define __dead2		__attribute__((__noreturn__))
#endif

FILE	*dscript;
int	master, slave;
int	child;
const char *fname;
char	lastKey;
int	qflg, ttyflg, dflg;
struct timeval period;
struct	termios tt;

void	done(int) __dead2;
void	dooutput(void);
void	doshell(char **);
void	fail(void);
void	finish(void);
void	usage(void);
void	writedemottosubprocess(void);
void	writetosubprocess(const char*, int);
void	writetouser(const char*, int);
void	setperiod(double);

int
main(int argc, char *argv[])
{
	int cc;
	struct termios rtt;
	struct winsize win;
	int ch, n;
	struct timeval start, elapsed, timeout, *timeoutp;
	char obuf[BUFSIZ];
	char ibuf[BUFSIZ];
	fd_set rfd;
	setperiod(30.0);

	dflg = 1;
	lastKey = '\r';
	while ((ch = getopt(argc, argv, "aqkt:")) != -1) {
		switch(ch) {
		case 'q':
			qflg = 1;
			break;
		case 't':
			setperiod(atof(optarg));
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0) {
		fname = argv[0];
		argv++;
		argc--;
	} else {
		usage();
	}

	if ((dscript = fopen(fname, "r")) == NULL)
		err(1, "%s", fname);

	if ((ttyflg = isatty(STDIN_FILENO))) {
		if (tcgetattr(STDIN_FILENO, &tt) == -1)
			err(1, "tcgetattr");
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) == -1)
			err(1, "ioctl");
		if (openpty(&master, &slave, NULL, &tt, &win) == -1)
			err(1, "openpty");
	} else {
		if (openpty(&master, &slave, NULL, NULL, NULL) == -1)
			err(1, "openpty");
	}

	if (!qflg) {
		(void)printf("Demo started, input file is %s\n", fname);
	}
	if (ttyflg) {
		rtt = tt;
		cfmakeraw(&rtt);
		rtt.c_lflag &= ~ECHO;
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &rtt);
	}

	child = fork();
	if (child < 0) {
		warn("fork");
		done(1);
	}
	if (child == 0)
		doshell(argv);

	(void)close(slave);


	timeout = period;
	gettimeofday(&start, NULL);
	FD_ZERO(&rfd);
	for (;;) {
		FD_SET(master, &rfd);
		FD_SET(STDIN_FILENO, &rfd);
		if (timeout.tv_sec || timeout.tv_usec)
			timeoutp = &timeout;
		else
			timeoutp = NULL;
		n = select(master + 1, &rfd, 0, 0, timeoutp);
		if (n < 0 && errno != EINTR)
			break;
		if (n > 0 && FD_ISSET(STDIN_FILENO, &rfd)) {
			cc = read(STDIN_FILENO, ibuf, BUFSIZ);
			writetosubprocess(ibuf, cc);
		}
		if (n > 0 && FD_ISSET(master, &rfd)) {
			cc = read(master, obuf, sizeof (obuf));
			if (cc <= 0)
				break;
			writetouser(obuf, cc);
		}
		gettimeofday(&elapsed, NULL);
		timersub(&elapsed, &start, &elapsed);
		if (timercmp(&elapsed, &timeout, >)) {
			timeout = period;
		} else {
			timersub(&timeout, &elapsed, &timeout);
			if (timercmp(&timeout, &period, >))
				timeout = period;
		}
		gettimeofday(&start, NULL);
	}
	finish();
	done(0);
}


void setperiod(double intervaltime)
{
	double intpart, fracpart;
	if (intervaltime < 0)
		err(1, "invalid time interval %f", intervaltime);
	fracpart = modf(intervaltime, &intpart);
	period.tv_sec = (int) intpart;
	period.tv_usec = (int) (fracpart * 1000000.0);
}

void
writedemotosubprocess(void)
{
	int value;
	char temp;
	if (dflg) {
		while ((value = getc(dscript)) != EOF && value != '\n') {
			temp = (char)value;
			(void)write(master, &temp, 1);
		}
		dflg = value != EOF;
	}
}

void
writetosubprocess(const char* buf, int cc)
{
        int i;
	if (cc <= 0)
	       return;
        for (i = 0; i < cc; ++i) {
        	if (lastKey == '\r' && buf[i] == '\r') {
        		writedemotosubprocess();
        	}
		lastKey = buf[i];
        	(void)write(master, buf + i, 1);
        }
}


void
writetouser(const char* buf, int cc)
{
	if (cc <= 0)
	       return;
	(void)write(STDOUT_FILENO, buf, cc);
}


void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: demo [-akq] [-t time] [file [command ...]]\n");
	exit(1);
}


void
finish(void)
{
	pid_t pid;
	int die, e, status;

	die = e = 0;
	while ((pid = wait3(&status, WNOHANG, 0)) > 0)
		if (pid == child) {
			die = 1;
			if (WIFEXITED(status))
				e = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				e = WTERMSIG(status);
			else /* can't happen */
				e = 1;
		}

	if (die)
		done(e);
}


void
doshell(char **av)
{
	const char *shell;

	shell = getenv("SHELL");
	if (shell == NULL)
		shell = _PATH_BSHELL;

	(void)close(master);
	(void)fclose(dscript);
	login_tty(slave);
	if (av[0]) {
		execvp(av[0], av);
		warn("%s", av[0]);
	} else {
		execl(shell, shell, "-i", (char *)NULL);
		warn("%s", shell);
	}
	fail();
}


void
fail(void)
{
	(void)kill(0, SIGTERM);
	done(1);
}


void
done(int eno)
{
	if (ttyflg)
		(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &tt);
	if (dscript) {
		if (!qflg) {
			(void)printf("\nDemo done\n");
		}
		(void)fclose(dscript);
	}
	if (master)
		(void)close(master);
	exit(eno);
}
