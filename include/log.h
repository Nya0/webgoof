#pragma once
#include <stddef.h>
#include <stdio.h>

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_DEBUG
#endif

typedef enum {
	LOG_NONE,
	LOG_ERROR,
	LOG_WARN,
	LOG_INFO,
	LOG_DEBUG,
} log_level_t;

void log_init(FILE *out);
void log_set_verbosity(log_level_t level);

void log_message(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...);

#define LOG(level, fmt, ...)                                                          \
	do {                                                                              \
		if ((level) <= LOG_LEVEL)                                                     \
			log_message((level), __FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__); \
	} while (0)
