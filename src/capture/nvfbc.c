#include "../../include/capture/nvfbc.h"
#include "../../external/NvFBC.h"
#include "../../include/cuda.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <X11/Xlib.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/frame.h>
#include <libavutil/version.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_nvfbc_params params;
    void *library;

    NVFBC_SESSION_HANDLE nv_fbc_handle;
    PNVFBCCREATEINSTANCE nv_fbc_create_instance;
    NVFBC_API_FUNCTION_LIST nv_fbc_function_list;
    bool fbc_handle_created;
    bool capture_session_created;

    gsr_cuda cuda;
    bool frame_initialized;
} gsr_capture_nvfbc;

#if defined(_WIN64) || defined(__LP64__)
typedef unsigned long long CUdeviceptr_v2;
#else
typedef unsigned int CUdeviceptr_v2;
#endif
typedef CUdeviceptr_v2 CUdeviceptr;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

/* Returns 0 on failure */
static uint32_t get_output_id_from_display_name(NVFBC_RANDR_OUTPUT_INFO *outputs, uint32_t num_outputs, const char *display_name, uint32_t *width, uint32_t *height) {
    if(!outputs)
        return 0;

    for(uint32_t i = 0; i < num_outputs; ++i) {
        if(strcmp(outputs[i].name, display_name) == 0) {
            *width = outputs[i].trackedBox.w;
            *height = outputs[i].trackedBox.h;
            return outputs[i].dwId;
        }
    }

    return 0;
}

/* TODO: Test with optimus and open kernel modules */
static bool get_driver_version(int *major, int *minor) {
    *major = 0;
    *minor = 0;

    FILE *f = fopen("/proc/driver/nvidia/version", "rb");
    if(!f) {
        fprintf(stderr, "gsr warning: failed to get nvidia driver version (failed to read /proc/driver/nvidia/version)\n");
        return false;
    }

    char buffer[2048];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[bytes_read] = '\0';

    bool success = false;
    const char *p = strstr(buffer, "Kernel Module");
    if(p) {
        p += 13;
        int driver_major_version = 0, driver_minor_version = 0;
        if(sscanf(p, "%d.%d", &driver_major_version, &driver_minor_version) == 2) {
            *major = driver_major_version;
            *minor = driver_minor_version;
            success = true;
        }
    }

    if(!success)
        fprintf(stderr, "gsr warning: failed to get nvidia driver version\n");

    fclose(f);
    return success;
}

static bool version_at_least(int major, int minor, int expected_major, int expected_minor) {
    return major > expected_major || (major == expected_major && minor >= expected_minor);
}

static bool version_less_than(int major, int minor, int expected_major, int expected_minor) {
    return major < expected_major || (major == expected_major && minor < expected_minor);
}

static void set_func_ptr(void **dst, void *src) {
    *dst = src;
}

static bool gsr_capture_nvfbc_load_library(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    dlerror(); /* clear */
    void *lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
    if(!lib) {
        fprintf(stderr, "gsr error: failed to load libnvidia-fbc.so.1, error: %s\n", dlerror());
        return false;
    }

    set_func_ptr((void**)&cap_nvfbc->nv_fbc_create_instance, dlsym(lib, "NvFBCCreateInstance"));
    if(!cap_nvfbc->nv_fbc_create_instance) {
        fprintf(stderr, "gsr error: unable to resolve symbol 'NvFBCCreateInstance'\n");
        dlclose(lib);
        return false;
    }

    memset(&cap_nvfbc->nv_fbc_function_list, 0, sizeof(cap_nvfbc->nv_fbc_function_list));
    cap_nvfbc->nv_fbc_function_list.dwVersion = NVFBC_VERSION;
    NVFBCSTATUS status = cap_nvfbc->nv_fbc_create_instance(&cap_nvfbc->nv_fbc_function_list);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: failed to create NvFBC instance (status: %d)\n", status);
        dlclose(lib);
        return false;
    }

    cap_nvfbc->library = lib;
    return true;
}

