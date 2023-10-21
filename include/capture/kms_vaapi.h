#ifndef GSR_CAPTURE_KMS_VAAPI_H
#define GSR_CAPTURE_KMS_VAAPI_H

#include "../vec2.h"
#include "../utils.h"
#include "capture.h"

typedef struct _XDisplay Display;

typedef struct {
    gsr_egl *egl;
    const char *display_to_capture; /* if this is "screen", then the first monitor is captured. A copy is made of this */
    gsr_gpu_info gpu_inf;
    const char *card_path; /* reference */
    bool wayland;
} gsr_capture_kms_vaapi_params;

gsr_capture* gsr_capture_kms_vaapi_create(const gsr_capture_kms_vaapi_params *params);

#endif /* GSR_CAPTURE_KMS_VAAPI_H */
