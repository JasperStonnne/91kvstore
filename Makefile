CC := gcc

SRC_DIR := src
ENGINE_DIR := $(SRC_DIR)/engines
NETWORK_DIR := $(SRC_DIR)/network
MEMORY_DIR := $(SRC_DIR)/memory
INCLUDE_DIR := include
TEST_DIR := tests

THIRD_PARTY_DIR := third_party
NTYCO_DIR := $(THIRD_PARTY_DIR)/NtyCo

BUILD_DIR := build
BIN_DIR := bin

PERSISTENCE_DIR := $(SRC_DIR)/persistence

CPPFLAGS := -I$(INCLUDE_DIR) -I$(NTYCO_DIR)/core
HASH_USE_MEMORY_POOL ?= 1
CPPFLAGS += -DKVS_HASH_USE_MEMORY_POOL=$(HASH_USE_MEMORY_POOL)
CFLAGS := -std=gnu11 -Wall -Wextra -g
LDFLAGS := -L$(NTYCO_DIR)
LDLIBS := -luring -lntyco -lpthread -ldl

OBJS := \
	$(BUILD_DIR)/kvstore.o \
	$(BUILD_DIR)/reactor.o \
	$(BUILD_DIR)/proactor.o \
	$(BUILD_DIR)/ntyco.o \
	$(BUILD_DIR)/kvs_array.o \
	$(BUILD_DIR)/kvs_rbtree.o \
	$(BUILD_DIR)/kvs_hash.o \
	$(BUILD_DIR)/kvs_skiplist.o\
	$(BUILD_DIR)/aof.o\
	$(BUILD_DIR)/snapshot.o\
	$(BUILD_DIR)/memory_pool.o

.PHONY: all testcase test-memory-pool test-hash-memory-pool clean

all: $(BIN_DIR)/kvstore

testcase: $(BIN_DIR)/testcase

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

$(NTYCO_DIR)/libntyco.a:
	$(MAKE) -C $(NTYCO_DIR)

$(BIN_DIR)/kvstore: $(OBJS) $(NTYCO_DIR)/libntyco.a | $(BIN_DIR)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/kvstore.o: $(SRC_DIR)/kvstore.c $(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/reactor.o: $(NETWORK_DIR)/reactor.c $(INCLUDE_DIR)/server.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/proactor.o: $(NETWORK_DIR)/proactor.c $(INCLUDE_DIR)/server.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ntyco.o: $(NETWORK_DIR)/ntyco.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_array.o: $(ENGINE_DIR)/kvs_array.c $(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_rbtree.o: $(ENGINE_DIR)/kvs_rbtree.c $(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_hash.o: $(ENGINE_DIR)/kvs_hash.c $(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/testcase: $(TEST_DIR)/testcase.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

$(BUILD_DIR)/kvs_skiplist.o: $(ENGINE_DIR)/kvs_skiplist.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/aof.o: $(PERSISTENCE_DIR)/aof.c $(INCLUDE_DIR)/persistence.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/snapshot.o: $(PERSISTENCE_DIR)/snapshot.c $(INCLUDE_DIR)/persistence.h $(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory_pool.o: $(MEMORY_DIR)/memory_pool.c $(INCLUDE_DIR)/memory_pool.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test-memory-pool: $(BIN_DIR)/test_memory_pool
	./$(BIN_DIR)/test_memory_pool

test-hash-memory-pool: $(BIN_DIR)/test_hash_memory_pool
	./$(BIN_DIR)/test_hash_memory_pool

$(BIN_DIR)/test_hash_memory_pool: \
	$(TEST_DIR)/test_hash_memory_pool.c \
	$(ENGINE_DIR)/kvs_hash.c \
	$(MEMORY_DIR)/memory_pool.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/memory_pool.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address -fno-omit-frame-pointer \
		$(TEST_DIR)/test_hash_memory_pool.c \
		$(ENGINE_DIR)/kvs_hash.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $@

$(BIN_DIR)/test_memory_pool: \
		$(TEST_DIR)/test_memory_pool.c \
		$(MEMORY_DIR)/memory_pool.c \
		$(INCLUDE_DIR)/memory_pool.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address -fno-omit-frame-pointer \
		$(TEST_DIR)/test_memory_pool.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $@

clean:
	$(RM) -r $(BUILD_DIR) $(BIN_DIR)