#if LIBAVUTIL_VERSION_MAJOR < 57
static AVBufferRef* dummy_hw_frame_init(int size) {
    return av_buffer_alloc(size);
}
#else
static AVBufferRef* dummy_hw_frame_init(size_t size) {
    return av_buffer_alloc(size);
}
#endif

static bool ffmpeg_create_cuda_contexts(gsr_capture_nvfbc *cap_nvfbc, AVCodecContext *video_codec_context) {
    AVBufferRef *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!device_ctx) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to create hardware device context\n");
        return false;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = cap_nvfbc->cuda.cu_ctx;
    if(av_hwdevice_ctx_init(device_ctx) < 0) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to create hardware device context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_BGR0;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    hw_frame_context->pool = av_buffer_pool_init(1, dummy_hw_frame_init);
    hw_frame_context->initial_pool_size = 1;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "gsr error: cuda_create_codec_context failed: failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

static int gsr_capture_nvfbc_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;
    if(!gsr_cuda_load(&cap_nvfbc->cuda, cap_nvfbc->params.dpy, cap_nvfbc->params.overclock))
        return -1;

    if(!gsr_capture_nvfbc_load_library(cap)) {
        gsr_cuda_unload(&cap_nvfbc->cuda);
        return -1;
    }

    const uint32_t x = max_int(cap_nvfbc->params.pos.x, 0);
    const uint32_t y = max_int(cap_nvfbc->params.pos.y, 0);
    const uint32_t width = max_int(cap_nvfbc->params.size.x, 0);
    const uint32_t height = max_int(cap_nvfbc->params.size.y, 0);

    const bool capture_region = (x > 0 || y > 0 || width > 0 || height > 0);

    bool supports_direct_cursor = false;
    bool direct_capture = cap_nvfbc->params.direct_capture;
    int driver_major_version = 0;
    int driver_minor_version = 0;
    if(direct_capture && get_driver_version(&driver_major_version, &driver_minor_version)) {
        fprintf(stderr, "Info: detected nvidia version: %d.%d\n", driver_major_version, driver_minor_version);

        // TODO:
        if(version_at_least(driver_major_version, driver_minor_version, 515, 57) && version_less_than(driver_major_version, driver_minor_version, 520, 56)) {
            direct_capture = false;
            fprintf(stderr, "Warning: \"screen-direct\" has temporary been disabled as it causes stuttering with driver versions >= 515.57 and < 520.56. Please update your driver if possible. Capturing \"screen\" instead.\n");
        }

        // TODO:
        // Cursor capture disabled because moving the cursor doesn't update capture rate to monitor hz and instead captures at 10-30 hz
        /*
        if(direct_capture) {
            if(version_at_least(driver_major_version, driver_minor_version, 515, 57))
                supports_direct_cursor = true;
            else
                fprintf(stderr, "Info: capturing \"screen-direct\" but driver version appears to be less than 515.57. Disabling capture of cursor. Please update your driver if you want to capture your cursor or record \"screen\" instead.\n");
        }
        */
    }

    NVFBCSTATUS status;
    NVFBC_TRACKING_TYPE tracking_type;
    uint32_t output_id = 0;
    cap_nvfbc->fbc_handle_created = false;
    cap_nvfbc->capture_session_created = false;

    NVFBC_CREATE_HANDLE_PARAMS create_params;
    memset(&create_params, 0, sizeof(create_params));
    create_params.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
    if(status != NVFBC_SUCCESS) {
        // Reverse engineering for interoperability
        const uint8_t enable_key[] = { 0xac, 0x10, 0xc9, 0x2e, 0xa5, 0xe6, 0x87, 0x4f, 0x8f, 0x4b, 0xf4, 0x61, 0xf8, 0x56, 0x27, 0xe9 };
        create_params.privateData = enable_key;
        create_params.privateDataSize = 16;

        status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
            goto error_cleanup;
        }
    }
    cap_nvfbc->fbc_handle_created = true;

    NVFBC_GET_STATUS_PARAMS status_params;
    memset(&status_params, 0, sizeof(status_params));
    status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCGetStatus(cap_nvfbc->nv_fbc_handle, &status_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }

    if(status_params.bCanCreateNow == NVFBC_FALSE) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: it's not possible to create a capture session on this system\n");
        goto error_cleanup;
    }

    uint32_t tracking_width = XWidthOfScreen(DefaultScreenOfDisplay(cap_nvfbc->params.dpy));
    uint32_t tracking_height = XHeightOfScreen(DefaultScreenOfDisplay(cap_nvfbc->params.dpy));
    tracking_type = strcmp(cap_nvfbc->params.display_to_capture, "screen") == 0 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT;
    if(tracking_type == NVFBC_TRACKING_OUTPUT) {
        if(!status_params.bXRandRAvailable) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the xrandr extension is not available\n");
            goto error_cleanup;
        }

        if(status_params.bInModeset) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the x server is in modeset, unable to record\n");
            goto error_cleanup;
        }

        output_id = get_output_id_from_display_name(status_params.outputs, status_params.dwOutputNum, cap_nvfbc->params.display_to_capture, &tracking_width, &tracking_height);
        if(output_id == 0) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: display '%s' not found\n", cap_nvfbc->params.display_to_capture);
            goto error_cleanup;
        }
    }

    NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
    memset(&create_capture_params, 0, sizeof(create_capture_params));
    create_capture_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    create_capture_params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
    create_capture_params.bWithCursor = (!direct_capture || supports_direct_cursor) ? NVFBC_TRUE : NVFBC_FALSE;
    if(capture_region)
        create_capture_params.captureBox = (NVFBC_BOX){ x, y, width, height };
    create_capture_params.eTrackingType = tracking_type;
    create_capture_params.dwSamplingRateMs = (uint32_t)ceilf(1000.0f / (float)cap_nvfbc->params.fps);
    create_capture_params.bAllowDirectCapture = direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.bPushModel = direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    //create_capture_params.bDisableAutoModesetRecovery = true; // TODO:
    if(tracking_type == NVFBC_TRACKING_OUTPUT)
        create_capture_params.dwOutputId = output_id;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateCaptureSession(cap_nvfbc->nv_fbc_handle, &create_capture_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }
    cap_nvfbc->capture_session_created = true;

    NVFBC_TOCUDA_SETUP_PARAMS setup_params;
    memset(&setup_params, 0, sizeof(setup_params));
    setup_params.dwVersion = NVFBC_TOCUDA_SETUP_PARAMS_VER;
    setup_params.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCToCudaSetUp(cap_nvfbc->nv_fbc_handle, &setup_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }

    if(capture_region) {
        video_codec_context->width = width & ~1;
        video_codec_context->height = height & ~1;
    } else {
        video_codec_context->width = tracking_width & ~1;
        video_codec_context->height = tracking_height & ~1;
    }

    if(!ffmpeg_create_cuda_contexts(cap_nvfbc, video_codec_context))
        goto error_cleanup;

    return 0;

    error_cleanup:
    if(cap_nvfbc->fbc_handle_created) {
        if(cap_nvfbc->capture_session_created) {
            NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
            memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
            destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
            cap_nvfbc->nv_fbc_function_list.nvFBCDestroyCaptureSession(cap_nvfbc->nv_fbc_handle, &destroy_capture_params);
            cap_nvfbc->capture_session_created = false;
        }

        NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
        memset(&destroy_params, 0, sizeof(destroy_params));
        destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        cap_nvfbc->nv_fbc_function_list.nvFBCDestroyHandle(cap_nvfbc->nv_fbc_handle, &destroy_params);
        cap_nvfbc->fbc_handle_created = false;
    }

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_cuda_unload(&cap_nvfbc->cuda);
    return -1;
}

