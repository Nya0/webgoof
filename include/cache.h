#pragma once
#include "uthash.h"
#include <netinet/in.h>
#include <stddef.h>
#include <sys/types.h>

struct cached_file {
	UT_hash_handle hh;
	char path[256];
	int fd;
	off_t size;
	time_t last_access;
};

struct file_cache {
	struct cached_file *entries;
	pthread_rwlock_t lock;
	int max_entries;
	int entry_count;
};

void cache_init(int max_entries);
struct cached_file *cache_lookup(const char *path);

// returns 0 on success <0 on error
int cache_insert(const char *path, int fd, off_t size);