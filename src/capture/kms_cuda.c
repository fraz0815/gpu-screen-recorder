#include "../../include/capture/kms_cuda.h"
#include "../../kms/client/kms_client.h"
#include "../../include/utils.h"
#include "../../include/color_conversion.h"
#include "../../include/cuda.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

/*
    TODO: Use dummy pool for cuda buffer so we can create our own cuda buffers from pixel buffer objects
    and copy the input textures to the pixel buffer objects. Use sw_format NV12 as well. Then this is
    similar to kms_vaapi. This allows us to remove one extra texture and texture copy.
*/
// TODO: Wayland capture

#define MAX_CONNECTOR_IDS 32

typedef struct {
    uint32_t connector_ids[MAX_CONNECTOR_IDS];
    int num_connector_ids;
} MonitorId;

typedef struct {
    gsr_capture_kms_cuda_params params;

    bool should_stop;
    bool stop_is_error;
    bool created_hw_frame;

    gsr_cuda cuda;
    
    gsr_kms_client kms_client;
    gsr_kms_response kms_response;

    vec2i capture_pos;
    vec2i capture_size;
    MonitorId monitor_id;

    CUgraphicsResource cuda_graphics_resource;
    CUarray mapped_array;

    unsigned int input_texture;
    unsigned int cursor_texture;
    unsigned int target_texture;

    gsr_color_conversion color_conversion;

    AVCodecContext *video_codec_context;
    AVMasteringDisplayMetadata *mastering_display_metadata;
    AVContentLightMetadata *light_metadata;

    gsr_monitor_rotation monitor_rotation;
} gsr_capture_kms_cuda;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static void gsr_capture_kms_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static bool cuda_create_codec_context(gsr_capture_kms_cuda *cap_kms, AVCodecContext *video_codec_context) {
    CUcontext old_ctx;
    cap_kms->cuda.cuCtxPushCurrent_v2(cap_kms->cuda.cu_ctx);

    AVBufferRef *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!device_ctx) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = cap_kms->cuda.cu_ctx;
    if(av_hwdevice_ctx_init(device_ctx) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        av_buffer_unref(&device_ctx);
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_BGR0;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    hw_frame_context->initial_pool_size = 1;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

// TODO: On monitor reconfiguration, find monitor x, y, width and height again. Do the same for nvfbc.

typedef struct {
    gsr_capture_kms_cuda *cap_kms;
    const char *monitor_to_capture;
    int monitor_to_capture_len;
    int num_monitors;
} MonitorCallbackUserdata;

static void monitor_callback(const gsr_monitor *monitor, void *userdata) {
    MonitorCallbackUserdata *monitor_callback_userdata = userdata;
    ++monitor_callback_userdata->num_monitors;

    if(monitor_callback_userdata->monitor_to_capture_len != monitor->name_len || memcmp(monitor_callback_userdata->monitor_to_capture, monitor->name, monitor->name_len) != 0)
        return;

    if(monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids < MAX_CONNECTOR_IDS) {
        monitor_callback_userdata->cap_kms->monitor_id.connector_ids[monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids] = monitor->connector_id;
        ++monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids;
    }

    if(monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids == MAX_CONNECTOR_IDS)
        fprintf(stderr, "gsr warning: reached max connector ids\n");
}

static int gsr_capture_kms_cuda_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    cap_kms->video_codec_context = video_codec_context;

    gsr_monitor monitor;
    cap_kms->monitor_id.num_connector_ids = 0;

    int kms_init_res = gsr_kms_client_init(&cap_kms->kms_client, cap_kms->params.egl->card_path);
    if(kms_init_res != 0) {
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return kms_init_res;
    }

    MonitorCallbackUserdata monitor_callback_userdata = {
        cap_kms,
        cap_kms->params.display_to_capture, strlen(cap_kms->params.display_to_capture),
        0
    };
    for_each_active_monitor_output(cap_kms->params.egl, GSR_CONNECTION_DRM, monitor_callback, &monitor_callback_userdata);

    if(!get_monitor_by_name(cap_kms->params.egl, GSR_CONNECTION_DRM, cap_kms->params.display_to_capture, &monitor)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_cuda_start: failed to find monitor by name \"%s\"\n", cap_kms->params.display_to_capture);
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return -1;
    }

    monitor.name = cap_kms->params.display_to_capture;
    cap_kms->monitor_rotation = drm_monitor_get_display_server_rotation(cap_kms->params.egl, &monitor);

    cap_kms->capture_pos = monitor.pos;
    if(cap_kms->monitor_rotation == GSR_MONITOR_ROT_90 || cap_kms->monitor_rotation == GSR_MONITOR_ROT_270) {
        cap_kms->capture_size.x = monitor.size.y;
        cap_kms->capture_size.y = monitor.size.x;
    } else {
        cap_kms->capture_size = monitor.size;
    }

    video_codec_context->width = max_int(2, cap_kms->capture_size.x & ~1);
    video_codec_context->height = max_int(2, cap_kms->capture_size.y & ~1);

    /* Disable vsync */
    cap_kms->params.egl->eglSwapInterval(cap_kms->params.egl->egl_display, 0);

    // TODO: overclocking is not supported on wayland...
    if(!gsr_cuda_load(&cap_kms->cuda, NULL, false)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_cuda_start: failed to load cuda\n");
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return -1;
    }

    if(!cuda_create_codec_context(cap_kms, video_codec_context)) {
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        return -1;
    }

    return 0;
}

