CC := gcc



SRC_DIR := src
ENGINE_DIR := $(SRC_DIR)/engines
NETWORK_DIR := $(SRC_DIR)/network
MEMORY_DIR := $(SRC_DIR)/memory
PERSISTENCE_DIR := $(SRC_DIR)/persistence
REPLICATION_DIR := $(SRC_DIR)/replication
INCLUDE_DIR := include
TEST_DIR := tests

THIRD_PARTY_DIR := third_party
NTYCO_DIR := $(THIRD_PARTY_DIR)/NtyCo

BUILD_DIR := build
BIN_DIR := bin

CPPFLAGS := \
	-I$(INCLUDE_DIR) \
	-I$(NTYCO_DIR)/core

CFLAGS := -std=gnu11 -Wall -Wextra -g

LDFLAGS := -L$(NTYCO_DIR)
LDLIBS := -lntyco -lpthread -ldl

OBJS := \
	$(BUILD_DIR)/kvstore.o \
	$(BUILD_DIR)/config.o \
	$(BUILD_DIR)/log.o \
	$(BUILD_DIR)/codec.o \
	$(BUILD_DIR)/ntyco.o \
	$(BUILD_DIR)/kvs_array.o \
	$(BUILD_DIR)/kvs_rbtree.o \
	$(BUILD_DIR)/kvs_hash.o \
	$(BUILD_DIR)/kvs_skiplist.o \
	$(BUILD_DIR)/aof.o \
	$(BUILD_DIR)/snapshot.o \
	$(BUILD_DIR)/snapshot_store.o \
	$(BUILD_DIR)/mapped_file.o \
	$(BUILD_DIR)/uring_io.o \
	$(BUILD_DIR)/replication.o \
	$(BUILD_DIR)/kvs_allocator.o \
	$(BUILD_DIR)/buffer.o


.PHONY: \
	all \
	testcase \
	test-hash-memory-pool \
	test-rbtree-memory-pool \
	test-skiplist-memory-pool \
	test-array-memory \
	clean

all: $(BIN_DIR)/kvstore

testcase: $(BIN_DIR)/testcase

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

$(NTYCO_DIR)/libntyco.a:
	$(MAKE) -C $(NTYCO_DIR)

$(BIN_DIR)/kvstore: $(OBJS) $(NTYCO_DIR)/libntyco.a | $(BIN_DIR)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -luring -o $@

