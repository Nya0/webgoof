

#include "cache.h"
#include "uthash.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static struct file_cache cache = {0};
static bool cache_initialized = false;

void cache_init(int max_entries) {
	cache.max_entries = max_entries;
	cache.entry_count = 0;
	cache.entries = NULL;
	pthread_rwlock_init(&cache.lock, NULL);
	cache_initialized = true;
}

struct cached_file *cache_lookup(const char *path) {
	struct cached_file *entry = NULL;

	pthread_rwlock_rdlock(&cache.lock);
	HASH_FIND_STR(cache.entries, path, entry);
	if (entry) {
		entry->last_access = time(NULL);
	}
	pthread_rwlock_unlock(&cache.lock);

	return entry;
}

int cache_insert(const char *path, char *content, off_t size) {
	if (!cache_initialized) {
		return -1;
	}

	struct cached_file *entry = malloc(sizeof *entry);
	if (!entry) {
		return -1;
	}

	snprintf(entry->path, sizeof(entry->path), "%s", path);
	entry->size = size;
	entry->content = content;
	entry->last_access = time(NULL);

	pthread_rwlock_wrlock(&cache.lock);
	HASH_ADD_STR(cache.entries, path, entry);
	cache.entry_count++;
	pthread_rwlock_unlock(&cache.lock);

	return 0;
}