static unsigned int gl_create_texture(gsr_capture_kms_cuda *cap_kms, int width, int height) {
    unsigned int texture_id = 0;
    cap_kms->params.egl->glGenTextures(1, &texture_id);
    cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    cap_kms->params.egl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

static bool cuda_register_opengl_texture(gsr_capture_kms_cuda *cap_kms) {
    CUresult res;
    CUcontext old_ctx;
    res = cap_kms->cuda.cuCtxPushCurrent_v2(cap_kms->cuda.cu_ctx);
    // TODO: Use cuGraphicsEGLRegisterImage instead with the window egl image (dont use window_texture).
    // That removes the need for an extra texture and texture copy
    res = cap_kms->cuda.cuGraphicsGLRegisterImage(
        &cap_kms->cuda_graphics_resource, cap_kms->target_texture, GL_TEXTURE_2D,
        CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if (res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        cap_kms->cuda.cuGetErrorString(res, &err_str);
        fprintf(stderr, "gsr error: cuda_register_opengl_texture: cuGraphicsGLRegisterImage failed, error: %s, texture " "id: %u\n", err_str, cap_kms->target_texture);
        res = cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    res = cap_kms->cuda.cuGraphicsResourceSetMapFlags(cap_kms->cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    res = cap_kms->cuda.cuGraphicsMapResources(1, &cap_kms->cuda_graphics_resource, 0);

    res = cap_kms->cuda.cuGraphicsSubResourceGetMappedArray(&cap_kms->mapped_array, cap_kms->cuda_graphics_resource, 0, 0);
    res = cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
    return true;
}

static void gsr_capture_kms_cuda_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    if(!cap_kms->created_hw_frame) {
        cap_kms->created_hw_frame = true;

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_tick: failed to allocate frame\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
        (*frame)->format = video_codec_context->pix_fmt;
        (*frame)->width = video_codec_context->width;
        (*frame)->height = video_codec_context->height;
        (*frame)->color_range = video_codec_context->color_range;
        (*frame)->color_primaries = video_codec_context->color_primaries;
        (*frame)->color_trc = video_codec_context->color_trc;
        (*frame)->colorspace = video_codec_context->colorspace;
        (*frame)->chroma_location = video_codec_context->chroma_sample_location;

        if(av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0) < 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_tick: av_hwframe_get_buffer failed\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }

        cap_kms->params.egl->glGenTextures(1, &cap_kms->input_texture);
        cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, cap_kms->input_texture);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

        cap_kms->params.egl->glGenTextures(1, &cap_kms->cursor_texture);
        cap_kms->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, cap_kms->cursor_texture);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        cap_kms->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        cap_kms->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

        cap_kms->target_texture = gl_create_texture(cap_kms, video_codec_context->width, video_codec_context->height);
        if(cap_kms->target_texture == 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_tick: failed to create opengl texture\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }

        if(!cuda_register_opengl_texture(cap_kms)) {
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }

        gsr_color_conversion_params color_conversion_params = {0};
        color_conversion_params.egl = cap_kms->params.egl;
        color_conversion_params.source_color = GSR_SOURCE_COLOR_RGB;
        color_conversion_params.destination_color = GSR_DESTINATION_COLOR_BGR;

        color_conversion_params.destination_textures[0] = cap_kms->target_texture;
        color_conversion_params.num_destination_textures = 1;

        if(gsr_color_conversion_init(&cap_kms->color_conversion, &color_conversion_params) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_cuda_tick: failed to create color conversion\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
    }
}

static bool gsr_capture_kms_cuda_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;
    if(cap_kms->should_stop) {
        if(err)
            *err = cap_kms->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static float monitor_rotation_to_radians(gsr_monitor_rotation rot) {
    switch(rot) {
        case GSR_MONITOR_ROT_0:   return 0.0f;
        case GSR_MONITOR_ROT_90:  return M_PI_2;
        case GSR_MONITOR_ROT_180: return M_PI;
        case GSR_MONITOR_ROT_270: return M_PI + M_PI_2;
    }
    return 0.0f;
}

/* Prefer non combined planes */
static gsr_kms_response_fd* find_drm_by_connector_id(gsr_kms_response *kms_response, uint32_t connector_id) {
    int index_combined = -1;
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].connector_id == connector_id && !kms_response->fds[i].is_cursor) {
            if(kms_response->fds[i].is_combined_plane)
                index_combined = i;
            else
                return &kms_response->fds[i];
        }
    }

    if(index_combined != -1)
        return &kms_response->fds[index_combined];
    else
        return NULL;
}

