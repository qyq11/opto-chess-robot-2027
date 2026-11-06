CC = gcc
BUILD_DIR ?= ../../../temp/motor-uart-build
CFLAGS += -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror

.PHONY: all
all: $(BUILD_DIR)/motor-query

$(BUILD_DIR)/motor-query: motor_query.c Makefile
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) motor_query.c -o $@ $(LDFLAGS) -lm
