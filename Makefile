CC = cc
CLANG_FORMAT ?= clang-format
PYTHON ?= python3
BUILD_DIR ?= build
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
REQUIRED_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic
CFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS += -pthread
TEST_ARGS ?=

SOURCES = $(wildcard src/*.c)
LIB_SOURCES = $(filter-out src/main.c,$(SOURCES))
TEST_SOURCES = $(wildcard tests/test_*.c)
SERVER_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))
TEST_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(TEST_SOURCES) $(LIB_SOURCES))
FORMAT_FILES = $(sort $(wildcard include/*.h src/*.c tests/*.c tests/*.h))

.PHONY: all test smoke sanitize-test format-check format clean
all: $(BUILD_DIR)/mini-webserver

$(BUILD_DIR)/mini-webserver: $(SERVER_OBJECTS)
	$(CC) $(REQUIRED_CFLAGS) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/test-unit: $(TEST_OBJECTS)
	$(CC) $(REQUIRED_CFLAGS) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(REQUIRED_CFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test: $(BUILD_DIR)/test-unit $(BUILD_DIR)/mini-webserver
	$(BUILD_DIR)/test-unit $(TEST_ARGS)
	$(PYTHON) tests/integration.py --binary $(BUILD_DIR)/mini-webserver

smoke: $(BUILD_DIR)/mini-webserver
	bash tests/smoke.sh $(BUILD_DIR)/mini-webserver

sanitize-test:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/sanitize \
		CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' \
		LDFLAGS='-fsanitize=address,undefined' \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 test

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

clean:
	rm -f $(SERVER_OBJECTS) $(TEST_OBJECTS) \
		$(SERVER_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) \
		$(BUILD_DIR)/mini-webserver $(BUILD_DIR)/test-unit
	rm -f $(BUILD_DIR)/sanitize/src/*.o $(BUILD_DIR)/sanitize/src/*.d \
		$(BUILD_DIR)/sanitize/tests/*.o $(BUILD_DIR)/sanitize/tests/*.d \
		$(BUILD_DIR)/sanitize/mini-webserver $(BUILD_DIR)/sanitize/test-unit

-include $(SERVER_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
