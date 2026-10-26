@ The FastVideoDS decoder blob's entry table (tools/fv_blob.py; port/ds/movie_fv.c loads the blob): at offset 0 of
@ the image, each word relocated to where the blob is loaded. The decoder itself comes from an external FastVideoDS
@ player checkout (fastvideo/), which is not part of this repository.
    .section .fvhead, "a"
    .word 0x31425646 @ FVB1
    .word fv_initDecoderAsm
    .word fv_decodeFrame_asm
    .word fv_decodePFrameVectors
    .word fv_decodePFrameDcts
    .word fv_finishPBlock
    .word fv_gMcGxCmdBuf    @ (in the display list, which is loaded elsewhere)
    .word fv_gMcGxCmdBufA
