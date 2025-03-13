#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <drm/msm_drm.h>
#include <drm/panfrost_drm.h>
#include <linux/dma-heap.h>
#include <linux/ion.h>

// Device path options
#define DMA_HEAP_PATH "/dev/dma_heap/vidbuf_cached"
#define REAL_DRM_DEVICE "/dev/dri/renderD128"
#define REAL_ION_DEVICE "/dev/ion"

// Required fcntl.h defines
#define O_RDWR        00000002
#define O_CLOEXEC     02000000
#define AT_FDCWD          -100

// Function/Data stores
int (*open64_new)(const char *, int, __u32);
int (*real_ioctl)(int, unsigned long, ...);
int (*get_fd)(int, size_t);
__u32 (*xyz_to_handle)(int, size_t);
int cma_heap_id = -1;

// Actual syscall for open
int real_open(const char *path, int flags, __u32 mode) {
    return syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

// Catch any calls that try to use DMA_HEAP and provide a device appropriate FD
int open64_drm(const char *path, int flags, __u32 mode) {
    if (strcmp(path, DMA_HEAP_PATH) == 0) {
        return real_open(REAL_DRM_DEVICE, flags, mode);
    }
    return real_open(path, flags, mode);
}

int open64_ion(const char *path, int flags, __u32 mode) {
    if (strcmp(path, DMA_HEAP_PATH) == 0) {
        return real_open(REAL_ION_DEVICE, flags, mode);
    }
    return real_open(path, flags, mode);
}

int open64_shm(const char *path, int flags, __u32 mode) {
    if (strcmp(path, DMA_HEAP_PATH) == 0) {
        return 0;
    }
    return real_open(path, flags, mode);
}

// Because controls don't work unless this is routed this way
int open(const char *path, int flags, __u32 mode) {
    return open64_new(path, flags, mode);
}

// Catch any calls that try to use DMA_HEAP and provide a usable DMA-BUF FD
int ioctl(int fd, unsigned long request, ...) {
    if (!real_ioctl) { real_ioctl = dlsym(RTLD_NEXT, "ioctl"); }

    va_list args;
    va_start(args, request);

    if (request == DMA_HEAP_IOCTL_ALLOC) {
        struct dma_heap_allocation_data *data = va_arg(args, struct dma_heap_allocation_data*);
        data->fd = get_fd(fd, data->len);
        va_end(args);
        return 0;
    }

    void *data = va_arg(args, void*);
    int ret = real_ioctl(fd, request, data);
    va_end(args);
    return ret;
}

// DRM FD and ION FD and SHM FD functions
int drm_to_fd(int fd, size_t size) {
    struct drm_prime_handle prime = {
        .handle = xyz_to_handle(fd, size),
        .flags = DRM_RDWR | DRM_CLOEXEC
    };
    ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
    return prime.fd;
}

int ion_to_fd(int fd, size_t size) {
    struct ion_allocation_data alloc_data = {
        .len = size,
        .heap_id_mask = (1 << cma_heap_id),
        .flags = 0
    };
    ioctl(fd, ION_IOC_ALLOC, &alloc_data);

    struct ion_fd_data share_data = {
        .handle = alloc_data.handle,
    };
    ioctl(fd, ION_IOC_SHARE, &share_data);
    return share_data.fd;
}

int shm_to_fd(int fd, size_t size) {
    fd = memfd_create("shm_buf", 0);
    ftruncate(fd, size);
    return fd;
}

// GPU-based handle functions
__u32 msm_to_handle(int fd, size_t size) {
    struct drm_msm_gem_new gem = {
        .size = size,
        .flags = MSM_BO_WC
    };
    ioctl(fd, DRM_IOCTL_MSM_GEM_NEW, &gem);
    return gem.handle;
}

__u32 pnf_to_handle(int fd, size_t size) {
    struct drm_panfrost_create_bo gem = {
        .size = size,
        .flags = 0
    };
    ioctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &gem);
    return gem.handle;
}

int get_cma_heap_id(void) {
    int ion_fd = open(REAL_ION_DEVICE, O_RDWR, 0);
    struct ion_heap_query query;
    memset(&query, 0, sizeof(query));
    ioctl(ion_fd, ION_IOC_HEAP_QUERY, &query);
    struct ion_heap_data *heaps = calloc(query.cnt, sizeof(struct ion_heap_data));
    query.heaps = (intptr_t)heaps;
    ioctl(ion_fd, ION_IOC_HEAP_QUERY, &query);
    for (int i = 0; i < query.cnt; i++) {
        if (strstr(heaps[i].name, "cma")) {
            cma_heap_id = heaps[i].heap_id;
            break;
        }
    }
    free(heaps);
    close(ion_fd);
    return cma_heap_id;
}

__attribute__((constructor))
void detect_gpu_at_launch(void) {
    struct dirent *entry;
    const char *home_directory = getenv("HOME");
    DIR *dir = opendir(home_directory);
    char filepath[512];

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "device_info_", 12) == 0) {
            snprintf(filepath, sizeof(filepath), "%s/%s", home_directory, entry->d_name);
            FILE *file = fopen(filepath, "r");

            char cfwline[128];
            char devline[128];
            while (fgets(cfwline, sizeof(cfwline), file)) {
                if (strncmp(cfwline, "CFW_NAME=", 9) == 0) {
                    char *value = cfwline + 9;
                    value[strcspn(value, "\n")] = 0;

                    open64_new = open64_drm;
                    get_fd = drm_to_fd;
                    if (strcasestr(value, "rocknix") != NULL || strcasestr(value, "batocera") != NULL) {
                        while (fgets(devline, sizeof(devline), file)) {
                            if (strncmp(devline, "DEVICE_NAME=", 12) == 0) {
                                value = devline + 12;
                                value[strcspn(value, "\n")] = 0;
                                if (strcasestr(value, "retroid") != NULL || strcasestr(value, "odin") != NULL) {
                                    fprintf(stderr, "Set GPU functions to MSM (Adreno) compatibility mode.\n");
                                    xyz_to_handle = msm_to_handle;
                                } else {
                                    fprintf(stderr, "Set GPU functions to Panfrost (Mali) compatibility mode.\n");
                                    xyz_to_handle = pnf_to_handle;
                                }
                            }
                        }
                    } else if (strcasestr(value, "knulli") != NULL || strcasestr(value, "muos") != NULL) {
                        fprintf(stderr, "Set GPU functions to ION (Allwinner) compatibility mode.\n");
                        get_cma_heap_id();
                        open64_new = open64_ion;
                        get_fd = ion_to_fd;
                        xyz_to_handle = NULL;
                    } else {
                        fprintf(stderr, "Set GPU functions to SHM (fallback) compatibility mode.\n");
                        open64_new = open64_shm;
                        get_fd = shm_to_fd;
                        xyz_to_handle = NULL;
                    }

                    fclose(file);
                    closedir(dir);
                    return;
                }
            }
            fclose(file);
        }
    }
}
