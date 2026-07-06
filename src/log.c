// log.c
#include "log.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define COLOR_RESET "\033[0m"

static FILE *g_out;
static log_level_t g_level = LOG_DEBUG;
static int g_use_color = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(FILE *out) {
	g_out = out ? out : stderr;
	// only colorize when writing to a real terminal, and respect NO_COLOR
	g_use_color = isatty(fileno(g_out)) && getenv("NO_COLOR") == NULL;
}

void log_set_level(log_level_t lvl) { g_level = lvl; }

static const char *level_str(log_level_t lvl) {
	switch (lvl) {
	case LOG_DEBUG:
		return "DEBUG";
	case LOG_INFO:
		return "INFO";
	case LOG_WARN:
		return "WARN";
	case LOG_ERROR:
		return "ERROR";
	}

	return "LOG";
}

static const char *level_color(log_level_t lvl) {
	switch (lvl) {
	case LOG_DEBUG:
		return "\033[36m"; // cyan
	case LOG_INFO:
		return "\033[32m"; // green
	case LOG_WARN:
		return "\033[33m"; // yellow
	case LOG_ERROR:
		return "\033[31m"; // red
	}

	return "";
}

#include <time.h>

static void ts_hhmmss_utc(char out[9]) {
	time_t t = time(NULL);
	struct tm tmv;
	gmtime_r(&t, &tmv);
	strftime(out, 9, "%H:%M:%S", &tmv);
}

void log_message(log_level_t lvl, const char *file, int line, const char *func, const char *fmt, ...) {
	if (lvl < g_level)
		return;

	char ts[9];
	ts_hhmmss_utc(ts);

	char msg[2048];

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&g_lock);
	if (g_use_color) {
		fprintf(g_out, "%s[%s][%s][%s:%d %s]: %s%s\n", level_color(lvl), ts, level_str(lvl), file, line, func, msg, COLOR_RESET);
	} else {
		fprintf(g_out, "[%s][%s][%s:%d %s]: %s\n", ts, level_str(lvl), file, line, func, msg);
	}
	fflush(g_out);
	pthread_mutex_unlock(&g_lock);
}