$(BUILD_DIR)/kvstore.o: \
	$(SRC_DIR)/kvstore.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/replication.h \
	$(INCLUDE_DIR)/protocol.h \
	$(INCLUDE_DIR)/config.h \
	$(INCLUDE_DIR)/server.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/config.o: \
        $(SRC_DIR)/config/config.c \
        $(INCLUDE_DIR)/config.h \
        $(INCLUDE_DIR)/role.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/log.o: \
        $(SRC_DIR)/log/log.c \
        $(INCLUDE_DIR)/log.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/codec.o: \
	$(SRC_DIR)/protocol/codec.c \
	$(INCLUDE_DIR)/protocol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ntyco.o: \
	$(NETWORK_DIR)/ntyco.c \
	$(INCLUDE_DIR)/server.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_array.o: \
	$(ENGINE_DIR)/kvs_array.c \
	$(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_rbtree.o: \
	$(ENGINE_DIR)/kvs_rbtree.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_hash.o: \
	$(ENGINE_DIR)/kvs_hash.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kvs_skiplist.o: \
	$(ENGINE_DIR)/kvs_skiplist.c \
	$(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/aof.o: \
    $(PERSISTENCE_DIR)/aof.c \
    $(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/kvs_allocator.h \
    $(INCLUDE_DIR)/mapped_file.h \
    $(INCLUDE_DIR)/uring_io.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/snapshot.o: \
	$(PERSISTENCE_DIR)/snapshot.c \
	$(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/kvstore.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/snapshot_store.o: \
	$(PERSISTENCE_DIR)/snapshot_store.c \
	$(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/mapped_file.h \
	$(INCLUDE_DIR)/kvs_allocator.h \
    $(INCLUDE_DIR)/uring_io.h \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/protocol.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mapped_file.o: \
	$(PERSISTENCE_DIR)/mapped_file.c \
	$(INCLUDE_DIR)/mapped_file.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/uring_io.o: \
    $(PERSISTENCE_DIR)/uring_io.c \
    $(INCLUDE_DIR)/uring_io.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/replication.o: \
	$(REPLICATION_DIR)/replication.c \
	$(INCLUDE_DIR)/replication.h \
	$(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/server.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/buffer.o: \
	$(NETWORK_DIR)/buffer.c \
	$(INCLUDE_DIR)/server.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/testcase: \
	$(TEST_DIR)/testcase.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

test-protocol: $(BIN_DIR)/test_protocol
	./$(BIN_DIR)/test_protocol

$(BIN_DIR)/test_protocol: \
        $(TEST_DIR)/test_protocol.c \
        $(SRC_DIR)/protocol/codec.c \
        $(INCLUDE_DIR)/protocol.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_protocol.c \
		$(SRC_DIR)/protocol/codec.c \
		-o $@

test-hash-memory-pool: $(BIN_DIR)/test_hash_memory_pool
	./$(BIN_DIR)/test_hash_memory_pool

$(BIN_DIR)/test_hash_memory_pool: \
	$(TEST_DIR)/test_hash_memory_pool.c \
	$(ENGINE_DIR)/kvs_hash.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_hash_memory_pool.c \
		$(ENGINE_DIR)/kvs_hash.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@


test-rbtree-memory-pool: $(BIN_DIR)/test_rbtree_memory_pool
	./$(BIN_DIR)/test_rbtree_memory_pool

$(BIN_DIR)/test_rbtree_memory_pool: \
	$(TEST_DIR)/test_rbtree_memory_pool.c \
	$(ENGINE_DIR)/kvs_rbtree.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvs_allocator.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_rbtree_memory_pool.c \
		$(ENGINE_DIR)/kvs_rbtree.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@

test-skiplist-memory-pool: $(BIN_DIR)/test_skiplist_memory_pool
	./$(BIN_DIR)/test_skiplist_memory_pool

$(BIN_DIR)/test_skiplist_memory_pool: \
	$(TEST_DIR)/test_skiplist_memory_pool.c \
	$(ENGINE_DIR)/kvs_skiplist.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/kvs_allocator.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_skiplist_memory_pool.c \
		$(ENGINE_DIR)/kvs_skiplist.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@

test-array-memory: $(BIN_DIR)/test_array_memory
	./$(BIN_DIR)/test_array_memory

$(BIN_DIR)/test_array_memory: \
	$(TEST_DIR)/test_array_memory.c \
	$(ENGINE_DIR)/kvs_array.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_array_memory.c \
		$(ENGINE_DIR)/kvs_array.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@

clean:
	$(RM) -r $(BUILD_DIR) $(BIN_DIR)

UNIT_TEST_TARGETS := \
    test-uring-io \
	test-protocol \
	test-config \
	test-snapshot \
	test-hash-memory-pool \
	test-rbtree-memory-pool \
	test-skiplist-memory-pool \
	test-mapped-file \
	test-aof-replay \
	test-connection-protocol \
	test-replication-boundary \
	test-array-memory \
    test-allocator \
    test-buffer-allocator
.PHONY: test-unit test-integration test

test-unit:
	@count=0; \
	for target in $(UNIT_TEST_TARGETS); do \
		if output=$$($(MAKE) --no-print-directory "$$target" 2>&1); then \
			printf '[PASS] %s\n' "$$target"; \
			count=$$((count + 1)); \
		else \
			status=$$?; \
			printf '[FAIL] %s (exit %s)\n%s\n' "$$target" "$$status" "$$output" >&2; \
			exit "$$status"; \
		fi; \
	done; \
	printf 'Unit tests: %s passed\n' "$$count"

test-integration: $(BIN_DIR)/kvstore
	@if output=$$(python3 -B $(TEST_DIR)/test_standalone.py 2>&1); then \
		printf '[PASS] standalone integration\n'; \
	else \
		status=$$?; \
		printf '[FAIL] standalone integration (exit %s)\n%s\n' "$$status" "$$output" >&2; \
		exit "$$status"; \
	fi
	@if output=$$(bash $(TEST_DIR)/test_replication.sh 2>&1); then \
		printf '[PASS] replication integration\n'; \
	else \
		status=$$?; \
		printf '[FAIL] replication integration (exit %s)\n%s\n' "$$status" "$$output" >&2; \
		exit "$$status"; \
	fi
	@printf 'Integration test groups: 5 passed\n'

test:
	@$(MAKE) --no-print-directory test-unit
	@$(MAKE) --no-print-directory test-integration

.PHONY: test-config
test-config: $(BIN_DIR)/test_config
	./$(BIN_DIR)/test_config

$(BIN_DIR)/test_config: \
        $(TEST_DIR)/test_config.c \
        $(SRC_DIR)/config/config.c \
        $(INCLUDE_DIR)/config.h \
        $(INCLUDE_DIR)/role.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
                $(TEST_DIR)/test_config.c \
                $(SRC_DIR)/config/config.c \
                -o $@

.PHONY: test-snapshot

test-snapshot: $(BIN_DIR)/test_snapshot_store
	./$(BIN_DIR)/test_snapshot_store

$(BIN_DIR)/test_snapshot_store: \
	$(TEST_DIR)/test_snapshot_store.c \
	$(PERSISTENCE_DIR)/snapshot_store.c \
	$(PERSISTENCE_DIR)/mapped_file.c \
	$(PERSISTENCE_DIR)/uring_io.c \
	$(ENGINE_DIR)/kvs_array.c \
	$(ENGINE_DIR)/kvs_rbtree.c \
	$(ENGINE_DIR)/kvs_hash.c \
	$(ENGINE_DIR)/kvs_skiplist.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvs_allocator.h \
	$(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/mapped_file.h \
	$(INCLUDE_DIR)/uring_io.h \
	$(INCLUDE_DIR)/kvstore.h \
	$(INCLUDE_DIR)/protocol.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_snapshot_store.c \
		$(PERSISTENCE_DIR)/snapshot_store.c \
		$(PERSISTENCE_DIR)/mapped_file.c \
		$(PERSISTENCE_DIR)/uring_io.c \
		$(ENGINE_DIR)/kvs_array.c \
		$(ENGINE_DIR)/kvs_rbtree.c \
		$(ENGINE_DIR)/kvs_hash.c \
		$(ENGINE_DIR)/kvs_skiplist.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@ -luring
.PHONY: test-snapshot-restart

test-snapshot-restart: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_snapshot_restart.py

test-integration: test-snapshot-restart

.PHONY: test-aof-restart

test-aof-restart: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_aof_restart.py

test-integration: test-aof-restart

.PHONY: test-mapped-file

test-mapped-file: $(BIN_DIR)/test_mapped_file
	./$(BIN_DIR)/test_mapped_file

$(BIN_DIR)/test_mapped_file: \
	$(TEST_DIR)/test_mapped_file.c \
	$(PERSISTENCE_DIR)/mapped_file.c \
	$(INCLUDE_DIR)/mapped_file.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_mapped_file.c \
		$(PERSISTENCE_DIR)/mapped_file.c \
		-o $@

.PHONY: test-aof-replay

test-aof-replay: $(BIN_DIR)/test_aof_replay
	./$(BIN_DIR)/test_aof_replay

$(BIN_DIR)/test_aof_replay: \
	$(TEST_DIR)/test_aof_replay.c \
	$(PERSISTENCE_DIR)/aof.c \
	$(PERSISTENCE_DIR)/mapped_file.c \
	$(PERSISTENCE_DIR)/uring_io.c \
	$(SRC_DIR)/protocol/codec.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvs_allocator.h \
	$(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/mapped_file.h \
	$(INCLUDE_DIR)/uring_io.h \
	$(INCLUDE_DIR)/protocol.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_aof_replay.c \
		$(PERSISTENCE_DIR)/aof.c \
		$(PERSISTENCE_DIR)/mapped_file.c \
		$(PERSISTENCE_DIR)/uring_io.c \
		$(SRC_DIR)/protocol/codec.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@ -luring

.PHONY: test-uring-io

test-uring-io: $(BIN_DIR)/test_uring_io
	./$(BIN_DIR)/test_uring_io

$(BIN_DIR)/test_uring_io: \
    $(TEST_DIR)/test_uring_io.c \
    $(PERSISTENCE_DIR)/uring_io.c \
    $(INCLUDE_DIR)/uring_io.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
	    $(TEST_DIR)/test_uring_io.c \
	    $(PERSISTENCE_DIR)/uring_io.c \
	    -o $@ -luring

.PHONY: test-connection-protocol

test-connection-protocol: $(BIN_DIR)/test_connection_protocol
	./$(BIN_DIR)/test_connection_protocol

$(BIN_DIR)/test_connection_protocol: \
	$(TEST_DIR)/test_connection_protocol.c \
	$(SRC_DIR)/protocol/codec.c \
	$(INCLUDE_DIR)/protocol.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_connection_protocol.c \
		$(SRC_DIR)/protocol/codec.c \
		-o $@

.PHONY: test-single-port

test-single-port: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/verify_single_port.py .

test-integration: test-single-port

.PHONY: test-replication-boundary

test-replication-boundary: $(BIN_DIR)/test_replication_boundary
	./$(BIN_DIR)/test_replication_boundary

$(BIN_DIR)/test_replication_boundary: \
	$(TEST_DIR)/test_replication_boundary.c \
	$(filter-out $(BUILD_DIR)/kvstore.o $(BUILD_DIR)/ntyco.o,$(OBJS)) \
	$(wildcard $(INCLUDE_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_replication_boundary.c \
		$(filter-out $(BUILD_DIR)/kvstore.o $(BUILD_DIR)/ntyco.o,$(OBJS)) \
		-luring -lpthread -ldl -o $@
include tests/demo.mk

.PHONY: test-allocator

test-allocator: $(BIN_DIR)/test_allocator
	./$(BIN_DIR)/test_allocator

$(BIN_DIR)/test_allocator: \
	$(TEST_DIR)/test_allocator.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_allocator.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@

$(BUILD_DIR)/kvs_allocator.o: \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

.PHONY: test-buffer-allocator

test-buffer-allocator: $(BIN_DIR)/test_buffer_allocator
	./$(BIN_DIR)/test_buffer_allocator

$(BIN_DIR)/test_buffer_allocator: \
	$(TEST_DIR)/test_buffer_allocator.c \
	$(NETWORK_DIR)/buffer.c \
	$(MEMORY_DIR)/kvs_allocator.c \
	$(INCLUDE_DIR)/server.h \
	$(INCLUDE_DIR)/kvs_allocator.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		-fsanitize=address,undefined \
		-fno-omit-frame-pointer \
		$(TEST_DIR)/test_buffer_allocator.c \
		$(NETWORK_DIR)/buffer.c \
		$(MEMORY_DIR)/kvs_allocator.c \
		-o $@