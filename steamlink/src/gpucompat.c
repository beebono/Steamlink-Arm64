#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <drm/msm_drm.h>
#include <drm/panfrost_drm.h>
#include <linux/dma-heap.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_stdinc.h>

// Device path options
#define DMA_HEAP_PATH "/dev/dma_heap/vidbuf_cached"
#define REAL_REND_DEVICE "/dev/dri/renderD128"
#define REAL_CARD_DEVICE "/dev/dri/card0"
#define FAKE_DRM_DEVICE "/tmp/drmpipe"

// Fake/Dumb Buffer limit and struct
#define MAX_BUFFERS 8

// Required fcntl.h defines
#define O_RDONLY    00000000
#define O_RDWR      00000002
#define O_NONBLOCK  00004000
#define O_CLOEXEC   02000000
#define AT_FDCWD        -100

typedef struct {
    int fds[MAX_BUFFERS];
    int count;
} BufferManager;

static BufferManager manager = { .fds = { -1 }, .count = 0 };

// Static data stores
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static bool (*real_sdl_initsub)(SDL_InitFlags) = NULL;
static void (*real_sdl_quit)(void) = NULL;
static int fake_drm_fd = -1;
static char drm_dri_name[16];

// DRM Driver version check
int get_gpu_dri(int fd) {
    struct drm_version version = {
        .name = drm_dri_name,
        .name_len = sizeof(drm_dri_name)
    };
    ioctl(fd, DRM_IOCTL_VERSION, &version);
    if (drm_dri_name[0] == '\0') {
        return -1;
    }
    return 0;
}

// DRM Device FD -> GEM Handle -> DRM Prime FD
int drm_to_dma(int fd, size_t size) {
    if (drm_dri_name[0] == '\0') {
        if (get_gpu_dri(fd) != 0) {
            fprintf(stderr, "ERROR: Failed to get GPU driver info!\n");
            return -1;
        }
    }

    uint32_t gem_handle = 0;
    if (strcmp(drm_dri_name, "msm") == 0) {
        struct drm_msm_gem_new gem = {
            .size = size,
            .flags = MSM_BO_WC
        };
        ioctl(fd, DRM_IOCTL_MSM_GEM_NEW, &gem);
        gem_handle = gem.handle;
    } else if (strcmp(drm_dri_name, "panfrost") == 0) {
        struct drm_panfrost_create_bo gem = {
            .size = size,
            .flags = 0
        };
        ioctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &gem);
        gem_handle = gem.handle;
    } else {
        fprintf(stderr, "ERROR: GPU driver '%s' is unsupported!\n", drm_dri_name);
        return -1;
    }

    struct drm_prime_handle prime = {
        .handle = gem_handle,
        .flags = DRM_RDWR | DRM_CLOEXEC
    };
    ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);

    return prime.fd;
}

// Allocate and return a fake buffer FD using memfd
int fake_drm_to_dma(size_t size) {
    if (manager.count >= MAX_BUFFERS) {
        fprintf(stderr, "Maximum amount of buffers (%d) reached!\n", MAX_BUFFERS);
        return -1;
    }

    int fd = memfd_create("fake-buffer", MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    manager.fds[manager.count++] = fd;
    return fd;
}

// Actual syscall for open
int real_open(const char *path, int flags, mode_t mode) {
    return syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

// Catch any calls that try to use DMA_HEAP and provide a device appropriate FD
int open64(const char *path, int flags, mode_t mode) {
    if (strcmp(path, DMA_HEAP_PATH) == 0) {
        return real_open(REAL_REND_DEVICE, flags, mode);
    } else if (strcmp(path, REAL_CARD_DEVICE) == 0 && fake_drm_fd > 0) {
        return fake_drm_fd;
    }
    return real_open(path, flags, mode);
}

// Because controls don't work unless this is routed this way
int open(const char *path, int flags, mode_t mode) {
    return open64(path, flags, mode);
}

// Catch any calls that try to use DMA_HEAP and provide a usable DMA-BUF FD
int ioctl(int fd, unsigned long request, ...) {
    if (!real_ioctl) { real_ioctl = dlsym(RTLD_NEXT, "ioctl"); }

    va_list args;
    va_start(args, request);

    if (request == DMA_HEAP_IOCTL_ALLOC) {
        struct dma_heap_allocation_data *data = va_arg(args, struct dma_heap_allocation_data*);
        data->fd = (fd == fake_drm_fd) ? fake_drm_to_dma(data->len) : drm_to_dma(fd, data->len);
        va_end(args);
        return 0;
    }

    if (fd == fake_drm_fd) {
        switch (request) {
            case DRM_IOCTL_SET_MASTER:
            case DRM_IOCTL_DROP_MASTER:
            case DRM_IOCTL_MODE_GETRESOURCES:
                va_end(args);
                return 0;
            default:
                va_end(args);
                return -1;
        }
    }

    void *data = va_arg(args, void*);
    int ret = real_ioctl(fd, request, data);
    va_end(args);
    return ret;
}

// Create a fake DRM device before trying to initialize a certain SDL_VIDEO_DRIVER
bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (!real_sdl_initsub) { real_sdl_initsub = dlsym(RTLD_NEXT, "SDL_Init"); }

    if (flags & SDL_INIT_VIDEO && // SDL added an underscore to the envvar in SDL3, check for it
        (getenv("SDL_VIDEODRIVER") && strcmp(getenv("SDL_VIDEODRIVER"), "kmsdrm") == 0) ||
        (getenv("SDL_VIDEO_DRIVER") && strcmp(getenv("SDL_VIDEO_DRIVER"), "kmsdrm") == 0)) {
        if (fake_drm_fd == -1) { // Just in case this is called twice
            umask(0); // Ensure the right mask is set
            mknod(FAKE_DRM_DEVICE, S_IFIFO|0666, 0);
            fake_drm_fd = open(FAKE_DRM_DEVICE, O_RDONLY | O_NONBLOCK, 0);
        }
    }
    return real_sdl_initsub(flags);
}

// Clean up the fake DRM device and any fake buffers on stream/program end
void SDL_Quit(void) {
    if (!real_sdl_quit) { real_sdl_quit = dlsym(RTLD_NEXT, "SDL_Quit"); }

    if (fake_drm_fd > 0) {
        close(fake_drm_fd);
        remove(FAKE_DRM_DEVICE);
    }

    if (manager.count > 0) {
        for (int i = 0; i < manager.count; i++) {
            if (manager.fds[i] != -1) {
                close(manager.fds[i]);
                manager.fds[i] = -1;
            }
        }
        manager.count = 0;
    }
    real_sdl_quit();
}
