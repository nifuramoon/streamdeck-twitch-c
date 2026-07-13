#include "device.h"
#include "log.h"
#include <stdlib.h>
#include <tmmintrin.h>
#include <smmintrin.h>
#include <string.h>
#include <stdio.h>
#include <hidapi/hidapi.h>
#include <time.h>

#include "stb_image_write.h"

struct device {
    hid_device *handle;
    int pid;
    unsigned int jpg_crc[15];
};

#define REPORT_SIZE 1024
#define HEADER_SIZE 8
#define ITER_SIZE (REPORT_SIZE - HEADER_SIZE)


struct jpg_buf {
    unsigned char *data;
    unsigned long len;
};

static void jpg_write_fn(void *ctx, void *d, int sz) {
    struct jpg_buf *b = (struct jpg_buf *)ctx;
    b->data = realloc(b->data, b->len + (size_t)sz);
    memcpy(b->data + b->len, d, (size_t)sz);
    b->len += (size_t)sz;
}

static int je(const uint8_t *rgb, int w, int h, int q, uint8_t **out, unsigned long *out_len) {
    struct jpg_buf b = {0};
    int r = stbi_write_jpg_to_func(jpg_write_fn, &b, w, h, 3, rgb, q);
    if (!r) { free(b.data); return -1; }
    *out = b.data;
    *out_len = b.len;
    return 0;
}

device *device_open(void) {
    if (hid_init() < 0) { errorLog("hid_init failed"); return NULL; }

    int pids[] = {STREAMDECK_PID_V2, STREAMDECK_PID_ORIG, STREAMDECK_PID_MK2};
    hid_device *handle = NULL;
    int pid = 0;
    for (int i = 0; i < 3 && !handle; i++) {
        handle = hid_open(STREAMDECK_VID, pids[i], NULL);
        if (handle) pid = pids[i];
    }
    if (!handle) { errorLog("no Stream Deck"); hid_exit(); return NULL; }

    device *dev = calloc(1, sizeof(device)); dev->handle = handle; dev->pid = pid;
    memset(dev->jpg_crc, 0, sizeof(dev->jpg_crc));

    if (pid == STREAMDECK_PID_V2) {
        /* Reset via feature report */
        unsigned char rst[32] = {0x03, 0x02};
        hid_send_feature_report(handle, rst, 32);

        unsigned char b[32];
        hid_read_timeout(handle, b, 32, 100);

        /* Reset key stream */
        unsigned char ks[1024] = {0}; ks[0] = 0x02;
        hid_write(handle, ks, 1024);

        nanosleep(&(struct timespec){0, 50000000}, NULL);
        hid_read_timeout(handle, b, 32, 100);
    }

    infoLog("Stream Deck opened (PID 0x%04x)", pid);
    return dev;
}

void device_close(device *dev) {
    if (!dev) return;
    if (dev->handle) hid_close(dev->handle);
    free(dev); hid_exit();
}

void device_reset_cache(device *dev) {
    if (!dev) return;
    memset(dev->jpg_crc, 0, sizeof(dev->jpg_crc));
}

int device_clear_all(device *dev) {
    if (!dev) return -1;
    static const uint8_t black[72*72*4] = {0};
    for (int i = 0; i < 15; i++)
        device_set_key(dev, i, black, sizeof(black));
    device_reset_cache(dev);
    return 0;
}

int device_set_key(device *dev, int key_idx, const uint8_t *image_data, size_t image_size) {
    (void)image_size;
    if (!dev || !dev->handle || key_idx < 0 || key_idx >= 15) return -1;
    size_t px = (size_t)72*72;
    uint8_t rgb[72*72*3] __attribute__((aligned(16)));
    uint8_t flp[72*72*3] __attribute__((aligned(16)));
    /* x86-64 SSE2: RGBA->BGR 4 pixels at a time */
    static const unsigned char bgr_mask[16] __attribute__((aligned(16))) =
        {0x00,0x01,0x02, 0x04,0x05,0x06, 0x08,0x09,0x0a, 0x0c,0x0d,0x0e, 0xff,0xff,0xff,0xff};
    (void)bgr_mask;
    for (size_t i = 0; i < 72*72; i += 4) {
        __m128i v = _mm_loadu_si128((__m128i*)(image_data + i * 4));
        __m128i r = _mm_shuffle_epi8(v, _mm_set_epi8(
            -1,-1,-1,-1, 14,13,12, 10,9,8, 6,5,4, 2,1,0
        ));
        _mm_storeu_si128((__m128i*)(rgb + i * 3), r);
    }
    /* flip: horizontal + vertical mirror */
    for (int y = 0; y < 72; y++) {
        uint8_t *src_row = rgb + y * 216;
        uint8_t *dst_row = flp + (71 - y) * 216;
        for (int x = 0; x < 72; x++) {
            int si = x * 3;
            int di = (71 - x) * 3;
            dst_row[di] = src_row[si];
            dst_row[di+1] = src_row[si+1];
            dst_row[di+2] = src_row[si+2];
        }
    }
    uint8_t *jp=NULL; unsigned long jl=0;
    je(flp,72,72,100,&jp,&jl);

    /* CRC32 of JPEG data (hardware CRC32 instruction) */
    unsigned int crc = 0xFFFFFFFF;
    unsigned long k = 0;
    /* Process 4 bytes at a time */
    for (; k + 4 <= jl; k += 4) {
        crc = _mm_crc32_u32(crc, *(unsigned int*)(jp + k));
    }
    for (; k < jl; k++) {
        crc = _mm_crc32_u8(crc, jp[k]);
    }
    crc ^= 0xFFFFFFFF;
    if (crc == dev->jpg_crc[key_idx]) { free(jp); return 0; }
    dev->jpg_crc[key_idx] = crc;
    int pn=0, rem=(int)jl;
    while (rem > 0) {
        int tl = rem > ITER_SIZE ? ITER_SIZE : rem;
        int bs = pn * ITER_SIZE;
        unsigned char pkt[REPORT_SIZE]; memset(pkt,0,REPORT_SIZE);
        pkt[0]=0x02; pkt[1]=0x07; pkt[2]=(unsigned char)key_idx;
        pkt[3]=(tl==rem)?1:0; pkt[4]=(unsigned char)(tl&0xFF); pkt[5]=(unsigned char)((tl>>8)&0xFF);
        pkt[6]=(unsigned char)(pn&0xFF); pkt[7]=(unsigned char)((pn>>8)&0xFF);
        memcpy(pkt+8, jp+bs, (size_t)tl);
        if (hid_write(dev->handle, pkt, REPORT_SIZE) < 0) { errorLog("hid_write fail k%d p%d",key_idx,pn); free(jp); return -1; }
        rem -= tl; pn++;
    }
    free(jp); return 0;
}

int device_set_brightness(device *dev, int brightness) {
    if (!dev || !dev->handle) return -1;
    if (brightness<0) brightness=0; if (brightness>100) brightness=100;
    unsigned char pkt[32] = {0x03, 0x08, (unsigned char)brightness};
    if (hid_send_feature_report(dev->handle, pkt, 32) < 0) { errorLog("brightness failed"); return -1; }
    infoLog("brightness %d%%", brightness); return 0;
}

int device_read_input(device *dev, uint8_t *buf, size_t buf_size) {
    if (!dev || !dev->handle) return -1;
    int r = hid_read_timeout(dev->handle, buf, (int)buf_size, 50);
    if (r < 0) return -1;
    return r;
}
