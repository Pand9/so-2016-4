#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "err.h"
#undef log
#undef syserr

void syserr_(const char *fmt, ...)  
{
	va_list fmt_args;

	fprintf(stderr, "ERROR: ");

	va_start(fmt_args, fmt);
	vfprintf(stderr, fmt, fmt_args);
	va_end (fmt_args);
	fprintf(stderr," (%d; %s)\n", errno, strerror(errno));
	kill(0, SIGINT); // signal handler is supposed to do a cleanup
	exit(1);
}

#define TIME_LEN		26
#define TIMEBUF_LEN		31
char timebuf[TIME_LEN];

static
void load_current_time_()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);

	int millisec = (int)(tv.tv_usec / 1000.0);
	if (millisec >= 1000) {
		millisec -= 1000;
		tv.tv_sec++;
	}

	struct tm* tm_info = localtime(&tv.tv_sec);
	size_t pos = strftime(timebuf, TIME_LEN, "%Y:%m:%d %H:%M:%S", tm_info);

	assert(0 <= millisec && millisec < 1000);
	snprintf(&timebuf[pos], 6, ".%d", millisec);
}


void log_(const char *fmt, ...) {
	load_current_time_();
	fprintf(stderr, "%s: ", timebuf);

	va_list fmt_args;
	va_start(fmt_args, fmt);
	vfprintf(stderr, fmt, fmt_args);
	va_end(fmt_args);
	fputc('\n', stderr);
}

void log_nnl(const char *fmt, ...) {
	load_current_time_();
	fprintf(stderr, "%s: ", timebuf);
	va_list fmt_args;
	va_start(fmt_args, fmt);
	vfprintf(stderr, fmt, fmt_args);
	va_end(fmt_args);
}

/*  = log_middle + macro for prefix */
void log_first_(const char *fmt, ...) {
	load_current_time_();
	fprintf(stderr, "%s: ", timebuf);

	va_list fmt_args;
	va_start(fmt_args, fmt);
	vfprintf(stderr, fmt, fmt_args);
	va_end(fmt_args);
}

void log_middle(const char *fmt, ...) {
	va_list fmt_args;
	va_start(fmt_args, fmt);
	vfprintf(stderr, fmt, fmt_args);
	va_end(fmt_args);
}

void log_last(const char *fmt, ...) {
	va_list fmt_args;
	va_start(fmt_args, fmt);
	vfprintf(stderr, fmt, fmt_args);
	va_end(fmt_args);
	fputc('\n', stderr);
}
