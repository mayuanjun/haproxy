/*
 * include/common/memory.h
 * Memory management definitions..
 *
 * Copyright (C) 2000-2014 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _COMMON_MEMORY_H
#define _COMMON_MEMORY_H

#include <string.h>

#include <haproxy/base.h>
#include <haproxy/freq_ctr.h>
#include <haproxy/list.h>
#include <haproxy/pool-os.h>
#include <haproxy/pool-t.h>
#include <haproxy/thread.h>

extern struct pool_cache_head pool_cache[][MAX_BASE_POOLS];
extern struct list pool_lru_head[MAX_THREADS];
extern THREAD_LOCAL size_t pool_cache_bytes;   /* total cache size */
extern THREAD_LOCAL size_t pool_cache_count;   /* #cache objects   */
extern struct pool_head pool_base_start[MAX_BASE_POOLS];
extern unsigned int pool_base_count;

/* poison each newly allocated area with this byte if >= 0 */
extern int mem_poison_byte;

/* Allocates new entries for pool <pool> until there are at least <avail> + 1
 * available, then returns the last one for immediate use, so that at least
 * <avail> are left available in the pool upon return. NULL is returned if the
 * last entry could not be allocated. It's important to note that at least one
 * allocation is always performed even if there are enough entries in the pool.
 * A call to the garbage collector is performed at most once in case malloc()
 * returns an error, before returning NULL.
 */
void *__pool_refill_alloc(struct pool_head *pool, unsigned int avail);
void *pool_refill_alloc(struct pool_head *pool, unsigned int avail);

/* Try to find an existing shared pool with the same characteristics and
 * returns it, otherwise creates this one. NULL is returned if no memory
 * is available for a new creation.
 */
struct pool_head *create_pool(char *name, unsigned int size, unsigned int flags);
void create_pool_callback(struct pool_head **ptr, char *name, unsigned int size);

/* This registers a call to create_pool_callback(ptr, name, size) */
#define REGISTER_POOL(ptr, name, size)  \
	INITCALL3(STG_POOL, create_pool_callback, (ptr), (name), (size))

/* This macro declares a pool head <ptr> and registers its creation */
#define DECLARE_POOL(ptr, name, size)   \
	struct pool_head *(ptr) = NULL; \
	REGISTER_POOL(&ptr, name, size)

/* This macro declares a static pool head <ptr> and registers its creation */
#define DECLARE_STATIC_POOL(ptr, name, size) \
	static struct pool_head *(ptr);      \
	REGISTER_POOL(&ptr, name, size)

/* Dump statistics on pools usage.
 */
void dump_pools_to_trash();
void dump_pools(void);
int pool_total_failures();
unsigned long pool_total_allocated();
unsigned long pool_total_used();

/*
 * This function frees whatever can be freed in pool <pool>.
 */
void pool_flush(struct pool_head *pool);

/*
 * This function frees whatever can be freed in all pools, but respecting
 * the minimum thresholds imposed by owners.
 *
 * <pool_ctx> is used when pool_gc is called to release resources to allocate
 * an element in __pool_refill_alloc. It is important because <pool_ctx> is
 * already locked, so we need to skip the lock here.
 */
void pool_gc(struct pool_head *pool_ctx);

/*
 * This function destroys a pull by freeing it completely.
 * This should be called only under extreme circumstances.
 */
void *pool_destroy(struct pool_head *pool);
void pool_destroy_all();

/* returns the pool index for pool <pool>, or -1 if this pool has no index */
static inline ssize_t pool_get_index(const struct pool_head *pool)
{
	size_t idx;

	idx = pool - pool_base_start;
	if (idx >= MAX_BASE_POOLS)
		return -1;
	return idx;
}

/* returns true if the pool is considered to have too many free objects */
static inline int pool_is_crowded(const struct pool_head *pool)
{
	return pool->allocated >= swrate_avg(pool->needed_avg + pool->needed_avg / 4, POOL_AVG_SAMPLES) &&
	       (int)(pool->allocated - pool->used) >= pool->minavail;
}

#ifdef CONFIG_HAP_LOCKLESS_POOLS

/* Tries to retrieve an object from the local pool cache corresponding to pool
 * <pool>. Returns NULL if none is available.
 */
static inline void *__pool_get_from_cache(struct pool_head *pool)
{
	ssize_t idx = pool_get_index(pool);
	struct pool_cache_item *item;
	struct pool_cache_head *ph;

	/* pool not in cache */
	if (idx < 0)
		return NULL;

	ph = &pool_cache[tid][idx];
	if (LIST_ISEMPTY(&ph->list))
		return NULL; // empty

	item = LIST_NEXT(&ph->list, typeof(item), by_pool);
	ph->count--;
	pool_cache_bytes -= ph->size;
	pool_cache_count--;
	LIST_DEL(&item->by_pool);
	LIST_DEL(&item->by_lru);
#ifdef DEBUG_MEMORY_POOLS
	/* keep track of where the element was allocated from */
	*POOL_LINK(pool, item) = (void *)pool;
#endif
	return item;
}