static gsr_kms_response_fd* find_first_combined_drm(gsr_kms_response *kms_response) {
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].is_combined_plane && !kms_response->fds[i].is_cursor)
            return &kms_response->fds[i];
    }
    return NULL;
}

static gsr_kms_response_fd* find_largest_drm(gsr_kms_response *kms_response) {
    if(kms_response->num_fds == 0)
        return NULL;

    int64_t largest_size = 0;
    gsr_kms_response_fd *largest_drm = &kms_response->fds[0];
    for(int i = 0; i < kms_response->num_fds; ++i) {
        const int64_t size = (int64_t)kms_response->fds[i].width * (int64_t)kms_response->fds[i].height;
        if(size > largest_size && !kms_response->fds[i].is_cursor) {
            largest_size = size;
            largest_drm = &kms_response->fds[i];
        }
    }
    return largest_drm;
}

static void gsr_capture_kms_unload_cuda_graphics(gsr_capture_kms_cuda *cap_kms) {
    if(cap_kms->cuda.cu_ctx) {
        CUcontext old_ctx;
        cap_kms->cuda.cuCtxPushCurrent_v2(cap_kms->cuda.cu_ctx);

        if(cap_kms->cuda_graphics_resource) {
            cap_kms->cuda.cuGraphicsUnmapResources(1, &cap_kms->cuda_graphics_resource, 0);
            cap_kms->cuda.cuGraphicsUnregisterResource(cap_kms->cuda_graphics_resource);
            cap_kms->cuda_graphics_resource = 0;
        }

        cap_kms->cuda.cuCtxPopCurrent_v2(&old_ctx);
    }
}

static gsr_kms_response_fd* find_cursor_drm(gsr_kms_response *kms_response) {
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].is_cursor)
            return &kms_response->fds[i];
    }
    return NULL;
}

