CC := gcc

# 1：Hash 节点使用内存池
# 0：Hash 节点使用 malloc
HASH_USE_MEMORY_POOL ?= 1
# 1：红黑树节点使用内存池
# 0：红黑树节点使用 malloc
RBTREE_USE_MEMORY_POOL ?= 1
# 1：跳表普通节点使用内存池
# 0：跳表普通节点使用 malloc
SKIPLIST_USE_MEMORY_POOL ?= 1

SRC_DIR := src
ENGINE_DIR := $(SRC_DIR)/engines
NETWORK_DIR := $(SRC_DIR)/network
MEMORY_DIR := $(SRC_DIR)/memory
PERSISTENCE_DIR := $(SRC_DIR)/persistence

INCLUDE_DIR := include
TEST_DIR := tests

THIRD_PARTY_DIR := third_party
NTYCO_DIR := $(THIRD_PARTY_DIR)/NtyCo

BUILD_DIR := build
BIN_DIR := bin

CPPFLAGS := \
	-I$(INCLUDE_DIR) \
	-I$(NTYCO_DIR)/core \
	-DKVS_HASH_USE_MEMORY_POOL=$(HASH_USE_MEMORY_POOL) \
	-DKVS_RBTREE_USE_MEMORY_POOL=$(RBTREE_USE_MEMORY_POOL) \
	-DKVS_SKIPLIST_USE_MEMORY_POOL=$(SKIPLIST_USE_MEMORY_POOL)

CFLAGS := -std=gnu11 -Wall -Wextra -g
BENCH_CFLAGS := $(CFLAGS) -O2 -Werror

ARRAY_USE_JEMALLOC ?= 0
ARRAY_BENCH_DEFINE := $(if $(filter 1,$(ARRAY_USE_JEMALLOC)),-DKVS_BENCHMARK_JEMALLOC)
ARRAY_BENCH_LIB := $(if $(filter 1,$(ARRAY_USE_JEMALLOC)),-ljemalloc)


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
	$(BUILD_DIR)/kvs_skiplist.o \
	$(BUILD_DIR)/aof.o \
	$(BUILD_DIR)/snapshot.o \
	$(BUILD_DIR)/memory_pool.o

.PHONY: \
	all \
	testcase \
	test-memory-pool \
	test-hash-memory-pool \
	benchmark-hash \
	benchmark-hash-memory \
	test-rbtree-memory-pool \
	benchmark-rbtree \
	benchmark-rbtree-memory \
	test-skiplist-memory-pool \
	benchmark-skiplist \
	benchmark-skiplist-memory \
	test-array-memory \
	benchmark-array \
	benchmark-array-memory \
	clean

all: $(BIN_DIR)/kvstore

testcase: $(BIN_DIR)/testcase

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

$(NTYCO_DIR)/libntyco.a:
	$(MAKE) -C $(NTYCO_DIR)

