/*
 * Copyright (c) 1988 Mark Nudleman
 * Copyright (c) 1988, 1993
 *	Regents of the University of California.  All rights reserved.
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

#ifndef lint
char copyright[] =
"@(#) Copyright (c) 1988 Mark Nudleman.\n\
@(#) Copyright (c) 1988, 1993
	Regents of the University of California.  All rights reserved.\n";
#endif /* not lint */

#ifndef lint
static char sccsid[] = "@(#)main.c	8.1 (Berkeley) 6/7/93";
#endif /* not lint */

#ifndef lint
static const char rcsid[] =
  "$FreeBSD$";
#endif /* not lint */

/*
 * Entry point, initialization, miscellaneous routines.
 */

#include <sys/file.h>
#include <sys/param.h>
#include <sys/types.h>

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFINEGLOBALS
#include "defrc.h"
#include "less.h"
#include "pathnames.h"

int	ispipe;
char	*current_file, *previous_file, *current_name, *next_name;
int	any_display;
int	ac;
char	**av;
int	curr_ac;

extern int	file;
extern int	cbufs;
extern int	errmsgs;

extern char	*tagfile;
extern int	tagoption;

/*
 * Edit a new file.
 * Filename "-" means standard input.
 * No filename means the "current" file, from the command line.  If called
 * with the same filename in succession, filename will be closed and reopened.
 *
 * If called with FORCE_OPEN, the file will be re-opened even if it is
 * already open.
 */
edit(filename, force_open)
	register char *filename;
{
	register int f;
	register char *m;
	off_t initial_pos, position();
	static off_t prev_pos;
	static int didpipe;
	char message[MAXPATHLEN + 50], *p;
	char *rindex(), *strerror(), *save(), *bad_file();
	extern int horiz_off, wraplines;

	if (force_open == NO_FORCE_OPEN &&
	    current_file && filename && !strcmp(filename, current_file))
		return(1);

	/* Okay since later code in this fcn() always forces redraw() */
	horiz_off = 0;
	wraplines = 0;

	initial_pos = NULL_POSITION;
	if (filename == NULL || *filename == '\0') {
		if (curr_ac >= ac) {
			error("No current file");
			return(0);
		}
		filename = save(av[curr_ac]);
	}
	else if (strcmp(filename, "#") == 0) {
		if (!previous_file || *previous_file == '\0') {
			error("no previous file");
			return(0);
		}
		filename = save(previous_file);
		initial_pos = prev_pos;
	} else
		filename = save(filename);

	/* use standard input. */
	if (!strcmp(filename, "-")) {
		if (didpipe) {
			error("can view standard input only once");
			return(0);
		}
		f = 0;
	}
	else if ((m = bad_file(filename, message, sizeof(message))) != NULL) {
		error(m);
		free(filename);
		return(0);
	}
	else if ((f = open(filename, O_RDONLY, 0)) < 0) {
		(void)snprintf(message, sizeof(message),
		   "%s: %s", filename, strerror(errno));
		error(message);
		free(filename);
		return(0);
	}

	if (isatty(f)) {
		/*
		 * Not really necessary to call this an error,
		 * but if the control terminal (for commands)
		 * and the input file (for data) are the same,
		 * we get weird results at best.
		 */
		error("Can't take input from a terminal");
		if (f > 0)
			(void)close(f);
		(void)free(filename);
		return(0);
	}

	/*
	 * We are now committed to using the new file.
	 * Close the current input file and set up to use the new one.
	 */
	if (file > 0)
		(void)close(file);
	if (previous_file != NULL)
		free(previous_file);
	previous_file = current_file;
	current_file = filename;
	pos_clear();
	prev_pos = position(TOP);
	ispipe = (f == 0);
	if (ispipe) {
		didpipe = 1;
		current_name = "stdin";
	} else
		current_name = (p = rindex(filename, '/')) ? p + 1 : filename;
	if (curr_ac >= ac)
		next_name = NULL;
	else
		next_name = av[curr_ac + 1];
	file = f;
	ch_init(cbufs, 0);

	if (isatty(STDOUT_FILENO)) {
		int no_display = !any_display;
		any_display = 1;
		if (no_display && errmsgs > 0) {
			/*
			 * We displayed some messages on error output
			 * (file descriptor 2; see error() function).
			 * Before erasing the screen contents,
			 * display the file name and wait for a keystroke.
			 */
			error(filename);
		}
		/*
		 * Indicate there is nothing displayed yet.
		 */
		if (initial_pos != NULL_POSITION)
			jump_loc(initial_pos);
		clr_linenum();
	}
	return(1);
}