/*
 * Returns a pointer to type <type> taken from the pool <pool_type> if
 * available, otherwise returns NULL. No malloc() is attempted, and poisonning
 * is never performed. The purpose is to get the fastest possible allocation.
 */
static inline void *__pool_get_first(struct pool_head *pool)
{
	struct pool_free_list cmp, new;

	cmp.seq = pool->seq;
	__ha_barrier_load();

	cmp.free_list = pool->free_list;
	do {
		if (cmp.free_list == NULL)
			return NULL;
		new.seq = cmp.seq + 1;
		__ha_barrier_load();
		new.free_list = *POOL_LINK(pool, cmp.free_list);
	} while (HA_ATOMIC_DWCAS((void *)&pool->free_list, (void *)&cmp, (void *)&new) == 0);
	__ha_barrier_atomic_store();

	_HA_ATOMIC_ADD(&pool->used, 1);
#ifdef DEBUG_MEMORY_POOLS
	/* keep track of where the element was allocated from */
	*POOL_LINK(pool, cmp.free_list) = (void *)pool;
#endif
	return cmp.free_list;
}

static inline void *pool_get_first(struct pool_head *pool)
{
	void *ret;

	if (likely(ret = __pool_get_from_cache(pool)))
		return ret;

	ret = __pool_get_first(pool);
	return ret;
}
/*
 * Returns a pointer to type <type> taken from the pool <pool_type> or
 * dynamically allocated. In the first case, <pool_type> is updated to point to
 * the next element in the list. No memory poisonning is ever performed on the
 * returned area.
 */
static inline void *pool_alloc_dirty(struct pool_head *pool)
{
	void *p;

	if (likely(p = __pool_get_from_cache(pool)))
		return p;

	if ((p = __pool_get_first(pool)) == NULL)
		p = __pool_refill_alloc(pool, 0);
	return p;
}

/*
 * Returns a pointer to type <type> taken from the pool <pool_type> or
 * dynamically allocated. In the first case, <pool_type> is updated to point to
 * the next element in the list. Memory poisonning is performed if enabled.
 */
static inline void *pool_alloc(struct pool_head *pool)
{
	void *p;

	p = pool_alloc_dirty(pool);
	if (p && mem_poison_byte >= 0) {
		memset(p, mem_poison_byte, pool->size);
	}

	return p;
}

/* Locklessly add item <ptr> to pool <pool>, then update the pool used count.
 * Both the pool and the pointer must be valid. Use pool_free() for normal
 * operations.
 */
static inline void __pool_free(struct pool_head *pool, void *ptr)
{
	void **free_list = pool->free_list;

	_HA_ATOMIC_SUB(&pool->used, 1);

	if (unlikely(pool_is_crowded(pool))) {
		pool_free_area(ptr, pool->size + POOL_EXTRA);
		_HA_ATOMIC_SUB(&pool->allocated, 1);
	} else {
		do {
			*POOL_LINK(pool, ptr) = (void *)free_list;
			__ha_barrier_store();
		} while (!_HA_ATOMIC_CAS(&pool->free_list, &free_list, ptr));
		__ha_barrier_atomic_store();
	}
	swrate_add(&pool->needed_avg, POOL_AVG_SAMPLES, pool->used);
}

void pool_evict_from_cache();

/* Frees an object to the local cache, possibly pushing oldest objects to the
 * global pool.
 */
static inline void pool_put_to_cache(struct pool_head *pool, void *ptr, ssize_t idx)
{
	struct pool_cache_item *item = (struct pool_cache_item *)ptr;
	struct pool_cache_head *ph = &pool_cache[tid][idx];

	LIST_ADD(&ph->list, &item->by_pool);
	LIST_ADD(&pool_lru_head[tid], &item->by_lru);
	ph->count++;
	pool_cache_count++;
	pool_cache_bytes += ph->size;

	if (unlikely(pool_cache_bytes > CONFIG_HAP_POOL_CACHE_SIZE))
		pool_evict_from_cache(pool, ptr, idx);
}
/*
 * Puts a memory area back to the corresponding pool.
 * Items are chained directly through a pointer that
 * is written in the beginning of the memory area, so
 * there's no need for any carrier cell. This implies
 * that each memory area is at least as big as one
 * pointer. Just like with the libc's free(), nothing
 * is done if <ptr> is NULL.
 */
static inline void pool_free(struct pool_head *pool, void *ptr)
{
        if (likely(ptr != NULL)) {
		ssize_t idx __maybe_unused;

#ifdef DEBUG_MEMORY_POOLS
		/* we'll get late corruption if we refill to the wrong pool or double-free */
		if (*POOL_LINK(pool, ptr) != (void *)pool)
			*DISGUISE((volatile int *)0) = 0;
#endif
		if (mem_poison_byte >= 0)
			memset(ptr, mem_poison_byte, pool->size);

		/* put the object back into the cache only if there are not too
		 * many objects yet in this pool (no more than half of the cached
		 * is used or this pool uses no more than 1/8 of the cache size).
		 */
		idx = pool_get_index(pool);
		if (idx >= 0 &&
		    (pool_cache_bytes <= CONFIG_HAP_POOL_CACHE_SIZE * 3 / 4 ||
		     pool_cache[tid][idx].count < 16 + pool_cache_count / 8)) {
			pool_put_to_cache(pool, ptr, idx);
			return;
		}

		__pool_free(pool, ptr);
	}
}

