CC       ?= gcc
CFLAGS   ?= -O3 -march=native -flto -fomit-frame-pointer
CFLAGS   += -std=gnu2x -D_GNU_SOURCE -DNDEBUG
CFLAGS   += -ffunction-sections -fdata-sections
CFLAGS   += -Wall -Wextra -Werror=implicit-function-declaration
CFLAGS   += -Isrc $(shell pkg-config --cflags libcurl hidapi-libusb) $(EXTRA_CFLAGS)
CFLAGS   += -fstack-protector-strong

LDFLAGS  += -flto -Wl,--gc-sections $(EXTRA_LDFLAGS)
LDLIBS   += $(shell pkg-config --libs libcurl hidapi-libusb) -lpthread -lm

ifeq ($(DEBUG),1)
    CFLAGS := $(filter-out -O3 -flto -fomit-frame-pointer -DNDEBUG,$(CFLAGS)) -O0 -g -DDEBUG
    LDFLAGS := $(filter-out -flto -Wl,--gc-sections,$(LDFLAGS))
endif

SRC_DIR := src
OBJ_DIR := obj

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TARGET := streamdeck-twitch

.PHONY: all clean run watchdog

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

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

-include $(DEPS)
