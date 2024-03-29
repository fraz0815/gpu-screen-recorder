#include "../include/egl.h"
#include "../include/library_loader.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <assert.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <unistd.h>
#include <sys/capability.h>

// Move this shit to a separate wayland file, and have a separate file for x11.

static void output_handle_geometry(void *data, struct wl_output *wl_output,
        int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
        int32_t subpixel, const char *make, const char *model,
        int32_t transform) {
    (void)wl_output;
    (void)phys_width;
    (void)phys_height;
    (void)subpixel;
    (void)make;
    (void)model;
    gsr_wayland_output *gsr_output = data;
    gsr_output->pos.x = x;
    gsr_output->pos.y = y;
    gsr_output->transform = transform;
}

static void output_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)wl_output;
    (void)flags;
    (void)refresh;
    gsr_wayland_output *gsr_output = data;
    gsr_output->size.x = width;
    gsr_output->size.y = height;
}

static void output_handle_done(void *data, struct wl_output *wl_output) {
    (void)data;
    (void)wl_output;
}

static void output_handle_scale(void* data, struct wl_output *wl_output, int32_t factor) {
    (void)data;
    (void)wl_output;
    (void)factor;
}

static void output_handle_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)wl_output;
    gsr_wayland_output *gsr_output = data;
    if(gsr_output->name) {
        free(gsr_output->name);
        gsr_output->name = NULL;
    }
    gsr_output->name = strdup(name);
}

static void output_handle_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)data;
    (void)wl_output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .name = output_handle_name,
    .description = output_handle_description,
};

static void registry_add_object(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)version;
    gsr_egl *egl = data;
    if (strcmp(interface, "wl_compositor") == 0) {
        if(egl->wayland.compositor) {
            wl_compositor_destroy(egl->wayland.compositor);
            egl->wayland.compositor = NULL;
        }
        egl->wayland.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if(strcmp(interface, wl_output_interface.name) == 0) {
        if(version < 4) {
            fprintf(stderr, "gsr warning: wl output interface version is < 4, expected >= 4 to capture a monitor. Using KMS capture instead\n");
            return;
        }

        if(egl->wayland.num_outputs == GSR_MAX_OUTPUTS) {
            fprintf(stderr, "gsr warning: reached maximum outputs (32), ignoring output %u\n", name);
            return;
        }

        gsr_wayland_output *gsr_output = &egl->wayland.outputs[egl->wayland.num_outputs];
        egl->wayland.num_outputs++;
        *gsr_output = (gsr_wayland_output) {
            .wl_name = name,
            .output = wl_registry_bind(registry, name, &wl_output_interface, 4),
            .pos = { .x = 0, .y = 0 },
            .size = { .x = 0, .y = 0 },
            .transform = 0,
            .name = NULL,
        };
        wl_output_add_listener(gsr_output->output, &output_listener, gsr_output);
    }
}

