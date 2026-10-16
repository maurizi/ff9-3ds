#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "psx_spu_source.h"

static uint8_t ram[PSX_SPU_RAM_BYTES];

static void block(unsigned address, unsigned header, unsigned flags, unsigned data) {
    ram[address] = header;
    ram[address + 1] = flags;
    memset(ram + address + 2, data, 14);
}

static unsigned play_block(PsxSpuSource *source, uint16_t *loop, int noise) {
    psx_spu_source_fetch(source, ram, loop);
    for (unsigned i = 0; i < 27; ++i)
        assert(psx_spu_source_advance(source, 0x1000, *loop, noise) == 0);
    return psx_spu_source_advance(source, 0x1000, *loop, noise);
}

int main(void) {
    PsxSpuSource source;
    uint16_t loop = 0x200;
    /* Key-on aligns SSA but preserves the independent loop register. */
    memset(&source, 0xff, sizeof source);
    psx_spu_source_key_on(&source, 0x21);
    assert(source.address == 0x20 && loop == 0x200);
    assert(source.counter == 0 && source.history.previous == 0 && source.history.older == 0);
    for (unsigned i = 0; i < 31; ++i) assert(source.samples[i] == 0);

    /* Loop-start updates LSAX on fetch, not at key-on. Normal fetch is latched,
     * so writes into an already decoded block cannot replace it mid-playback. */
    block(0x100, 0x0c, 4, 0x21);
    block(0x110, 0x1c, 3, 0x21);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(loop == 0x20 && source.samples[3] == 1 && source.samples[30] == 2);
    block(0x100, 0x0c, 4, 0x65);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(source.samples[3] == 1);
    assert(play_block(&source, &loop, 0) == 0 && source.address == 0x22);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(source.samples[0] == 2 && source.samples[1] == 1 && source.samples[2] == 2);
    assert(source.samples[3] == 2); /* filter 1 uses the previous block's last 2 */
    assert(play_block(&source, &loop, 0) == PSX_SPU_SOURCE_END);
    assert(source.address == 0x20 && !source.fetched && !source.first_block);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(source.samples[3] == 5); /* RAM modification becomes visible on refetch */

    /* End+mute redirects regardless of envelope/noise; only mute is conditional.
     * Repeat without End has no effect. Exercise all eight low flag patterns. */
    for (unsigned flags = 0; flags < 8; ++flags) {
        for (int noise = 0; noise < 2; ++noise) {
            block(0x180, 0x0c, flags | 0xf8, 0x87);
            psx_spu_source_key_on(&source, 0x30);
            loop = 0x41;
            unsigned actions = play_block(&source, &loop, noise);
            assert(loop == ((flags & 4) ? 0x30 : 0x41));
            if (flags & 1) {
                unsigned expected = PSX_SPU_SOURCE_END;
                if (!(flags & 2) && !noise) expected |= PSX_SPU_SOURCE_MUTE;
                assert(actions == expected && source.address == (loop & 0xfffeu));
            } else assert(actions == 0 && source.address == 0x32);
        }
    }

    /* Manual loop writes during the first block remain subject to its loop
     * flag. After that window, even an equal-value write suppresses flags. */
    block(0x100, 0x0c, 0, 0x21);
    block(0x110, 0x0c, 4, 0x43);
    psx_spu_source_key_on(&source, 0x20);
    psx_spu_source_set_loop(&source, &loop, 0x50, 1);
    assert(!source.ignore_loop_start);
    assert(play_block(&source, &loop, 0) == 0);
    psx_spu_source_set_loop(&source, &loop, 0x50, 1);
    assert(source.ignore_loop_start);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(loop == 0x50);
    psx_spu_source_key_on(&source, 0x22);
    assert(!source.ignore_loop_start);
    psx_spu_source_set_loop(&source, &loop, 0x50, 0);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(loop == 0x50 && source.ignore_loop_start);

    /* Fractional carry survives boundaries and looping. Pitch zero freezes the
     * source; maximum pitch advances fewer than four samples per output tick. */
    block(0x100, 0x0c, 3, 0);
    psx_spu_source_key_on(&source, 0x20);
    loop = 0x40;
    psx_spu_source_fetch(&source, ram, &loop);
    assert(psx_spu_source_advance(&source, 0, loop, 0) == 0 && source.counter == 0);
    for (unsigned i = 0; i < 55; ++i)
        assert(psx_spu_source_advance(&source, 0x801, loop, 0) == 0);
    assert(psx_spu_source_advance(&source, 0x801, loop, 0) == PSX_SPU_SOURCE_END);
    assert(source.counter == 56 && source.address == 0x40);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(psx_spu_source_advance(&source, 0xffff, loop, 0) == 0);
    assert(source.counter == 56 + 0x3fff);

    /* Last aligned block fits RAM and wraps to address zero without OOB reads. */
    block(0x7fff0, 0x0c, 0, 0xf1);
    psx_spu_source_key_on(&source, 0xffff);
    assert(source.address == 0xfffe);
    psx_spu_source_fetch(&source, ram, &loop);
    assert(source.samples[3] == 1 && source.samples[4] == -1);
    assert(play_block(&source, &loop, 0) == 0 && source.address == 0);

    puts("SPU source: flags, loops, history, pitch and RAM wrapping PASS");
    return 0;
}