#else /* CONFIG_HAP_LOCKLESS_POOLS */
/*
 * Returns a pointer to type <type> taken from the pool <pool_type> if
 * available, otherwise returns NULL. No malloc() is attempted, and poisonning
 * is never performed. The purpose is to get the fastest possible allocation.
 */
static inline void *__pool_get_first(struct pool_head *pool)
{
	void *p;

	if ((p = pool->free_list) != NULL) {
		pool->free_list = *POOL_LINK(pool, p);
		pool->used++;
#ifdef DEBUG_MEMORY_POOLS
		/* keep track of where the element was allocated from */
		*POOL_LINK(pool, p) = (void *)pool;
#endif
	}
	return p;
}

static inline void *pool_get_first(struct pool_head *pool)
{
	void *ret;

	HA_SPIN_LOCK(POOL_LOCK, &pool->lock);
	ret = __pool_get_first(pool);
	HA_SPIN_UNLOCK(POOL_LOCK, &pool->lock);
	return ret;
}
/*
 * Returns a pointer to type <type> taken from the pool <pool_type> or
 * dynamically allocated. In the first case, <pool_type> is updated to point to
 * the next element in the list. No memory poisonning is ever performed on the
 * returned area.
 */
static inline void *pool_alloc_dirty(struct pool_head *pool)
{
	void *p;

	HA_SPIN_LOCK(POOL_LOCK, &pool->lock);
	if ((p = __pool_get_first(pool)) == NULL)
		p = __pool_refill_alloc(pool, 0);
	HA_SPIN_UNLOCK(POOL_LOCK, &pool->lock);
	return p;
}

/*
 * Returns a pointer to type <type> taken from the pool <pool_type> or
 * dynamically allocated. In the first case, <pool_type> is updated to point to
 * the next element in the list. Memory poisonning is performed if enabled.
 */
static inline void *pool_alloc(struct pool_head *pool)
{
	void *p;

	p = pool_alloc_dirty(pool);
	if (p && mem_poison_byte >= 0) {
		memset(p, mem_poison_byte, pool->size);
	}

	return p;
}

/* unconditionally stores the object as-is into the global pool. The object
 * must not be NULL. Use pool_free() instead.
 */
static inline void __pool_free(struct pool_head *pool, void *ptr)
{
#ifndef DEBUG_UAF /* normal pool behaviour */
	HA_SPIN_LOCK(POOL_LOCK, &pool->lock);
	pool->used--;
	if (pool_is_crowded(pool)) {
		pool_free_area(ptr, pool->size + POOL_EXTRA);
		pool->allocated--;
	} else {
		*POOL_LINK(pool, ptr) = (void *)pool->free_list;
		pool->free_list = (void *)ptr;
	}
	swrate_add(&pool->needed_avg, POOL_AVG_SAMPLES, pool->used);
	HA_SPIN_UNLOCK(POOL_LOCK, &pool->lock);
#else  /* release the entry for real to detect use after free */
	/* ensure we crash on double free or free of a const area*/
	*(uint32_t *)ptr = 0xDEADADD4;
	pool_free_area(ptr, pool->size + POOL_EXTRA);
	HA_SPIN_LOCK(POOL_LOCK, &pool->lock);
	pool->allocated--;
	pool->used--;
	swrate_add(&pool->needed_avg, POOL_AVG_SAMPLES, pool->used);
	HA_SPIN_UNLOCK(POOL_LOCK, &pool->lock);
#endif /* DEBUG_UAF */
}

/*
 * Puts a memory area back to the corresponding pool.
 * Items are chained directly through a pointer that
 * is written in the beginning of the memory area, so
 * there's no need for any carrier cell. This implies
 * that each memory area is at least as big as one
 * pointer. Just like with the libc's free(), nothing
 * is done if <ptr> is NULL.
 */
static inline void pool_free(struct pool_head *pool, void *ptr)
{
        if (likely(ptr != NULL)) {
#ifdef DEBUG_MEMORY_POOLS
		/* we'll get late corruption if we refill to the wrong pool or double-free */
		if (*POOL_LINK(pool, ptr) != (void *)pool)
			*DISGUISE((volatile int *)0) = 0;
#endif
		if (mem_poison_byte >= 0)
			memset(ptr, mem_poison_byte, pool->size);

		__pool_free(pool, ptr);
	}
}
#endif /* CONFIG_HAP_LOCKLESS_POOLS */
#endif /* _COMMON_MEMORY_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