#define HDMI_STATIC_METADATA_TYPE1 0
#define HDMI_EOTF_SMPTE_ST2084 2

static bool hdr_metadata_is_supported_format(const struct hdr_output_metadata *hdr_metadata) {
    return hdr_metadata->metadata_type == HDMI_STATIC_METADATA_TYPE1 &&
        hdr_metadata->hdmi_metadata_type1.metadata_type == HDMI_STATIC_METADATA_TYPE1 &&
        hdr_metadata->hdmi_metadata_type1.eotf == HDMI_EOTF_SMPTE_ST2084;
}

static void gsr_capture_kms_vaapi_set_hdr_metadata(gsr_capture_kms_cuda *cap_kms, AVFrame *frame, gsr_kms_response_fd *drm_fd) {
    if(!cap_kms->mastering_display_metadata)
        cap_kms->mastering_display_metadata = av_mastering_display_metadata_create_side_data(frame);

    if(!cap_kms->light_metadata)
        cap_kms->light_metadata = av_content_light_metadata_create_side_data(frame);

    if(cap_kms->mastering_display_metadata) {
        for(int i = 0; i < 3; ++i) {
            cap_kms->mastering_display_metadata->display_primaries[i][0] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.display_primaries[i].x, 50000);
            cap_kms->mastering_display_metadata->display_primaries[i][1] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.display_primaries[i].y, 50000);
        }

        cap_kms->mastering_display_metadata->white_point[0] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.white_point.x, 50000);
        cap_kms->mastering_display_metadata->white_point[1] = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.white_point.y, 50000);

        cap_kms->mastering_display_metadata->min_luminance = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance, 10000);
        cap_kms->mastering_display_metadata->max_luminance = av_make_q(drm_fd->hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance, 1);

        cap_kms->mastering_display_metadata->has_primaries = cap_kms->mastering_display_metadata->display_primaries[0][0].num > 0;
        cap_kms->mastering_display_metadata->has_luminance = cap_kms->mastering_display_metadata->max_luminance.num > 0;
    }

    if(cap_kms->light_metadata) {
        cap_kms->light_metadata->MaxCLL = drm_fd->hdr_metadata.hdmi_metadata_type1.max_cll;
        cap_kms->light_metadata->MaxFALL = drm_fd->hdr_metadata.hdmi_metadata_type1.max_fall;
    }
}

static vec2i swap_vec2i(vec2i value) {
    int tmp = value.x;
    value.x = value.y;
    value.y = tmp;
    return value;
}

