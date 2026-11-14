#ifndef COREUTILS_H
#define COREUTILS_H

/*
 * Bharat coreutils: the everyday commands (ls, cp, grep, ...) in one
 * program. /bin/ls, /bin/cp ... are links to /bin/coreutils, which looks
 * at the name it was started with (argv[0]) to know what to do.
 * One program instead of 60 keeps the system small.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* the name this command was called with, for error messages */
extern const char *prog;

/* print "prog: message" to stderr; returns 1 so you can "return fail(...)" */
int fail(const char *fmt, ...);

/*
 * Simple option reader: for "ls -la x" it sets flags['l'] and flags['a']
 * and returns the index of the first non-option ("x"). "--" ends options.
 * Options listed in with_value ("n" for "head -n 5") take the next word,
 * stored in values['n'].
 */
int options(int argc, char **argv, const char *allowed, const char *with_value,
            int flags[128], char *values[128]);

/* "1.5K", "23M" ... for -h */
void human(long long bytes, char *out, size_t size);

/* join a folder and a name with exactly one '/' */
void join(const char *dir, const char *name, char *out, size_t size);

/* copy everything from one open file to another; 0 = ok */
int copy_fd(int in, int out);

/* every command: int cmd_NAME(int argc, char **argv) */
#define COMMAND(name) int cmd_##name(int argc, char **argv)

#endif
