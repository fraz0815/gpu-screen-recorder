#ifndef GSR_CAPTURE_XCOMPOSITE_CUDA_H
#define GSR_CAPTURE_XCOMPOSITE_CUDA_H

#include "capture.h"
#include "../egl.h"
#include "../vec2.h"

typedef struct {
    gsr_egl *egl;
    Window window;
    bool follow_focused; /* If this is set then |window| is ignored */
    vec2i region_size; /* This is currently only used with |follow_focused| */
    bool overclock;
} gsr_capture_xcomposite_cuda_params;

gsr_capture* gsr_capture_xcomposite_cuda_create(const gsr_capture_xcomposite_cuda_params *params);

#endif /* GSR_CAPTURE_XCOMPOSITE_CUDA_H */
