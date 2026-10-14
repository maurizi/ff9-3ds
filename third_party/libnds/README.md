This is `source/arm9/video/videoGL.c` from BlocksDS libnds v1.24.0-blocks,
commit `7fd8ccbe781ed48a06fe063a2dcbb69035fa97d3`, matching the build container.
Source: https://github.com/blocksds/libnds/tree/v1.24.0-blocks
The Zlib license is included in `LICENSE`.

The sole source change restores `firstEmpty` when freeing a block from a
completely full VRAM heap. The original leaves that pointer null, so subsequent
allocations fail even after all textures have been evicted. The real native
opening reproduced this with 378 KiB free and failed 128-byte uploads.

`port/ds/video_gl.c` compiles this replacement before linking libnds9.a; the
archive's original videoGL object is therefore not selected. This avoids
changing the installed SDK. Keep this override until the SDK includes the fix,
then remove the vendored file and wrapper together.

The wrapper also raises the CPU texture-name limit from 2048 to 8192, using
libnds' existing dynamic arrays. Indexed pixel sharing can need an image name
and a separate palette handle for each of the 2048 logical cache entries. This
does not change the 512 KiB texture or 64 KiB palette VRAM budget. If removing
the allocator override, preserve the larger name allowance or reduce handle use
before enabling indexed sharing.
