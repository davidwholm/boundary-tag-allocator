#include <alloca.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define DBG(fmt, ...) fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__)

static const uint16_t HEAP_SIZE = 4096;
static const uint8_t HEAP_ALIGN = 8;

typedef uint16_t raw_boundary_t;

struct boundary_t {
    uint16_t length;
    bool p_alloc;
    bool alloc;
};

typedef struct boundary_t boundary_t;

static inline boundary_t unpack(raw_boundary_t raw) {
    return (boundary_t){
        .length = raw >> 2,
        .p_alloc = (raw >> 1) & 1,
        .alloc = raw & 1,
    };
}

static inline raw_boundary_t pack(boundary_t boundary) {
    return (boundary.length << 2) | (boundary.p_alloc << 1) | boundary.alloc;
}

static inline void put_header(uint8_t *ptr, boundary_t boundary) {
    *((raw_boundary_t *)ptr) = pack(boundary);
}

static inline void put_footer(uint8_t *ptr, boundary_t boundary) {
    *((raw_boundary_t *)(ptr + boundary.length - sizeof(raw_boundary_t))) =
        pack(boundary);
}

static inline void put_boundaries(uint8_t *ptr, boundary_t boundary) {
    put_header(ptr, boundary);
    if (!boundary.alloc) {
        put_footer(ptr, boundary);
    }
}

