#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define STREAMDECK_VID 0x0fd9
#define STREAMDECK_PID_ORIG 0x0060
#define STREAMDECK_PID_MK2  0x0080
#define STREAMDECK_PID_V2   0x006d

#define KEY_COLS 5
#define KEY_ROWS 3
#define KEY_COUNT 15
#define KEY_WIDTH 72
#define KEY_HEIGHT 72

#define MAX_STR 1024
#define CONFIG_DIR "/.config/streamdeck-twitch/"
#define CONFIG_FILE "config.json"

#endif
