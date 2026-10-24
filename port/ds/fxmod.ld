/* One battle effect module, linked on its own at psx_fx_area (FX_BASE, FX_SIZE from the main executable) against the
 * main executable's symbols (Makefile, ds_FXOVL; loaded by port/game/effect_native.c). Its header comes first. */
ENTRY(psx_fxhead)
SECTIONS {
    . = FX_BASE;
    .fxhead : { KEEP(*(.fxhead)) }
    .text : { *(.text .text.* .glue_7 .glue_7t .v4_bx .vfp11_veneer) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) . = ALIGN(4); }
    .bss (NOLOAD) : { __fx_bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __fx_bss_end = .; }
    ASSERT(. <= FX_BASE + FX_SIZE, "effect module larger than psx_fx_area (port/game/effect_native.c)")
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
