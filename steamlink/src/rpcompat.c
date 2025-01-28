/*
*   AUTHOR: Noxwell
*/

#define _GNU_SOURCE

#include <fcntl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <drm/msm_drm.h>
#include <drm/panfrost_drm.h>
#include <linux/dma-heap.h>

#define DMA_HEAP_PATH "/dev/dma_heap/vidbuf_cached"
#define DRM_DEVICE_PATH "/dev/dri/renderD128"

// Static data stores
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
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

// Catch any calls that try to use DMA_HEAP and provide a device appropriate FD
int open(const char *path, int flags, ...) {
    if (!real_open) { real_open = dlsym(RTLD_NEXT, "open"); }

    int fd;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, mode_t);
        if (path[0] == '/' && memcmp(path, DMA_HEAP_PATH, sizeof(DMA_HEAP_PATH)) == 0) {
            fd = real_open(DRM_DEVICE_PATH, flags, mode);
            va_end(args);
            return fd;
        }
        fd = real_open(path, flags, mode);
        va_end(args);
        return fd;
    }

    if (path[0] == '/' && memcmp(path, DMA_HEAP_PATH, sizeof(DMA_HEAP_PATH)) == 0) {
        fd = real_open(DRM_DEVICE_PATH, flags);
        return fd;
    }
    return real_open(path, flags);
}

// Catch any calls that try to use DMA_HEAP and provide a usable DMA-BUF FD
int ioctl(int fd, unsigned long request, ...) {
    if (!real_ioctl) { real_ioctl = dlsym(RTLD_NEXT, "ioctl"); }

    va_list args;
    va_start(args, request);
    if (request == DMA_HEAP_IOCTL_ALLOC) {
        struct dma_heap_allocation_data *data = va_arg(args, struct dma_heap_allocation_data*);
        data->fd = drm_to_dma(fd, data->len);
        va_end(args);
        return 0;
    }
    void *data = va_arg(args, void*);
    int ret = real_ioctl(fd, request, data);
    va_end(args);
    return ret;
}