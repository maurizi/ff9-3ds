#include <assert.h>
#include <stdio.h>
#include "allocator_under_test.h"

static unsigned char memory[512 * 1024];
static s_vramBlock heap;
static unsigned serial;

static void check(void) {
    struct s_SingleBlock *prev = NULL, *first_empty = NULL, *first_alloc = NULL;
    unsigned size = 0, count = 0;
    for (struct s_SingleBlock *b = heap.firstBlock; b; b = b->node[1]) {
        assert(++count < 4096);
        assert(b->node[0] == prev);
        assert(b->AddrSet == memory + size);
        size += b->blockSize;
        if (b->indexOut && !first_alloc) first_alloc = b;
        if (!b->indexOut && !first_empty) first_empty = b;
        prev = b;
    }
    assert(size == sizeof memory);
    assert(heap.firstEmpty == first_empty);
    assert(heap.firstAlloc == first_alloc);
}

static struct s_SingleBlock *allocate(unsigned size) {
    for (struct s_SingleBlock *b = heap.firstEmpty; b; b = b->node[3]) {
        if (b->blockSize < size) continue;
        struct s_SingleBlock *allocated = vramBlock__allocateBlock(&heap, b, b->AddrSet, size);
        assert(allocated);
        allocated->indexOut = ++serial;
        check();
        return allocated;
    }
    return NULL;
}

static void release(struct s_SingleBlock *b) {
    assert(vramBlock__deallocateBlock(&heap, b));
    check();
}

int main(void) {
    struct s_SingleBlock *initial = calloc(1, sizeof *initial);
    assert(initial);
    initial->AddrSet = memory;
    initial->blockSize = sizeof memory;
    heap.firstBlock = heap.firstEmpty = initial;
    check();

    /* This is the native failure: fill VRAM exactly, then evict. */
    struct s_SingleBlock *whole = allocate(sizeof memory);
    assert(whole && !heap.firstEmpty);
    release(whole);
    assert(heap.firstEmpty && heap.firstEmpty->blockSize == sizeof memory);

    /* Free nonadjacent blocks from a full heap, refill holes, then coalesce. */
    struct s_SingleBlock *slots[128] = {0};
    for (int i = 0; i < 128; i++) { slots[i] = allocate(4096); assert(slots[i]); }
    assert(!heap.firstEmpty);
    for (int i = 0; i < 128; i += 2) { release(slots[i]); slots[i] = NULL; }
    for (int i = 0; i < 128; i += 2) { slots[i] = allocate(4096); assert(slots[i]); }
    for (int i = 127; i >= 0; i--) { release(slots[i]); slots[i] = NULL; }

    unsigned rng = 0x963a21;
    for (int i = 0; i < 20000; i++) {
        rng = rng * 1664525u + 1013904223u;
        unsigned slot = (rng >> 16) & 127;
        if (slots[slot]) { release(slots[slot]); slots[slot] = NULL; }
        else slots[slot] = allocate(128u << ((rng >> 24) & 7));
    }
    for (int i = 0; i < 128; i++) if (slots[i]) release(slots[i]);
    assert(heap.firstEmpty == heap.firstBlock);
    assert(heap.firstEmpty->blockSize == sizeof memory && !heap.firstEmpty->node[1]);
    free(heap.firstBlock);
    puts("libnds full-heap eviction, refill, coalescing and 20000 allocation operations pass");
    return 0;
}
