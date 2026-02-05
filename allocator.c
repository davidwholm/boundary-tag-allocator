#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define HEAP_SIZE  4096 /* Heap size in bytes */

void error(char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

void *Mmap(size_t length, int prot, int flags) {
    void *res;

    if ((res = mmap(NULL, length, prot, flags, 0, 0)) == MAP_FAILED) {
        error("mmap");
    }

    return res;
}

void Munmap(void *ptr) {
    if (munmap(ptr, HEAP_SIZE) < 0) {
        error("munmap");
    }
}

struct allocator_t {
    char *heap;
    char *top;
};

typedef struct allocator_t allocator_t;

void allocator_init(allocator_t *a) {
    a->heap = Mmap(HEAP_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON);
    a->top  = a->heap;
}

void allocator_deinit(allocator_t *a) {
    Munmap(a->heap);
    a->heap = a->top = NULL;
}

void reset(allocator_t *a) {
    a->top = a->heap;
}

void *allocate(allocator_t *a, size_t size) {
    void *res;

    /* No 0-allocations */
    if (size == 0) {
      return NULL;
    }

    /* No space for requested size */    
    if (a->top + size > a->heap + HEAP_SIZE) {
        return NULL;
    }

    res = a->top;
    a->top += size;

    return res;
}

void test_fixed_size_blocks(allocator_t *a) {
    size_t allocations = 0;
    const size_t fixed_size = 64;

    while (allocate(a, fixed_size) != NULL) {
        allocations++;
    }

    assert(allocations == HEAP_SIZE/fixed_size);
}

void test_allocate_zero(allocator_t *a) {
    assert(allocate(a, 0) == NULL);
}

void test_read_write(allocator_t *a) {
    int *n = allocate(a, sizeof(int));

    *n = 23;

    assert(*n == 23);
}

void test_no_overlap(allocator_t *a) {
    char *n1 = allocate(a, sizeof(uint64_t));
    char *n2 = allocate(a, sizeof(uint64_t));

    assert(n1 + sizeof(uint64_t) == n2);
}

int main() {
    allocator_t a;
    allocator_init(&a);

    test_fixed_size_blocks(&a);
    reset(&a);
    
    test_allocate_zero(&a);
    reset(&a);

    test_read_write(&a);
    reset(&a);

    test_no_overlap(&a);
    reset(&a);

    allocator_deinit(&a);
}
