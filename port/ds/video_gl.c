/* Use the matching SDK implementation with its full-heap/free allocator repair.
 * Providing the complete object keeps libnds9.a's unmodified videoGL.o out of
 * the link, including its internal calls to the allocator. */
#include <nds/arm9/videoGL.h>
/* Names index dynamically allocated CPU records, not hardware texture slots.
 * The 2048 logical cache entries can each own an image and a palette handle.
 * Leave room for both; texture/palette VRAM limits are unchanged. */
#undef MAX_TEXTURES
#define MAX_TEXTURES 8192
#include "../../third_party/libnds/videoGL.c"
