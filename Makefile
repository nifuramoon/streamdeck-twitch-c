CC       ?= gcc
CFLAGS   ?= -O3 -march=native -Wall -Wextra -Wpedantic
CFLAGS   += -std=c2x -D_GNU_SOURCE -fstack-protector-strong $(EXTRA_CFLAGS)
LDFLAGS  := $(shell pkg-config --cflags --libs libcurl hidapi-libusb) -lm $(EXTRA_LDFLAGS)

SRC_DIR := src
OBJ_DIR := obj

STB_URL := https://raw.githubusercontent.com/nothings/stb/master
STB_DEPS := $(SRC_DIR)/stb_image.h $(SRC_DIR)/stb_image_write.h $(SRC_DIR)/stb_truetype.h

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
TARGET := streamdeck-twitch

.PHONY: all clean run watchdog

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

run: $(TARGET)
	./$(TARGET)

watchdog: $(TARGET)
	@while true; do \
		echo "[WATCHDOG] Starting streamdeck-twitch..."; \
		./$(TARGET) 2>&1; \
		EXIT_CODE=$$?; \
		echo "[WATCHDOG] Process exited with code $$EXIT_CODE, restarting in 2s..."; \
		sleep 2; \
	done

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
