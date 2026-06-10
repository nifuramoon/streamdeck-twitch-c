#ifndef DEVICE_H
#define DEVICE_H

#include "common.h"

typedef struct device device;

device *device_open(void);
void    device_close(device *dev);
int     device_clear_all(device *dev);
int     device_set_key(device *dev, int key_idx, const uint8_t *image_data,
                       size_t image_size);
int     device_set_brightness(device *dev, int brightness);
int     device_read_input(device *dev, uint8_t *buf, size_t buf_size);
void    device_reset_cache(device *dev);

#endif
