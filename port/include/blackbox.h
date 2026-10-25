/* BLACKBOX=1 (DS): the last 64 phases (tag, frame, an argument) in a DTCM ring, which survives a wipe of main RAM;
 * read it from a melonDS state with tools/ds_blackbox.py. */
#ifndef BLACKBOX_H
#define BLACKBOX_H
#if defined(PORT_BLACKBOX) && defined(__arm__)
extern volatile unsigned int port_blackbox[2 + 64 * 2];
static inline void port_bb(unsigned int tag, unsigned int arg) {
    unsigned int i = port_blackbox[1]++ & 63;
    port_blackbox[2 + 2 * i] = tag;
    port_blackbox[3 + 2 * i] = arg;
}
#define BB(tag, arg) port_bb((tag), (unsigned int)(arg))
#else
#define BB(tag, arg) ((void)0)
#endif
#endif