void error(char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

void *Mmap(size_t length) {
    void *res;

    if ((res = mmap(NULL, length, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, 0, 0)) == MAP_FAILED) {
        error("mmap");
    }

    return res;
}

void Munmap(void *ptr, size_t length) {
    if (munmap(ptr, length) < 0) {
        error("munmap");
    }
}

struct allocator_t {
    uint8_t *heap;

    size_t available;
    size_t allocations;
    size_t deallocations;
    size_t l_coalesce;
    size_t r_coalesce;
    size_t lr_coalesce;
};

typedef struct allocator_t allocator_t;

void allocator_reset(allocator_t *alloc) {
    boundary_t boundary = {
        .length = HEAP_SIZE - HEAP_ALIGN, .p_alloc = true, .alloc = false};
    put_boundaries(alloc->heap, boundary);
    boundary_t epi_boundary = {
        .length = HEAP_ALIGN, .p_alloc = false, .alloc = true};
    put_boundaries(alloc->heap + (HEAP_SIZE - HEAP_ALIGN), epi_boundary);
    alloc->allocations = alloc->deallocations = alloc->l_coalesce =
        alloc->r_coalesce = alloc->lr_coalesce = 0;
    alloc->available = HEAP_SIZE - HEAP_ALIGN;
}

void allocator_init(allocator_t *alloc) {
    alloc->heap = Mmap(HEAP_SIZE);
    allocator_reset(alloc);
}

void allocator_deinit(allocator_t *alloc) {
    Munmap(alloc->heap, HEAP_SIZE);
    alloc->allocations = alloc->deallocations = alloc->l_coalesce =
        alloc->r_coalesce = alloc->lr_coalesce = 0;
    alloc->available = HEAP_SIZE - HEAP_ALIGN;
}

void allocator_dump(allocator_t *alloc) {
    uint8_t *current = alloc->heap;
    uint16_t block = 0;

    printf("==================== HEAPDUMP =====================\n");

    while (current < alloc->heap + HEAP_SIZE) {
        if ((uint8_t *)current == alloc->heap + (HEAP_SIZE - HEAP_ALIGN)) {
            printf("==================== EPILOGUE =====================\n");
        }
        raw_boundary_t *boundary_ptr = (raw_boundary_t *)current;
        boundary_t boundary = unpack(*boundary_ptr);
        printf("[%3d] %p | length=%04u | %s | p_alloc=%d\n", block++, current,
               boundary.length, boundary.alloc ? "alloc" : "free ",
               boundary.p_alloc);
        current += boundary.length;
    }

    printf("===================================================\n\n");
}

// Check integrity of heap.
void allocator_check(allocator_t *alloc) {
    uint8_t *current = alloc->heap;
    bool p_alloc = true;

    while (current < alloc->heap + HEAP_SIZE) {
        raw_boundary_t *boundary_ptr = (raw_boundary_t *)current;
        boundary_t boundary = unpack(*boundary_ptr);
        assert(boundary.length != 0);
        assert(boundary.length % HEAP_ALIGN == 0);
        assert(boundary.p_alloc == p_alloc);
        if (!boundary.alloc) {
            raw_boundary_t header = *boundary_ptr;
            raw_boundary_t footer =
                *((raw_boundary_t *)((uint8_t *)boundary_ptr + boundary.length -
                                     sizeof(raw_boundary_t)));
            assert(header == footer);
        }
        p_alloc = boundary.alloc;
        current += boundary.length;
    }

    raw_boundary_t *epi_boundary_ptr =
        (raw_boundary_t *)(alloc->heap + (HEAP_SIZE - HEAP_ALIGN));
    boundary_t epi_boundary = unpack(*epi_boundary_ptr);
    assert(epi_boundary.length == HEAP_ALIGN);
    assert(epi_boundary.alloc); // Check that epilogue block is valid.
}

uint16_t padding(uint16_t length) {
    if (length % HEAP_ALIGN == 0) {
        return 0;
    }

    return HEAP_ALIGN - (length % HEAP_ALIGN);
}

uint16_t pad_length(uint16_t length) { return length + padding(length); }

void update_p_alloc(allocator_t *alloc, uint8_t *ptr, boundary_t boundary) {
    // Do not update if ptr is the last block
    if (alloc->heap + HEAP_SIZE <= ptr + boundary.length) {
        return;
    }

    raw_boundary_t *n_boundary_ptr =
        (raw_boundary_t *)((uint8_t *)ptr + boundary.length);
    boundary_t n_boundary = unpack(*n_boundary_ptr);
    n_boundary.p_alloc = boundary.alloc;
    put_boundaries((uint8_t *)n_boundary_ptr, n_boundary);
}

void *allocate(allocator_t *alloc, uint16_t length) {
    // Unless positive length, ignore request.
    if (length == 0) {
        return NULL;
    }

    // Find a find a free block sufficiently big
    uint8_t *current = alloc->heap;

    while (current < alloc->heap + (HEAP_SIZE - HEAP_ALIGN)) {
        boundary_t boundary = unpack(*((raw_boundary_t *)current));

        // Block already allocated; move on.
        if (boundary.alloc) {
            current += boundary.length;
            continue;
        }

        // Block is free.

        // Block too small; move on.
        length = pad_length(length + sizeof(raw_boundary_t));

        if (boundary.length < length) {
            current += boundary.length;
            continue;
        }

        // Block is free and big enough.

        // Remaining size of block not big enough for splitting; just set the
        // alloc bit to true. No splitting either exactly when space left is
        // enough for header and footer; we don't want 0-size free blocks.
        if (boundary.length - length <= sizeof(raw_boundary_t) * 2) {
            boundary.alloc = true;
            put_boundaries(current, boundary);
            // Update p_alloc of next block (status changed to alloc = true).
            update_p_alloc(alloc, current, boundary);
            alloc->available -= boundary.length;
            alloc->allocations++;
            return current + sizeof(raw_boundary_t);
        }

        // Split off remaining block into new free block.
        // Do not have to update next block's p_alloc because it is still free.
        boundary_t n_boundary = {
            .length = boundary.length - length,
            .p_alloc = true,
            .alloc = false,
        };
        put_boundaries(current + length, n_boundary);

        // Set header of newly allocated block.
        boundary.length = length;
        boundary.alloc = true;
        put_boundaries(current, boundary);
        alloc->available -= boundary.length;
        alloc->allocations++;
        return current + sizeof(raw_boundary_t);
    }

    return NULL;
}

void deallocate(allocator_t *alloc, void *ptr) {
    // Ignore NULL pointers
    if (ptr == NULL) {
        return;
    }

    raw_boundary_t *boundary_ptr = ptr;
    boundary_ptr -= 1; // Move back to header.
    boundary_t boundary = unpack(*boundary_ptr);

    // Do not free an already free block.
    if (!boundary.alloc) {
        DBG("Tried to free an already free block at %p", ptr);
        return;
    }

    // Do not free epilogue block.
    if ((uint8_t *)boundary_ptr == alloc->heap + (HEAP_SIZE - HEAP_ALIGN)) {
        DBG("Tried to free epilogue block");
        return;
    }

    raw_boundary_t *n_boundary_ptr =
        (raw_boundary_t *)((uint8_t *)boundary_ptr + boundary.length);
    boundary_t n_boundary = unpack(*n_boundary_ptr);

    // Both of the adjacent blocks are allocated; no coalescing.
    if (boundary.p_alloc && n_boundary.alloc) {
        boundary.alloc = false;
        put_boundaries((uint8_t *)boundary_ptr, boundary);
        update_p_alloc(alloc, (uint8_t *)boundary_ptr, boundary);
    }

    // The previous block is free but the next allocated; coalescing to the
    // left.
    else if (!boundary.p_alloc && n_boundary.alloc) {
        raw_boundary_t *p_boundary_ptr =
            boundary_ptr - 1; // Move back to footer of previous block (we know
                              // it has one because it's free).
        boundary_t p_boundary = unpack(*p_boundary_ptr);
        p_boundary_ptr =
            (raw_boundary_t *)((uint8_t *)p_boundary_ptr - p_boundary.length) +
            1; // Move to header of previous block.
        boundary.length += p_boundary.length;
        boundary.p_alloc = p_boundary.p_alloc;
        boundary.alloc = false;
        put_boundaries((uint8_t *)p_boundary_ptr, boundary);
        update_p_alloc(alloc, (uint8_t *)p_boundary_ptr, boundary);
        alloc->l_coalesce++;
    }

    // The previous block is allocated, but the next free; coalescing to the
    // right.
    else if (boundary.p_alloc && !n_boundary.alloc) {
        boundary.length += n_boundary.length;
        boundary.alloc = false;
        put_boundaries((uint8_t *)boundary_ptr, boundary);
        // Do not need to update p_block of next block because it hasn't changed
        // (free -> free).
        alloc->r_coalesce++;
    }

    // Both of the adjacent blocks are free; coalescing to both sides.
    else {
        raw_boundary_t *p_boundary_ptr =
            boundary_ptr - 1; // Move back to footer of previous block.
        boundary_t p_boundary = unpack(*p_boundary_ptr);
        p_boundary_ptr =
            (raw_boundary_t *)((uint8_t *)p_boundary_ptr - p_boundary.length) +
            1; // Move back to header of previous block.
        boundary.length += p_boundary.length + n_boundary.length;
        boundary.p_alloc = p_boundary.p_alloc;
        boundary.alloc = false;
        put_boundaries((uint8_t *)p_boundary_ptr, boundary);
        // Again, do not need to update p_block of next block because it went
        // from free -> free.
        alloc->lr_coalesce++;
    }

    alloc->deallocations++;
    alloc->available += boundary.length;
}

void test_allocate(allocator_t *alloc) {
    const uint16_t length = 1;
    const uint16_t block_length = 8;
    const size_t blocks = (HEAP_SIZE - HEAP_ALIGN) / block_length;
    void *ptrs[blocks];

    for (int i = 0; i < blocks; i++) {
        ptrs[i] = allocate(alloc, length);
        assert(ptrs[i] != NULL);
    }

    assert(alloc->allocations == blocks);

    for (int i = 0; i < blocks; i++) {
        deallocate(alloc, ptrs[i]);
    }

    assert(alloc->deallocations == blocks);

    raw_boundary_t *boundary_ptr = (raw_boundary_t *)alloc->heap;
    boundary_t boundary = unpack(*boundary_ptr);
    assert(boundary.length == HEAP_SIZE - HEAP_ALIGN);
    assert(boundary.p_alloc);
    assert(!boundary.alloc);
}

void test_l_coalesce(allocator_t *alloc) {
    const uint16_t length =
        1014; // Allocate 4 blocks that will be 1016 with padding, 4*1016=4064.
    const uint16_t leftover_length = 22; // 24 bytes leftover, 4088-4064=24.
    void *ptr1 = allocate(alloc, length);
    void *ptr2 = allocate(alloc, length);
    void *ptr3 = allocate(alloc, length);
    void *ptr4 = allocate(alloc, length);
    void *ptr5 = allocate(alloc, leftover_length); // To allocate everything.

    // Trigger left coalesce.
    deallocate(alloc, ptr1);
    deallocate(alloc, ptr2);
    assert(alloc->l_coalesce == 1);
    deallocate(alloc, ptr3);
    assert(alloc->l_coalesce == 2);
    deallocate(alloc, ptr4);
    assert(alloc->l_coalesce == 3);
    deallocate(alloc, ptr5);
    assert(alloc->l_coalesce == 4);
}

void test_r_coalesce(allocator_t *alloc) {
    const uint16_t length =
        1014; // Allocate 4 blocks that will be 1016 with padding, 4*1016=4064.
    const uint16_t leftover_length = 22; // 24 bytes leftover, 4088-4064=24.
    void *ptr1 = allocate(alloc, length);
    void *ptr2 = allocate(alloc, length);
    void *ptr3 = allocate(alloc, length);
    void *ptr4 = allocate(alloc, length);
    void *ptr5 = allocate(alloc, leftover_length); // To allocate everything.

    // Trigger right coalesce.
    deallocate(alloc, ptr5);
    deallocate(alloc, ptr4);
    assert(alloc->r_coalesce == 1);
    deallocate(alloc, ptr3);
    assert(alloc->r_coalesce == 2);
    deallocate(alloc, ptr2);
    assert(alloc->r_coalesce == 3);
    deallocate(alloc, ptr1);
    assert(alloc->r_coalesce == 4);
}

void test_lr_coalesce(allocator_t *alloc) {
    const uint16_t length =
        1358; // Allocate 2 blocks that will be 1360 with padding, 2*1360=2720.
    const uint16_t leftover_length =
        1366; // 1368 bytes leftover, 4088-2720=1368.
    void *ptr1 = allocate(alloc, length);
    void *ptr2 = allocate(alloc, length);
    void *ptr3 = allocate(alloc, leftover_length); // To allocate everything.

    // Trigger left-right coalesce.
    deallocate(alloc, ptr1);
    deallocate(alloc, ptr3);
    deallocate(alloc, ptr2);
    assert(alloc->lr_coalesce == 1);
}

void test_stress(allocator_t *alloc) {
    const uint16_t MAX_PTRS = (HEAP_SIZE - HEAP_ALIGN) / HEAP_ALIGN;
    void *ptrs[MAX_PTRS];
    uint16_t alloc_ptrs = 0;

    for (int i = 0; i < 200000; i++) {
        if (alloc_ptrs != MAX_PTRS && (alloc_ptrs == 0 || rand() % 2)) {
            void *p = allocate(alloc, rand() % 256 + 1);
            if (p != NULL) {
                ptrs[alloc_ptrs++] = p;
            }
            allocator_check(alloc);
        } else {
            uint16_t to_deallocate = rand() % alloc_ptrs;
            deallocate(alloc, ptrs[to_deallocate]);
            ptrs[to_deallocate] = ptrs[--alloc_ptrs];
            allocator_check(alloc);
        }
    }

    while (0 < alloc_ptrs) {
        deallocate(alloc, ptrs[--alloc_ptrs]);
    }
}

int main() {
    allocator_t alloc;
    allocator_init(&alloc);

    test_allocate(&alloc);
    allocator_reset(&alloc);

    test_l_coalesce(&alloc);
    allocator_reset(&alloc);

    test_r_coalesce(&alloc);
    allocator_reset(&alloc);

    test_lr_coalesce(&alloc);
    allocator_reset(&alloc);

    test_stress(&alloc);
    allocator_reset(&alloc);

    allocator_deinit(&alloc);

    return 0;
}