/*
 * Edit the next file in the command line list.
 */
next_file(n)
	int n;
{
	extern int quit_at_eof;
	off_t position();

	if (curr_ac + n >= ac) {
		if (quit_at_eof || position(TOP) == NULL_POSITION)
			quit();
		error("No (N-th) next file");
	}
	else
		(void)edit(av[curr_ac += n], FORCE_OPEN);
}

/*
 * Edit the previous file in the command line list.
 */
prev_file(n)
	int n;
{
	if (curr_ac - n < 0)
		error("No (N-th) previous file");
	else
		(void)edit(av[curr_ac -= n], FORCE_OPEN);
}

/*
 * copy a file directly to standard output; used if stdout is not a tty.
 * the only processing is to squeeze multiple blank input lines.
 */
static
cat_file()
{
	extern int squeeze;
	register int c, empty;

	if (squeeze) {
		empty = 0;
		while ((c = ch_forw_get()) != EOI)
			if (c != '\n') {
				putchr(c);
				empty = 0;
			}
			else if (empty < 2) {
				putchr(c);
				++empty;
			}
	}
	else while ((c = ch_forw_get()) != EOI)
		putchr(c);
	flush();
}

main(argc, argv)
	int argc;
	char **argv;
{
	int envargc, argcnt;
	char *envargv[3];

	(void) setlocale(LC_ALL, "");

	/*
	 * Process command line arguments and MORE environment arguments.
	 * Command line arguments override environment arguments.
	 */
	if (envargv[1] = getenv("MORE")) {
		envargc = 2;
		envargv[0] = "more";
		envargv[2] = NULL;
		(void)option(envargc, envargv);
	}
	argcnt = option(argc, argv);
	argv += argcnt;
	argc -= argcnt;

	/*
	 * Set up list of files to be examined.
	 */
	ac = argc;
	av = argv;
	curr_ac = 0;

	init_mark();

	/*
	 * Set up terminal, etc.
	 */
	if (!isatty(STDOUT_FILENO)) {
		/*
		 * Output is not a tty.
		 * Just copy the input file(s) to output.
		 */
		if (ac < 1) {
			(void)edit("-", NOFLAGS);
			if (file >= 0)
				cat_file();
		} else {
			do {
				(void)edit((char *)NULL, FORCE_OPEN);
				if (file >= 0)
					cat_file();
			} while (++curr_ac < ac);
		}
		exit(0);
	}

	raw_mode(1);
	get_term();
	open_getchr();
	init();
	init_signals(1);

	/* select the first file to examine. */
	if (tagoption) {
		/*
		 * A -t option was given; edit the file selected by the
		 * "tags" search, and search for the proper line in the file.
		 */
		if (!tagfile || !edit(tagfile, NOFLAGS) || tagsearch())
			quit();
	}
	else if (ac < 1)
		(void)edit("-", NOFLAGS);  /* Standard input */
	else {
		/*
		 * Try all the files named as command arguments.
		 * We are simply looking for one which can be
		 * opened without error.
		 */
		do {
			(void)edit((char *)NULL, NOFLAGS);
		} while (file < 0 && ++curr_ac < ac);
	}

	if (file >= 0) {
		/*
		 * Don't call rcfiles() until now so that people who put
		 * wierd things (like "forw_scroll") in their rc file don't
		 * cause us to SEGV.
		 */
		rcfiles();
		commands();
	}
	quit();
	/*NOTREACHED*/
}

/*
 * Copy a string to a "safe" place
 * (that is, to a buffer allocated by malloc).
 */
char *
save(s)
	char *s;
{
	char *p, *strcpy();

	p = malloc((u_int)strlen(s)+1);
	if (p == NULL)
	{
		error("cannot allocate memory");
		quit();
	}
	return(strcpy(p, s));
}

/*
 * Exit the program.
 */
quit()
{
	/*
	 * Put cursor at bottom left corner, clear the line,
	 * reset the terminal modes, and exit.
	 */
	lower_left();
	clear_eol();
	deinit();
	flush();
	raw_mode(0);
	exit(0);
}

/*
 * Read in from each of the three rc files - default, system, user.
 * Calls handle_error() directly to report errors.
 */
