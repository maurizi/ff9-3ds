/* Use the matching SDK implementation with its full-heap/free allocator repair.
 * Providing the complete object keeps libnds9.a's unmodified videoGL.o out of
 * the link, including its internal calls to the allocator. */
#include "../../third_party/libnds/videoGL.c"