static void registry_remove_object(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static struct wl_registry_listener registry_listener = {
    .global = registry_add_object,
    .global_remove = registry_remove_object,
};

static void reset_cap_nice(void) {
    cap_t caps = cap_get_proc();
    if(!caps)
        return;

    const cap_value_t cap_to_remove = CAP_SYS_NICE;
    cap_set_flag(caps, CAP_EFFECTIVE, 1, &cap_to_remove, CAP_CLEAR);
    cap_set_flag(caps, CAP_PERMITTED, 1, &cap_to_remove, CAP_CLEAR);
    cap_set_proc(caps);
    cap_free(caps);
}

// TODO: Create egl context without surface (in other words, x11/wayland agnostic, doesn't require x11/wayland dependency)
static bool gsr_egl_create_window(gsr_egl *self, bool wayland) {
    EGLConfig  ecfg;
    int32_t    num_config = 0;

    const int32_t attr[] = {
        EGL_BUFFER_SIZE, 24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    const int32_t ctxattr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_CONTEXT_PRIORITY_LEVEL_IMG, EGL_CONTEXT_PRIORITY_HIGH_IMG, /* requires cap_sys_nice, ignored otherwise */
        EGL_NONE
    };

    if(wayland) {
        self->wayland.dpy = wl_display_connect(NULL);
        if(!self->wayland.dpy) {
            fprintf(stderr, "gsr error: gsr_egl_create_window failed: wl_display_connect failed\n");
            goto fail;
        }

        self->wayland.registry = wl_display_get_registry(self->wayland.dpy); // TODO: Error checking
        wl_registry_add_listener(self->wayland.registry, &registry_listener, self); // TODO: Error checking

        // Fetch globals
        wl_display_roundtrip(self->wayland.dpy);

        // Fetch wl_output
        wl_display_roundtrip(self->wayland.dpy);

        if(!self->wayland.compositor) {
            fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to find compositor\n");
            goto fail;
        }
    } else {
        self->x11.window = XCreateWindow(self->x11.dpy, DefaultRootWindow(self->x11.dpy), 0, 0, 16, 16, 0, CopyFromParent, InputOutput, CopyFromParent, 0, NULL);

        if(!self->x11.window) {
            fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to create gl window\n");
            goto fail;
        }
    }

    self->eglBindAPI(EGL_OPENGL_API);

    self->egl_display = self->eglGetDisplay(self->wayland.dpy ? (EGLNativeDisplayType)self->wayland.dpy : (EGLNativeDisplayType)self->x11.dpy);
    if(!self->egl_display) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglGetDisplay failed\n");
        goto fail;
    }

    if(!self->eglInitialize(self->egl_display, NULL, NULL)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglInitialize failed\n");
        goto fail;
    }
    
    if(!self->eglChooseConfig(self->egl_display, attr, &ecfg, 1, &num_config) || num_config != 1) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to find a matching config\n");
        goto fail;
    }
    
    self->egl_context = self->eglCreateContext(self->egl_display, ecfg, NULL, ctxattr);
    if(!self->egl_context) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create egl context\n");
        goto fail;
    }

    if(wayland) {
        self->wayland.surface = wl_compositor_create_surface(self->wayland.compositor);
        self->wayland.window = wl_egl_window_create(self->wayland.surface, 16, 16);
        self->egl_surface = self->eglCreateWindowSurface(self->egl_display, ecfg, (EGLNativeWindowType)self->wayland.window, NULL);
    } else {
        self->egl_surface = self->eglCreateWindowSurface(self->egl_display, ecfg, (EGLNativeWindowType)self->x11.window, NULL);
    }

    if(!self->egl_surface) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create window surface\n");
        goto fail;
    }

    if(!self->eglMakeCurrent(self->egl_display, self->egl_surface, self->egl_surface, self->egl_context)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to make context current\n");
        goto fail;
    }

    reset_cap_nice();
    return true;

    fail:
    reset_cap_nice();
    gsr_egl_unload(self);
    return false;
}

static bool gsr_egl_load_egl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->eglGetError, "eglGetError" },
        { (void**)&self->eglGetDisplay, "eglGetDisplay" },
        { (void**)&self->eglInitialize, "eglInitialize" },
        { (void**)&self->eglTerminate, "eglTerminate" },
        { (void**)&self->eglChooseConfig, "eglChooseConfig" },
        { (void**)&self->eglCreateWindowSurface, "eglCreateWindowSurface" },
        { (void**)&self->eglCreateContext, "eglCreateContext" },
        { (void**)&self->eglMakeCurrent, "eglMakeCurrent" },
        { (void**)&self->eglCreateImage, "eglCreateImage" },
        { (void**)&self->eglDestroyContext, "eglDestroyContext" },
        { (void**)&self->eglDestroySurface, "eglDestroySurface" },
        { (void**)&self->eglDestroyImage, "eglDestroyImage" },
        { (void**)&self->eglSwapInterval, "eglSwapInterval" },
        { (void**)&self->eglSwapBuffers, "eglSwapBuffers" },
        { (void**)&self->eglBindAPI, "eglBindAPI" },
        { (void**)&self->eglGetProcAddress, "eglGetProcAddress" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libEGL.so.1\n");
        return false;
    }

    return true;
}

