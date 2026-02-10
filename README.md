# Implicit Free-List Allocator

A small implicit-free list allocator written in C, using boundary tags and immediate coalescing. This project was implemented as a learning exercise to work with low-level memory management and allocator design.

## Features

- Implicit free-list traversal.
- Block representation using boundary tags.
- Immediate coalescing on deallocation.
- Alignment-aware allocation.
- Epilogue sentinel block.
- Heap integrity checker.
- Stress testing with randomized allocation patterns.
- Internal statistics tracking.

## Design Overview

The allocator manages a fixed-size heap obtained via `mmap`. Blocks are arranged linearly and traversed as such using an implicit free list. Each block stores a boundary tag that contains the following information:

- The length of the block;
- The allocation status of the previous block;
- And the allocation status of block in question.

Immediate coalescing ensures low external fragmentation, while an epilogue block facilitates handling boundaries during deallocation.

## Block Layout

A boundary (header/footer) is a 15-bit value structured as follows:

```
[ length (13 bits) | p_alloc (1 bit) | alloc (1 bit) ]
```

where `length` naturally is the length of the block (including any boundaries and padding), `p_alloc` is set if the previous block is allocated, and `alloc` is set if the current block is allocated.

An allocated block has the following layout:

```
[ header | payload | padding (optional) ]
```

A free block has the following layout:

```
[ header | space | padding (optional) | footer ]
```

where `header == footer`. Only free blocks require two boundaries (header and footer), while allocated blocks only require one; a simpler design would have made this uniform and increased the internal fragmentation.

The first block in the heap has `p_alloc` set, and there is an epilogue block at the end of the heap. Upon initialization of the allocator, the layout of the whole heap is thus:

```
[ {length=4088, p_alloc=1, alloc=0} | {length=8, p_alloc=0, alloc=1} ]
```

One may notice that 13 bits for the block length are not strictly necessary. This doesn't really matter however, because we cannot escape the 16 bits in a `uint16_t/raw_boundary_t` for storage anyway.

## Allocation Strategy

Allocation uses a first-fit strategy; the heap is traversed from the beginning until a sufficiently long block is found. A new free block is split off only if the block would have space for more than just the header and footer. The next block's `p_alloc` bit has to be updated so that it never goes stale. The corresponding boundaries (headers/footers) are placed appropriately.

## Coalescing Logic

To coalesce, we need to examine whether:

- The previous block is allocated;
- And whether the next block is.

Because we have `p_alloc`, the first case is easily handled. The raison d'être of the epilogue block is precisely so that we are always able to observe the header of the next block (it cannot ever be deallocated). And so we can look at the four possible cases for coalescing:

- `p_alloc && n_alloc`: No coalescing.
- `!p_alloc && n_alloc`: Coalescing to the left.
- `p_alloc && !n_alloc`: Coalescing to the right.
- `!p_alloc && !n_alloc`: Coalescing to both the left and right.

Again, special care is needed to maintain integrity of the boundaries, and update the `p_alloc` of succeeding blocks as necessary.

## Statistics & Debugging

For testing purposes, and general statistics we keep the following information around in the allocator as well:

- The free memory left (`available`);
- Allocations (`allocations`);
- Deallocations (`deallocations`);
- Triggered left coalescings (`l_coalesce`);
- Triggered right coalescings (`r_coalesce`);
- And finally, triggered left-right coalescings (`lr_coalesce`).

## Building & Testing

To build the allocator, one may run simply `make`. Thereafter the executable `allocator` is available for running. It simply runs the tests called in `main`. The tests run are as follows:

- Allocate and then deallocate everything, making sure that `allocations == deallocations`;
- Deallocate in an order that triggers left coalescings and check `l_coalesce`;
- Deallocate in an order that triggers right coalescings and check `r_coalesce`;
- Deallocate in an order that triggers a left-right coalescing and check `lr_coalesce`;
- And finally, stress-test the allocator by a bunch of random allocations/deallocations, checking the integrity of the heap at all times with `allocator_check`.

`allocator_check` checks the integrity of the heap by ensuring the following invariants:

- Correct lengths in boundaries; that is, `length != 0` and `length % HEAP_ALIGN == 0`;
- The `alloc` status of block `b` is equal to the `p_alloc` status of the block next to `b`;
- If a block `b` is free, the header at the start of `b` is equal to the footer at the end of `b`;
- The epilogue block is not corruped and maintains its correct values.

## Possible Extensions

As this allocator is based on a simple implicit free-list design, one may modify/extend this to use the following designs:

- Explicit free lists; store the address of the first free block, and then keep data in the free blocks for traversal only of free blocks.
- Segregated free lists; keep different equivalence classes of blocks in a given length-range, and allocate accordingly.
- Heap visualizer/UI for inspection during runtime.
