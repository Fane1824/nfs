CC = gcc
CFLAGS = -Wall -Wextra -pthread -g

# Directories
SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin

# Components
COMMON_DIR = $(SRC_DIR)/common
NS_DIR = $(SRC_DIR)/naming_server
SS_DIR = $(SRC_DIR)/storage_server
CLIENT_DIR = $(SRC_DIR)/client

# Include paths
COMMON_INCLUDES = -I$(COMMON_DIR)/include
NS_INCLUDES = -I$(NS_DIR)/include
SS_INCLUDES = -I$(SS_DIR)/include
CLIENT_INCLUDES = -I$(CLIENT_DIR)/include

# Source files
COMMON_SRC = $(wildcard $(COMMON_DIR)/src/*.c)
NS_SRC = $(wildcard $(NS_DIR)/src/*.c)
SS_SRC = $(wildcard $(SS_DIR)/src/*.c)
CLIENT_SRC = $(wildcard $(CLIENT_DIR)/src/*.c)

# Object files
COMMON_OBJ = $(patsubst $(COMMON_DIR)/src/%.c,$(BUILD_DIR)/common/%.o,$(COMMON_SRC))
NS_OBJ = $(patsubst $(NS_DIR)/src/%.c,$(BUILD_DIR)/naming_server/%.o,$(NS_SRC))
SS_OBJ = $(patsubst $(SS_DIR)/src/%.c,$(BUILD_DIR)/storage_server/%.o,$(SS_SRC))
CLIENT_OBJ = $(patsubst $(CLIENT_DIR)/src/%.c,$(BUILD_DIR)/client/%.o,$(CLIENT_SRC))

# Binaries
NS_BIN = $(BIN_DIR)/naming_server
SS_BIN = $(BIN_DIR)/storage_server
CLIENT_BIN = $(BIN_DIR)/client

# Test directories
TEST_ROOT = test_root
SS1_DIR = $(TEST_ROOT)/ss1
SS2_DIR = $(TEST_ROOT)/ss2

.PHONY: all clean test test_dirs

all: $(NS_BIN) $(SS_BIN) $(CLIENT_BIN)

$(NS_BIN): $(COMMON_OBJ) $(NS_OBJ) | $(BIN_DIR)
	$(CC) $^ -o $@ $(CFLAGS) $(COMMON_INCLUDES) $(NS_INCLUDES)

$(SS_BIN): $(COMMON_OBJ) $(SS_OBJ) | $(BIN_DIR)
	$(CC) $^ -o $@ $(CFLAGS) $(COMMON_INCLUDES) $(SS_INCLUDES)

$(CLIENT_BIN): $(COMMON_OBJ) $(CLIENT_OBJ) | $(BIN_DIR)
	$(CC) $^ -o $@ $(CFLAGS) $(COMMON_INCLUDES) $(CLIENT_INCLUDES)

# Object compilation rules
$(BUILD_DIR)/common/%.o: $(COMMON_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COMMON_INCLUDES) -c $< -o $@

$(BUILD_DIR)/naming_server/%.o: $(NS_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COMMON_INCLUDES) $(NS_INCLUDES) -c $< -o $@

$(BUILD_DIR)/storage_server/%.o: $(SS_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COMMON_INCLUDES) $(SS_INCLUDES) -c $< -o $@

$(BUILD_DIR)/client/%.o: $(CLIENT_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(COMMON_INCLUDES) $(CLIENT_INCLUDES) -c $< -o $@

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

test_dirs:
	mkdir -p $(SS1_DIR)
	mkdir -p $(SS2_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(TEST_ROOT) test_clean.sh test_start.sh

test_scripts: 
	@echo '#!/bin/bash' > test_start.sh
	@echo 'trap "kill 0" EXIT' >> test_start.sh
	@echo '$(NS_BIN) -p 9000 &' >> test_start.sh
	@echo 'sleep 1' >> test_start.sh
	@echo '$(SS_BIN) -p 9001 -n localhost -N 9000 -d $(SS1_DIR) &' >> test_start.sh
	@echo '$(SS_BIN) -p 9002 -n localhost -N 9000 -d $(SS2_DIR) -b localhost:9001 &' >> test_start.sh
	@echo 'sleep 1' >> test_start.sh
	@echo 'echo "Test environment ready!"' >> test_start.sh
	@echo 'wait' >> test_start.sh
	@chmod +x test_start.sh
	
	@echo '#!/bin/bash' > test_clean.sh
	@echo 'killall naming_server storage_server 2>/dev/null' >> test_clean.sh
	@echo 'rm -rf $(TEST_ROOT)' >> test_clean.sh
	@chmod +x test_clean.sh

test: all test_dirs test_scripts
	@echo "Starting test environment..."
	@echo "Use ./test_start.sh to start components"
	@echo "Use ./test_clean.sh to clean up"