$(BIN_DIR)/kvstore: $(OBJS) $(NTYCO_DIR)/libntyco.a | $(BIN_DIR)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/kvstore.o: \
	$(SRC_DIR)/kvstore.c \
	$(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/reactor.o: \
	$(NETWORK_DIR)/reactor.c \
	$(INCLUDE_DIR)/server.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/proactor.o: \
	$(NETWORK_DIR)/proactor.c \
	$(INCLUDE_DIR)/server.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ntyco.o: \
	$(NETWORK_DIR)/ntyco.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_array.o: \
	$(ENGINE_DIR)/kvs_array.c \
	$(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_rbtree.o: \
	$(ENGINE_DIR)/kvs_rbtree.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/memory_pool.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_hash.o: \
	$(ENGINE_DIR)/kvs_hash.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/memory_pool.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_skiplist.o: \
	$(ENGINE_DIR)/kvs_skiplist.c \
	$(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/aof.o: \
	$(PERSISTENCE_DIR)/aof.c \
	$(INCLUDE_DIR)/persistence.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/snapshot.o: \
	$(PERSISTENCE_DIR)/snapshot.c \
	$(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory_pool.o: \
	$(MEMORY_DIR)/memory_pool.c \
	$(INCLUDE_DIR)/memory_pool.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/testcase: \
	$(TEST_DIR)/testcase.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

test-memory-pool: $(BIN_DIR)/test_memory_pool
	./$(BIN_DIR)/test_memory_pool

$(BIN_DIR)/test_memory_pool: \
	$(TEST_DIR)/test_memory_pool.c \
	$(MEMORY_DIR)/memory_pool.c \
	$(INCLUDE_DIR)/memory_pool.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_memory_pool.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $@

test-hash-memory-pool: $(BIN_DIR)/test_hash_memory_pool
	./$(BIN_DIR)/test_hash_memory_pool

$(BIN_DIR)/test_hash_memory_pool: \
	$(TEST_DIR)/test_hash_memory_pool.c \
	$(ENGINE_DIR)/kvs_hash.c \
	$(MEMORY_DIR)/memory_pool.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/memory_pool.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_hash_memory_pool.c \
		$(ENGINE_DIR)/kvs_hash.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $@

# 编译 Hash 分配速度测试，不自动运行。
#
# 使用方法：
# make HASH_USE_MEMORY_POOL=1 benchmark-hash
# ./bin/benchmark_hash_allocator
#
# make HASH_USE_MEMORY_POOL=0 benchmark-hash
# ./bin/benchmark_hash_allocator
benchmark-hash: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(TEST_DIR)/benchmark_hash_allocator.c \
		$(ENGINE_DIR)/kvs_hash.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $(BIN_DIR)/benchmark_hash_allocator

# 编译 Hash 内存占用测试，不自动运行。
#
# 使用方法：
# make HASH_USE_MEMORY_POOL=1 benchmark-hash-memory
# ./bin/benchmark_hash_memory
#
# make HASH_USE_MEMORY_POOL=0 benchmark-hash-memory
# ./bin/benchmark_hash_memory
benchmark-hash-memory: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(TEST_DIR)/benchmark_hash_memory.c \
		$(ENGINE_DIR)/kvs_hash.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $(BIN_DIR)/benchmark_hash_memory

test-rbtree-memory-pool: $(BIN_DIR)/test_rbtree_memory_pool
	./$(BIN_DIR)/test_rbtree_memory_pool

$(BIN_DIR)/test_rbtree_memory_pool: \
	$(TEST_DIR)/test_rbtree_memory_pool.c \
	$(ENGINE_DIR)/kvs_rbtree.c \
	$(MEMORY_DIR)/memory_pool.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/memory_pool.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_rbtree_memory_pool.c \
		$(ENGINE_DIR)/kvs_rbtree.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $@
# 编译红黑树节点分配速度测试，不自动运行。
#
# 使用方法：
# make RBTREE_USE_MEMORY_POOL=1 benchmark-rbtree
# ./bin/benchmark_rbtree_allocator
#
# make RBTREE_USE_MEMORY_POOL=0 benchmark-rbtree
# ./bin/benchmark_rbtree_allocator
benchmark-rbtree: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(TEST_DIR)/benchmark_rbtree_allocator.c \
		$(ENGINE_DIR)/kvs_rbtree.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $(BIN_DIR)/benchmark_rbtree_allocator

# 编译红黑树内存占用测试，不自动运行。
#
# 使用方法：
# make RBTREE_USE_MEMORY_POOL=1 benchmark-rbtree-memory
# ./bin/benchmark_rbtree_memory
#
# make RBTREE_USE_MEMORY_POOL=0 benchmark-rbtree-memory
# ./bin/benchmark_rbtree_memory
benchmark-rbtree-memory: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(TEST_DIR)/benchmark_rbtree_memory.c \
		$(ENGINE_DIR)/kvs_rbtree.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $(BIN_DIR)/benchmark_rbtree_memory

test-skiplist-memory-pool: $(BIN_DIR)/test_skiplist_memory_pool
	./$(BIN_DIR)/test_skiplist_memory_pool

$(BIN_DIR)/test_skiplist_memory_pool: \
	$(TEST_DIR)/test_skiplist_memory_pool.c \
	$(ENGINE_DIR)/kvs_skiplist.c \
	$(MEMORY_DIR)/memory_pool.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/memory_pool.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_skiplist_memory_pool.c \
		$(ENGINE_DIR)/kvs_skiplist.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $@

# 编译跳表节点分配速度测试，不自动运行。
#
# 使用方法：
# make SKIPLIST_USE_MEMORY_POOL=1 benchmark-skiplist
# ./bin/benchmark_skiplist_allocator
#
# make SKIPLIST_USE_MEMORY_POOL=0 benchmark-skiplist
# ./bin/benchmark_skiplist_allocator
benchmark-skiplist: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(TEST_DIR)/benchmark_skiplist_allocator.c \
		$(ENGINE_DIR)/kvs_skiplist.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $(BIN_DIR)/benchmark_skiplist_allocator

# 编译跳表内存占用测试，不自动运行。
#
# 使用方法：
# make SKIPLIST_USE_MEMORY_POOL=1 benchmark-skiplist-memory
# ./bin/benchmark_skiplist_memory
#
# make SKIPLIST_USE_MEMORY_POOL=0 benchmark-skiplist-memory
# ./bin/benchmark_skiplist_memory
benchmark-skiplist-memory: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(TEST_DIR)/benchmark_skiplist_memory.c \
		$(ENGINE_DIR)/kvs_skiplist.c \
		$(MEMORY_DIR)/memory_pool.c \
		-o $(BIN_DIR)/benchmark_skiplist_memory

test-array-memory: $(BIN_DIR)/test_array_memory
	./$(BIN_DIR)/test_array_memory

$(BIN_DIR)/test_array_memory: \
	$(TEST_DIR)/test_array_memory.c \
	$(ENGINE_DIR)/kvs_array.c \
	$(INCLUDE_DIR)/kvstore.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_array_memory.c \
		$(ENGINE_DIR)/kvs_array.c \
		-o $@

benchmark-array: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(ARRAY_BENCH_DEFINE) \
		$(TEST_DIR)/benchmark_array_allocator.c \
		$(ENGINE_DIR)/kvs_array.c \
		$(ARRAY_BENCH_LIB) \
		-o $(BIN_DIR)/benchmark_array_allocator

benchmark-array-memory: | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) \
		$(ARRAY_BENCH_DEFINE) \
		$(TEST_DIR)/benchmark_array_memory.c \
		$(ENGINE_DIR)/kvs_array.c \
		$(ARRAY_BENCH_LIB) \
		-o $(BIN_DIR)/benchmark_array_memory

clean:
	$(RM) -r $(BUILD_DIR) $(BIN_DIR)