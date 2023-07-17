#ifndef GSR_UTILS_H
#define GSR_UTILS_H

#include "vec2.h"
#include <stdbool.h>
#include <stdint.h>
#include <X11/extensions/Xrandr.h>

typedef enum {
    GSR_GPU_VENDOR_AMD,
    GSR_GPU_VENDOR_INTEL,
    GSR_GPU_VENDOR_NVIDIA
} gsr_gpu_vendor;

typedef struct {
    gsr_gpu_vendor vendor;
    int gpu_version; /* 0 if unknown */
} gsr_gpu_info;

typedef struct {
    const char *name;
    int name_len;
    vec2i pos;
    vec2i size;
    XRRCrtcInfo *crt_info; /* Only on x11 */
    uint32_t connector_id; /* Only on drm */
} gsr_monitor;

typedef enum {
    GSR_CONNECTION_X11,
    GSR_CONNECTION_WAYLAND,
    GSR_CONNECTION_DRM
} gsr_connection_type;

typedef struct {
    const char *name;
    int name_len;
    gsr_monitor *monitor;
    bool found_monitor;
} get_monitor_by_name_userdata;

double clock_get_monotonic_seconds(void);

typedef void (*active_monitor_callback)(const gsr_monitor *monitor, void *userdata);
void for_each_active_monitor_output(void *connection, gsr_connection_type connection_type, active_monitor_callback callback, void *userdata);
bool get_monitor_by_name(void *connection, gsr_connection_type connection_type, const char *name, gsr_monitor *monitor);

bool gl_get_gpu_info(Display *dpy, gsr_gpu_info *info, bool wayland);

/* |output| should be at least 128 bytes in size */
bool gsr_get_valid_card_path(char *output);

#endif /* GSR_UTILS_H */