static bool gsr_egl_proc_load_egl(gsr_egl *self) {
    self->eglExportDMABUFImageQueryMESA = (FUNC_eglExportDMABUFImageQueryMESA)self->eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    self->eglExportDMABUFImageMESA = (FUNC_eglExportDMABUFImageMESA)self->eglGetProcAddress("eglExportDMABUFImageMESA");
    self->glEGLImageTargetTexture2DOES = (FUNC_glEGLImageTargetTexture2DOES)self->eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if(!self->glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: could not find glEGLImageTargetTexture2DOES\n");
        return false;
    }

    return true;
}

static bool gsr_egl_load_gl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->glGetError, "glGetError" },
        { (void**)&self->glGetString, "glGetString" },
        { (void**)&self->glFlush, "glFlush" },
        { (void**)&self->glFinish, "glFinish" },
        { (void**)&self->glClear, "glClear" },
        { (void**)&self->glClearColor, "glClearColor" },
        { (void**)&self->glGenTextures, "glGenTextures" },
        { (void**)&self->glDeleteTextures, "glDeleteTextures" },
        { (void**)&self->glBindTexture, "glBindTexture" },
        { (void**)&self->glTexParameteri, "glTexParameteri" },
        { (void**)&self->glGetTexLevelParameteriv, "glGetTexLevelParameteriv" },
        { (void**)&self->glTexImage2D, "glTexImage2D" },
        { (void**)&self->glCopyImageSubData, "glCopyImageSubData" },
        { (void**)&self->glClearTexImage, "glClearTexImage" },
        { (void**)&self->glGenFramebuffers, "glGenFramebuffers" },
        { (void**)&self->glBindFramebuffer, "glBindFramebuffer" },
        { (void**)&self->glDeleteFramebuffers, "glDeleteFramebuffers" },
        { (void**)&self->glViewport, "glViewport" },
        { (void**)&self->glFramebufferTexture2D, "glFramebufferTexture2D" },
        { (void**)&self->glDrawBuffers, "glDrawBuffers" },
        { (void**)&self->glCheckFramebufferStatus, "glCheckFramebufferStatus" },
        { (void**)&self->glBindBuffer, "glBindBuffer" },
        { (void**)&self->glGenBuffers, "glGenBuffers" },
        { (void**)&self->glBufferData, "glBufferData" },
        { (void**)&self->glBufferSubData, "glBufferSubData" },
        { (void**)&self->glDeleteBuffers, "glDeleteBuffers" },
        { (void**)&self->glGenVertexArrays, "glGenVertexArrays" },
        { (void**)&self->glBindVertexArray, "glBindVertexArray" },
        { (void**)&self->glDeleteVertexArrays, "glDeleteVertexArrays" },
        { (void**)&self->glCreateProgram, "glCreateProgram" },
        { (void**)&self->glCreateShader, "glCreateShader" },
        { (void**)&self->glAttachShader, "glAttachShader" },
        { (void**)&self->glBindAttribLocation, "glBindAttribLocation" },
        { (void**)&self->glCompileShader, "glCompileShader" },
        { (void**)&self->glLinkProgram, "glLinkProgram" },
        { (void**)&self->glShaderSource, "glShaderSource" },
        { (void**)&self->glUseProgram, "glUseProgram" },
        { (void**)&self->glGetProgramInfoLog, "glGetProgramInfoLog" },
        { (void**)&self->glGetShaderiv, "glGetShaderiv" },
        { (void**)&self->glGetShaderInfoLog, "glGetShaderInfoLog" },
        { (void**)&self->glDeleteProgram, "glDeleteProgram" },
        { (void**)&self->glDeleteShader, "glDeleteShader" },
        { (void**)&self->glGetProgramiv, "glGetProgramiv" },
        { (void**)&self->glVertexAttribPointer, "glVertexAttribPointer" },
        { (void**)&self->glEnableVertexAttribArray, "glEnableVertexAttribArray" },
        { (void**)&self->glDrawArrays, "glDrawArrays" },
        { (void**)&self->glEnable, "glEnable" },
        { (void**)&self->glBlendFunc, "glBlendFunc" },
        { (void**)&self->glGetUniformLocation, "glGetUniformLocation" },
        { (void**)&self->glUniform1f, "glUniform1f" },
        { (void**)&self->glUniform2f, "glUniform2f" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libGL.so.1\n");
        return false;
    }

    return true;
}