static int gsr_capture_kms_cuda_capture(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    cap_kms->params.egl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    cap_kms->params.egl->glClear(GL_COLOR_BUFFER_BIT);

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;

    gsr_kms_response_fd *drm_fd = NULL;
    gsr_kms_response_fd *cursor_drm_fd = NULL;
    bool capture_is_combined_plane = false;

    if(gsr_kms_client_get_kms(&cap_kms->kms_client, &cap_kms->kms_response) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: failed to get kms, error: %d (%s)\n", cap_kms->kms_response.result, cap_kms->kms_response.err_msg);
        return -1;
    }

    if(cap_kms->kms_response.num_fds == 0) {
        static bool error_shown = false;
        if(!error_shown) {
            error_shown = true;
            fprintf(stderr, "gsr error: no drm found, capture will fail\n");
        }
        return -1;
    }

    for(int i = 0; i < cap_kms->monitor_id.num_connector_ids; ++i) {
        drm_fd = find_drm_by_connector_id(&cap_kms->kms_response, cap_kms->monitor_id.connector_ids[i]);
        if(drm_fd)
            break;
    }

    // Will never happen on wayland unless the target monitor has been disconnected
    if(!drm_fd) {
        drm_fd = find_first_combined_drm(&cap_kms->kms_response);
        if(!drm_fd)
            drm_fd = find_largest_drm(&cap_kms->kms_response);
        capture_is_combined_plane = true;
    }

    cursor_drm_fd = find_cursor_drm(&cap_kms->kms_response);

    if(!drm_fd)
        return -1;

    if(!capture_is_combined_plane && cursor_drm_fd && cursor_drm_fd->connector_id != drm_fd->connector_id)
        cursor_drm_fd = NULL;

    if(drm_fd->has_hdr_metadata && cap_kms->params.hdr && hdr_metadata_is_supported_format(&drm_fd->hdr_metadata))
        gsr_capture_kms_vaapi_set_hdr_metadata(cap_kms, frame, drm_fd);

    const intptr_t img_attr[] = {
        //EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_LINUX_DRM_FOURCC_EXT,       drm_fd->pixel_format,//cap_kms->params.egl->pixel_format, ARGB8888
        EGL_WIDTH,                      drm_fd->width,//cap_kms->params.egl->width,
        EGL_HEIGHT,                     drm_fd->height,//cap_kms->params.egl->height,
        EGL_DMA_BUF_PLANE0_FD_EXT,      drm_fd->fd,//cap_kms->params.egl->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  drm_fd->offset,//cap_kms->params.egl->offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   drm_fd->pitch,//cap_kms->params.egl->pitch,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, drm_fd->modifier & 0xFFFFFFFFULL,//cap_kms->params.egl->modifier & 0xFFFFFFFFULL,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, drm_fd->modifier >> 32ULL,//cap_kms->params.egl->modifier >> 32ULL,
        EGL_NONE
    };

    EGLImage image = cap_kms->params.egl->eglCreateImage(cap_kms->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
    cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, cap_kms->input_texture);
    cap_kms->params.egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    cap_kms->params.egl->eglDestroyImage(cap_kms->params.egl->egl_display, image);
    cap_kms->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    vec2i capture_pos = cap_kms->capture_pos;
    if(!capture_is_combined_plane)
        capture_pos = (vec2i){drm_fd->x, drm_fd->y};

    const float texture_rotation = monitor_rotation_to_radians(cap_kms->monitor_rotation);

    gsr_color_conversion_draw(&cap_kms->color_conversion, cap_kms->input_texture,
        (vec2i){0, 0}, cap_kms->capture_size,
        capture_pos, cap_kms->capture_size,
        texture_rotation, false);

    if(cursor_drm_fd) {
        const vec2i cursor_size = {cursor_drm_fd->width, cursor_drm_fd->height};
        vec2i cursor_pos = {cursor_drm_fd->x, cursor_drm_fd->y};
        switch(cap_kms->monitor_rotation) {
            case GSR_MONITOR_ROT_0:
                break;
            case GSR_MONITOR_ROT_90:
                cursor_pos = swap_vec2i(cursor_pos);
                cursor_pos.x = cap_kms->capture_size.x - cursor_pos.x;
                // TODO: Remove this horrible hack
                cursor_pos.x -= cursor_size.x;
                break;
            case GSR_MONITOR_ROT_180:
                cursor_pos.x = cap_kms->capture_size.x - cursor_pos.x;
                cursor_pos.y = cap_kms->capture_size.y - cursor_pos.y;
                // TODO: Remove this horrible hack
                cursor_pos.x -= cursor_size.x;
                cursor_pos.y -= cursor_size.y;
                break;
            case GSR_MONITOR_ROT_270:
                cursor_pos = swap_vec2i(cursor_pos);
                cursor_pos.y = cap_kms->capture_size.y - cursor_pos.y;
                // TODO: Remove this horrible hack
                cursor_pos.y -= cursor_size.y;
                break;
        }

        const intptr_t img_attr_cursor[] = {
            EGL_LINUX_DRM_FOURCC_EXT,       cursor_drm_fd->pixel_format,
            EGL_WIDTH,                      cursor_drm_fd->width,
            EGL_HEIGHT,                     cursor_drm_fd->height,
            EGL_DMA_BUF_PLANE0_FD_EXT,      cursor_drm_fd->fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,  cursor_drm_fd->offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,   cursor_drm_fd->pitch,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, cursor_drm_fd->modifier & 0xFFFFFFFFULL,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, cursor_drm_fd->modifier >> 32ULL,
            EGL_NONE
        };

        EGLImage cursor_image = cap_kms->params.egl->eglCreateImage(cap_kms->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr_cursor);
        cap_kms->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, cap_kms->cursor_texture);
        cap_kms->params.egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, cursor_image);
        cap_kms->params.egl->eglDestroyImage(cap_kms->params.egl->egl_display, cursor_image);
        cap_kms->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

        gsr_color_conversion_draw(&cap_kms->color_conversion, cap_kms->cursor_texture,
            cursor_pos, cursor_size,
            (vec2i){0, 0}, cursor_size,
            texture_rotation, true);
    }

    cap_kms->params.egl->eglSwapBuffers(cap_kms->params.egl->egl_display, cap_kms->params.egl->egl_surface);

    frame->linesize[0] = frame->width * 4;

    CUDA_MEMCPY2D memcpy_struct;
    memcpy_struct.srcXInBytes = 0;
    memcpy_struct.srcY = 0;
    memcpy_struct.srcMemoryType = CU_MEMORYTYPE_ARRAY;

    memcpy_struct.dstXInBytes = 0;
    memcpy_struct.dstY = 0;
    memcpy_struct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

    memcpy_struct.srcArray = cap_kms->mapped_array;
    memcpy_struct.srcPitch = frame->linesize[0];
    memcpy_struct.dstDevice = (CUdeviceptr)frame->data[0];
    memcpy_struct.dstPitch = frame->linesize[0];
    memcpy_struct.WidthInBytes = frame->width * 4;
    memcpy_struct.Height = frame->height;
    cap_kms->cuda.cuMemcpy2D_v2(&memcpy_struct);

    return 0;
}

