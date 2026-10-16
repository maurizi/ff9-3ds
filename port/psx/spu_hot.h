#ifndef PORT_SPU_HOT_H
#define PORT_SPU_HOT_H
/* DS instruction placement only; arithmetic and sequencing stay shared. */
#if defined(PORT_SPU_ITCM) && defined(__arm__)
#define SPU_HOT __attribute__((section(".itcm")))
#else
#define SPU_HOT
#endif
#endif
