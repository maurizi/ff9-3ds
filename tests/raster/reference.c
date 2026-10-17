/* Keep the generic colour-interpolation path as a differential oracle. */
#define PORT_SOFTGPU_REFERENCE
#define psx_softgpu psx_softgpu_reference
#define psx_softgpu_enable psx_softgpu_reference_enable
#define psx_softgpu_shadow_poly psx_softgpu_reference_shadow_poly
#define psx_softgpu_shadow_poly_region psx_softgpu_reference_shadow_poly_region
#define psx_softgpu_shadow_rect psx_softgpu_reference_shadow_rect
#define psx_softgpu_set_marks psx_softgpu_reference_set_marks
#include "../../port/psx/softgpu.c"
