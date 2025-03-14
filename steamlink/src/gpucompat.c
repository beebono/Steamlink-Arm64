#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/dma-heap.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

// Required fcntl.h defines
#define O_RDWR        00000002
#define O_CLOEXEC     02000000
#define AT_FDCWD          -100

// Function stores
int (*real_ioctl)(int, unsigned long, ...);
int (*real_av_frame_ref)(AVFrame *, const AVFrame *);

// Actual open syscall
int real_open(const char *path, int flags, __u32 mode) {
    return syscall(SYS_openat, AT_FDCWD, path, flags, mode);
}

// Force success for DMA_HEAP access
int open64(const char *path, int flags, __u32 mode) {
    if (strcasestr(path, "/dev/dma_heap") != NULL) {
        return 0;
    }
    return real_open(path, flags, mode);
}

// Initial open hook
int open(const char *path, int flags, __u32 mode) {
    return open64(path, flags, mode);
}

// Software-backed "DMA-BUF" maker
int get_shm_fd(size_t size) {
    int fd = memfd_create("shm_buf", 0);
    ftruncate(fd, (off_t)size);
    return fd;
}

// Catch any calls that try to use DMA_HEAP and provide a usable DMA-BUF FD
int ioctl(int fd, unsigned long request, ...) {
    if (!real_ioctl) { real_ioctl = dlsym(RTLD_NEXT, "ioctl"); }

    va_list args;
    va_start(args, request);

    if (request == DMA_HEAP_IOCTL_ALLOC) {
        struct dma_heap_allocation_data *data = va_arg(args, struct dma_heap_allocation_data*);
        data->fd = get_shm_fd(data->len);
        va_end(args);
        return 0;
    }

    void *data = va_arg(args, void*);
    int ret = real_ioctl(fd, request, data);
    va_end(args);
    return ret;
}

// Convert any YUV420P frame to RGB24
int av_frame_ref(AVFrame *dst, const AVFrame *src) {
    if (!real_av_frame_ref) { real_av_frame_ref = dlsym(RTLD_NEXT, "av_frame_ref"); }

    if (src->format == AV_PIX_FMT_YUV420P) {
        AVFrame *rgb_src = av_frame_alloc();
        rgb_src->width = src->width;
        rgb_src->height = src->height;
        rgb_src->format = AV_PIX_FMT_RGB0;
        av_frame_get_buffer(rgb_src, 32);

        struct SwsContext *sws_ctx = sws_getContext(
            src->width, src->height, AV_PIX_FMT_YUV420P,
            src->width, src->height, AV_PIX_FMT_RGB0,
            SWS_FAST_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, NULL, NULL, NULL);
        sws_scale(sws_ctx, (const uint8_t* const*)src->data, src->linesize, 0, src->height,
                           rgb_src->data, rgb_src->linesize);

        return real_av_frame_ref(dst, rgb_src);
    }
    return real_av_frame_ref(dst, src);
}
