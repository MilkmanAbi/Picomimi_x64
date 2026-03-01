/**
 * Picomimi-x64 Slab Allocator Header
 * 
 * Linux-compatible kmalloc/kfree interface
 */
#ifndef _MM_SLAB_H
#define _MM_SLAB_H

#include <kernel/types.h>

// Slab cache structure
typedef struct kmem_cache {
    const char *name;
    size_t object_size;
    size_t align;
    u32 flags;
    void (*ctor)(void *);
    void (*dtor)(void *);
    struct list_head slabs_full;
    struct list_head slabs_partial;
    struct list_head slabs_free;
    spinlock_t lock;
    u64 allocated;
    u64 freed;
} kmem_cache_t;

// Slab flags
#define SLAB_HWCACHE_ALIGN  0x00002000
#define SLAB_PANIC          0x00040000
#define SLAB_RECLAIM_ACCOUNT 0x00020000

// Initialization
void slab_init(void);

// kmalloc/kfree (Linux-compatible)
void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void *kcalloc(size_t n, size_t size, gfp_t flags);
void *krealloc(void *ptr, size_t new_size, gfp_t flags);
void kfree(void *ptr);
size_t ksize(const void *ptr);

// vmalloc/vfree (for large allocations)
void *vmalloc(size_t size);
void *vzalloc(size_t size);
void vfree(void *ptr);

// kmem_cache interface
kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align,
                                 u32 flags, void (*ctor)(void *));
void kmem_cache_destroy(kmem_cache_t *cache);
void *kmem_cache_alloc(kmem_cache_t *cache, gfp_t flags);
void kmem_cache_free(kmem_cache_t *cache, void *obj);

#endif // _MM_SLAB_H