rcfiles()
{
	FILE *fd;
	char fbuf[MAXPATHLEN + 1];
	char *c;
	int readrc();
	int savederrno;
	static int str_read();

	/* The default builtin rc file */
	if ((c = getenv("HOME")) &&
	    strlen(c) + strlen(_PATH_DEFRC) + 1 < MAXPATHLEN) {
		sprintf(fbuf, "%s/%s", c, _PATH_DEFRC);
		fd = fopen(fbuf, "r");
		savederrno = errno;
		if (!fd) {
			if (!access(_PATH_SYSMORERC, F_OK)) {
				SETERRSTR(E_EXTERN, "unable to read %s: %s",
				    _PATH_SYSMORERC, strerror(savederrno));
				handle_error();
			}
			/* We'd better have some type of default keys!! */
			goto use_builtin_defrc;
		} else {
			readrc(fd);
			fclose(fd);
		}
	} else {
use_builtin_defrc:
		fd = fropen(DEFRC, str_read);
		readrc(fd);
		fclose(fd);
	}

	/* The system rc file */
	fd = fopen(_PATH_SYSMORERC, "r");
	savederrno = errno;
	if (!fd) {
		if (!access(_PATH_SYSMORERC, F_OK)) {
			SETERRSTR(E_EXTERN, "unable to read %s: %s",
			    _PATH_SYSMORERC, strerror(savederrno));
			handle_error();
		} else
			; /* non-existant => non-error */
	} else {
		readrc(fd);
		fclose(fd);
	}

	/* The user rc file */
	if ((c = getenv("HOME")) &&
	    strlen(c) + strlen(_PATH_RC) + 1 < MAXPATHLEN) {
		sprintf(fbuf, "%s/%s", c, _PATH_RC);
		fd = fopen(fbuf, "r");
		savederrno = errno;
		if (!fd) {
			if (!access(fbuf, F_OK)) {
				SETERRSTR(E_EXTERN,
				    "unable to read %s: %s", fbuf,
				    strerror(savederrno));
				handle_error();
			} else
				; /* non-existant => non-error */
		} else {
			readrc(fd);
			fclose(fd);
		}
	}
}

/*
 * Read-in an rc file from a fd.  Calls handle_error() directly to handle
 * errors.
 *
 * This really belongs in ncommand.c, but that file is already 33292 bytes
 * long.
 */
readrc(fd)
	FILE *fd;
{
	char *bufptr, *buf;
	size_t len;
	int strlenbuf;

	buf = NULL;
	strlenbuf = 0;
	while (bufptr = fgetln(fd, &len)) {
		if (!len)
			continue;  /* ??? */
		if (*bufptr == '#')
			continue;  /* skip comments */
		if (!(buf = reallocf(buf, strlenbuf + len + 1))) {
			SETERR(E_MALLOC);
			handle_error();
			if (strlenbuf + len < 1024)
				return;  /* major memory shortage... */
			continue;
		}
		memcpy (buf + strlenbuf, bufptr, len);
		buf[len + strlenbuf] = '\0';
		if (len > 1 && buf[strlenbuf + len - 2] == '\\') {
			/* line continuation */
			buf[strlenbuf + len - 2] = '\0';
			strlenbuf = strlen(buf);
			continue;
		}
		if (buf[len + strlenbuf - 1] == '\n')
			buf[len + strlenbuf - 1] = '\0';
		if (command(buf))
			handle_error();
		free(buf);
		buf = NULL;
		strlenbuf = 0;
	}
}

/*
 * Read from the NUL-terminated cookie.  Non-reentrant: keeps a static pointer
 * to the current position in the cookie.  Used for funopen().
 */
static int
str_read(cookie, buf, len)
	void *cookie;
	char *buf;
	size_t len;
{
	static char *curpos;
	static int cooklen;
	static char *lastcook;

	if (lastcook != cookie) {
		/* begin working on a new cookie */
		curpos = cookie;
		lastcook = cookie;
		cooklen = strlen(cookie);
	}

	if (curpos + len > lastcook + cooklen) {
		ssize_t r;
		memcpy(buf, curpos, r = (cooklen - (curpos - lastcook)));
		curpos = cookie + cooklen;
		return (int) r;
	} else {
		memcpy(buf, curpos, len);
		curpos += len;
		return (int) len;
	}
}