static void gsr_capture_kms_cuda_capture_end(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;
}

static void gsr_capture_kms_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_cuda *cap_kms = cap->priv;

    gsr_color_conversion_deinit(&cap_kms->color_conversion);

    gsr_capture_kms_unload_cuda_graphics(cap_kms);

    if(cap_kms->params.egl->egl_context) {
        if(cap_kms->input_texture) {
            cap_kms->params.egl->glDeleteTextures(1, &cap_kms->input_texture);
            cap_kms->input_texture = 0;
        }

        if(cap_kms->cursor_texture) {
            cap_kms->params.egl->glDeleteTextures(1, &cap_kms->cursor_texture);
            cap_kms->cursor_texture = 0;
        }

        if(cap_kms->target_texture) {
            cap_kms->params.egl->glDeleteTextures(1, &cap_kms->target_texture);
            cap_kms->target_texture = 0;
        }
    }

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_cuda_unload(&cap_kms->cuda);
    gsr_kms_client_deinit(&cap_kms->kms_client);
}

static void gsr_capture_kms_cuda_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms_cuda *cap_kms = cap->priv;
    if(cap->priv) {
        gsr_capture_kms_cuda_stop(cap, video_codec_context);
        free((void*)cap_kms->params.display_to_capture);
        cap_kms->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_kms_cuda_create(const gsr_capture_kms_cuda_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_kms_cuda_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_kms_cuda *cap_kms = calloc(1, sizeof(gsr_capture_kms_cuda));
    if(!cap_kms) {
        free(cap);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        free(cap);
        free(cap_kms);
        return NULL;
    }

    cap_kms->params = *params;
    cap_kms->params.display_to_capture = display_to_capture;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_kms_cuda_start,
        .tick = gsr_capture_kms_cuda_tick,
        .should_stop = gsr_capture_kms_cuda_should_stop,
        .capture = gsr_capture_kms_cuda_capture,
        .capture_end = gsr_capture_kms_cuda_capture_end,
        .destroy = gsr_capture_kms_cuda_destroy,
        .priv = cap_kms
    };

    return cap;
}