bool gsr_egl_load(gsr_egl *self, Display *dpy, bool wayland) {
    memset(self, 0, sizeof(gsr_egl));
    self->x11.dpy = dpy;

    void *egl_lib = NULL;
    void *gl_lib = NULL;

    dlerror(); /* clear */
    egl_lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libEGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    gl_lib = dlopen("libGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    if(!gsr_egl_load_egl(self, egl_lib))
        goto fail;

    if(!gsr_egl_load_gl(self, gl_lib))
        goto fail;

    if(!gsr_egl_proc_load_egl(self))
        goto fail;

    if(!gsr_egl_create_window(self, wayland))
        goto fail;

    self->glEnable(GL_BLEND);
    self->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    self->egl_library = egl_lib;
    self->gl_library = gl_lib;
    return true;

    fail:
    if(egl_lib)
        dlclose(egl_lib);
    if(gl_lib)
        dlclose(gl_lib);
    memset(self, 0, sizeof(gsr_egl));
    return false;
}

void gsr_egl_unload(gsr_egl *self) {
    if(self->egl_context) {
        self->eglDestroyContext(self->egl_display, self->egl_context);
        self->egl_context = NULL;
    }

    if(self->egl_surface) {
        self->eglDestroySurface(self->egl_display, self->egl_surface);
        self->egl_surface = NULL;
    }

    if(self->egl_display) {
        self->eglTerminate(self->egl_display);
        self->egl_display = NULL;
    }

    if(self->x11.window) {
        XDestroyWindow(self->x11.dpy, self->x11.window);
        self->x11.window = None;
    }

    if(self->wayland.window) {
        wl_egl_window_destroy(self->wayland.window);
        self->wayland.window = NULL;
    }

    if(self->wayland.surface) {
        wl_surface_destroy(self->wayland.surface);
        self->wayland.surface = NULL;
    }

    for(int i = 0; i < self->wayland.num_outputs; ++i) {
        if(self->wayland.outputs[i].output) {
            wl_output_destroy(self->wayland.outputs[i].output);
            self->wayland.outputs[i].output = NULL;
        }

        if(self->wayland.outputs[i].name) {
            free(self->wayland.outputs[i].name);
            self->wayland.outputs[i].name = NULL;
        }
    }
    self->wayland.num_outputs = 0;

    if(self->wayland.compositor) {
        wl_compositor_destroy(self->wayland.compositor);
        self->wayland.compositor = NULL;
    }

    if(self->wayland.registry) {
        wl_registry_destroy(self->wayland.registry);
        self->wayland.registry = NULL;
    }

    if(self->wayland.dpy) {
        wl_display_disconnect(self->wayland.dpy);
        self->wayland.dpy = NULL;
    }

    if(self->egl_library) {
        dlclose(self->egl_library);
        self->egl_library = NULL;
    }

    if(self->gl_library) {
        dlclose(self->gl_library);
        self->gl_library = NULL;
    }

    memset(self, 0, sizeof(gsr_egl));
}

void gsr_egl_update(gsr_egl *self) {
    if(!self->wayland.dpy)
        return;

    // TODO: pselect on wl_display_get_fd before doing dispatch
    wl_display_dispatch(self->wayland.dpy);
}