static void gsr_capture_nvfbc_destroy_session(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    if(cap_nvfbc->fbc_handle_created) {
        if(cap_nvfbc->capture_session_created) {
            NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
            memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
            destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
            cap_nvfbc->nv_fbc_function_list.nvFBCDestroyCaptureSession(cap_nvfbc->nv_fbc_handle, &destroy_capture_params);
            cap_nvfbc->capture_session_created = false;
        }

        NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
        memset(&destroy_params, 0, sizeof(destroy_params));
        destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        cap_nvfbc->nv_fbc_function_list.nvFBCDestroyHandle(cap_nvfbc->nv_fbc_handle, &destroy_params);
        cap_nvfbc->fbc_handle_created = false;
    }

    cap_nvfbc->nv_fbc_handle = 0;
}

static void gsr_capture_nvfbc_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;
    if(!cap_nvfbc->frame_initialized && video_codec_context->hw_frames_ctx) {
        cap_nvfbc->frame_initialized = true;
        (*frame)->hw_frames_ctx = video_codec_context->hw_frames_ctx;
        (*frame)->buf[0] = av_buffer_pool_get(((AVHWFramesContext*)video_codec_context->hw_frames_ctx->data)->pool);
        (*frame)->extended_data = (*frame)->data;
        (*frame)->color_range = video_codec_context->color_range;
        (*frame)->color_primaries = video_codec_context->color_primaries;
        (*frame)->color_trc = video_codec_context->color_trc;
        (*frame)->colorspace = video_codec_context->colorspace;
        (*frame)->chroma_location = video_codec_context->chroma_sample_location;
    }
}

