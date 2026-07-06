#pragma once
#include <stddef.h>
#include <stdio.h>

typedef enum { LOG_DEBUG,
	           LOG_INFO,
	           LOG_WARN,
	           LOG_ERROR } log_level_t;

void log_init(FILE *out);
void log_set_level(log_level_t level);

void log_message(log_level_t level, const char *file, int line, const char *func, const char *fmt, ...);

#define LOG(level, fmt, ...) \
	log_message((level), __FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__)
