/* The FastVideoDS decoder blob (tools/fv_blob.py): code and tables linked at 0 and relocated when loaded; the motion
 * compensation's display list (fvMcData) linked at 0x10000000, loaded separately (VRAM: its words are DMA'd). */
SECTIONS {
    .image 0 : {
        KEEP(*(.fvhead))
        *(.text .text.* .glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.* .dtcm .dtcm.*)
        *(EXCLUDE_FILE(*fvMcData.o) .data EXCLUDE_FILE(*fvMcData.o) .data.*)
        . = ALIGN(32);
    }
    .bss : { *(.bss .bss.* COMMON) }
    ASSERT(SIZEOF(.bss) == 0, "the blob has bss")
    .mclist 0x10000000 : { */fvMcData.o(.data .data.*) }
    /DISCARD/ : { *(.ARM.attributes .comment .note.* .ARM.exidx* .debug*) }
}