static int gsr_capture_nvfbc_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    CUdeviceptr cu_device_ptr = 0;

    NVFBC_FRAME_GRAB_INFO frame_info;
    memset(&frame_info, 0, sizeof(frame_info));

    NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab_params;
    memset(&grab_params, 0, sizeof(grab_params));
    grab_params.dwVersion = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;
    grab_params.dwFlags = NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT;/* | NVFBC_TOCUDA_GRAB_FLAGS_FORCE_REFRESH;*/
    grab_params.pFrameGrabInfo = &frame_info;
    grab_params.pCUDADeviceBuffer = &cu_device_ptr;
    grab_params.dwTimeoutMs = 0;

    NVFBCSTATUS status = cap_nvfbc->nv_fbc_function_list.nvFBCToCudaGrabFrame(cap_nvfbc->nv_fbc_handle, &grab_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_capture failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        return -1;
    }

    /*
        *byte_size = frame_info.dwByteSize;

        TODO: Check bIsNewFrame
        TODO: Check dwWidth and dwHeight and update size in video output in ffmpeg. This can happen when xrandr is used to change monitor resolution
    */

    frame->data[0] = (uint8_t*)cu_device_ptr;
    //frame->data[1] = (uint8_t*)cu_device_ptr;
    //frame->data[2] = (uint8_t*)cu_device_ptr;
    frame->linesize[0] = frame->width * 4;
    // TODO: Use these when outputting yuv444 by changing nvfbc color to YUV444P and sw_format to YUV444P
    //frame->linesize[1] = frame->width * 1;
    //frame->linesize[2] = frame->width * 1;
    return 0;
}

static void gsr_capture_nvfbc_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;
    gsr_capture_nvfbc_destroy_session(cap);
    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);
    if(cap_nvfbc) {
        gsr_cuda_unload(&cap_nvfbc->cuda);
        dlclose(cap_nvfbc->library);
        free((void*)cap_nvfbc->params.display_to_capture);
        cap_nvfbc->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_nvfbc_create(const gsr_capture_nvfbc_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_create params is NULL\n");
        return NULL;
    }

    if(!params->display_to_capture) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_create params.display_to_capture is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_nvfbc *cap_nvfbc = calloc(1, sizeof(gsr_capture_nvfbc));
    if(!cap_nvfbc) {
        free(cap);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        free(cap);
        free(cap_nvfbc);
        return NULL;
    }

    cap_nvfbc->params = *params;
    cap_nvfbc->params.display_to_capture = display_to_capture;
    cap_nvfbc->params.fps = max_int(cap_nvfbc->params.fps, 1);
    
    *cap = (gsr_capture) {
        .start = gsr_capture_nvfbc_start,
        .tick = gsr_capture_nvfbc_tick,
        .should_stop = NULL,
        .capture = gsr_capture_nvfbc_capture,
        .capture_end = NULL,
        .destroy = gsr_capture_nvfbc_destroy,
        .priv = cap_nvfbc
    };

    return cap;
}
