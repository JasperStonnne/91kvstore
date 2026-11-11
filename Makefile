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

# NtyCo 的 read/write/connect hook 仅用于本程序，不能截获 librdmacm 的设备操作。
# ELF 下隐藏静态库的动态符号导出，防止终端设备写错误陷入协程等待。
LDFLAGS := -L$(NTYCO_DIR) -Wl,--exclude-libs,libntyco.a
RDMA_LIBS := -lrdmacm -libverbs
LDLIBS := -lntyco -lpthread -ldl $(RDMA_LIBS)

OBJS := \
	$(BUILD_DIR)/kvstore.o \
	$(BUILD_DIR)/config.o \
	$(BUILD_DIR)/log.o \
	$(BUILD_DIR)/codec.o \
	$(BUILD_DIR)/ntyco.o \
	$(BUILD_DIR)/rdma_transport.o \
	$(BUILD_DIR)/kvs_array.o \
	$(BUILD_DIR)/kvs_rbtree.o \
	$(BUILD_DIR)/kvs_hash.o \
	$(BUILD_DIR)/kvs_skiplist.o \
	$(BUILD_DIR)/aof.o \
	$(BUILD_DIR)/snapshot_io.o \
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

.PHONY: test-manual-testcase
test-manual-testcase: $(BIN_DIR)/kvstore $(BIN_DIR)/testcase
	python3 -u -B tests/test_manual_testcase.py

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
	$(INCLUDE_DIR)/rdma_transport.h \
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

$(BUILD_DIR)/rdma_transport.o: \
	$(NETWORK_DIR)/rdma_transport.c \
	$(INCLUDE_DIR)/rdma_transport.h | $(BUILD_DIR)
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

$(BUILD_DIR)/snapshot_io.o: \
	$(PERSISTENCE_DIR)/snapshot_io.c \
	$(INCLUDE_DIR)/persistence.h \
	$(INCLUDE_DIR)/protocol.h | $(BUILD_DIR)
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
	@$(MAKE) --no-print-directory test-hybrid-recovery
	@$(MAKE) --no-print-directory test-aof-write-failure
	@$(MAKE) --no-print-directory test-aof-sync-failure
	@$(MAKE) --no-print-directory test-snapshot-save-failure
	@$(MAKE) --no-print-directory test-snapshot-crash
	@$(MAKE) --no-print-directory test-replication-faults
	@printf 'Integration test groups: 11 passed\n'

.PHONY: test-hybrid-recovery
test-hybrid-recovery: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_hybrid_recovery.py

.PHONY: test-aof-write-failure
test-aof-write-failure: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_aof_write_failure.py

# 只有测试程序链接同步故障包装器；正式 kvstore 不包含该开关。
$(BIN_DIR)/test_kvstore_sync_fault: $(OBJS) $(TEST_DIR)/aof_sync_fault.c \
        $(INCLUDE_DIR)/uring_io.h $(NTYCO_DIR)/libntyco.a | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror $(TEST_DIR)/aof_sync_fault.c $(OBJS) \
	    $(LDFLAGS) -Wl,--wrap=kvs_uring_io_sync $(LDLIBS) -luring -o $@

.PHONY: test-aof-sync-failure
test-aof-sync-failure: $(BIN_DIR)/kvstore $(BIN_DIR)/test_kvstore_sync_fault
	python3 -B $(TEST_DIR)/test_aof_sync_failure.py

# 独立的 Snapshot 测试程序；正式程序不链接故障包装器。
$(BIN_DIR)/test_kvstore_snapshot_fault: $(OBJS) $(TEST_DIR)/snapshot_save_fault.c \
        $(INCLUDE_DIR)/uring_io.h $(NTYCO_DIR)/libntyco.a | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror $(TEST_DIR)/snapshot_save_fault.c $(OBJS) \
	    $(LDFLAGS) -Wl,--wrap=kvs_uring_io_write_batch -Wl,--wrap=kvs_uring_io_sync -Wl,--wrap=rename -Wl,--wrap=fsync $(LDLIBS) -luring -o $@

.PHONY: test-snapshot-save-failure
test-snapshot-save-failure: $(BIN_DIR)/kvstore $(BIN_DIR)/test_kvstore_snapshot_fault
	python3 -B $(TEST_DIR)/test_snapshot_save_failure.py --case file-sync
	python3 -B $(TEST_DIR)/test_snapshot_save_failure.py --case rename
	python3 -B $(TEST_DIR)/test_snapshot_save_failure.py --case dir-sync

.PHONY: test-snapshot-crash
test-snapshot-crash: $(BIN_DIR)/kvstore $(BIN_DIR)/test_kvstore_snapshot_fault
	python3 -B $(TEST_DIR)/test_snapshot_crash.py

.PHONY: test-replication-faults
test-replication-faults: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_replication_faults.py .

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

# 同一台机器的 ACTIVE iWARP 网卡上跑真实 RDMA；没有设备时明确失败。
.PHONY: test-rdma

test-rdma: $(BIN_DIR)/kvstore $(BIN_DIR)/test_kvstore_sync_fault
	python3 -B $(TEST_DIR)/test_rdma_replication.py .

test-integration: test-rdma

.PHONY: test-replication-boundary

test-replication-boundary: $(BIN_DIR)/test_replication_boundary
	./$(BIN_DIR)/test_replication_boundary

$(BIN_DIR)/test_replication_boundary: \
	$(TEST_DIR)/test_replication_boundary.c \
	$(filter-out $(BUILD_DIR)/kvstore.o $(BUILD_DIR)/ntyco.o $(BUILD_DIR)/rdma_transport.o,$(OBJS)) \
	$(wildcard $(INCLUDE_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror \
		$(TEST_DIR)/test_replication_boundary.c \
		$(filter-out $(BUILD_DIR)/kvstore.o $(BUILD_DIR)/ntyco.o $(BUILD_DIR)/rdma_transport.o,$(OBJS)) \
		-luring -lpthread -ldl $(RDMA_LIBS) -o $@
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

# eBPF ONLINE 实验构建；普通 all/test 构建使用原有 bin/kvstore。
BPF_CLANG ?= clang
BPF_ARCH ?= $(shell uname -m | sed 's/aarch64/arm64/;s/x86_64/x86/')

.PHONY: ebpf-startup test-ebpf-online test-ebpf-replication
ebpf-startup: $(BIN_DIR)/kvstore-ebpf

$(BUILD_DIR)/ebpf_capture.bpf.o: \
    $(REPLICATION_DIR)/ebpf_capture.bpf.c \
    $(INCLUDE_DIR)/ebpf_event.h | $(BUILD_DIR)
	$(BPF_CLANG) -target bpf -D__TARGET_ARCH_$(BPF_ARCH) -O2 -g \
	    -Wall -Wextra -Werror -I$(INCLUDE_DIR) -I/usr/include/$(shell $(CC) -dumpmachine) \
	    -c $< -o $@

$(BUILD_DIR)/kvstore_ebpf.o: \
    $(SRC_DIR)/kvstore.c \
    $(INCLUDE_DIR)/ebpf_capture.h \
    $(INCLUDE_DIR)/ebpf_event.h \
    $(INCLUDE_DIR)/kvstore.h \
    $(INCLUDE_DIR)/kvs_allocator.h \
    $(INCLUDE_DIR)/persistence.h \
    $(INCLUDE_DIR)/replication.h \
    $(INCLUDE_DIR)/server.h \
    $(INCLUDE_DIR)/protocol.h \
    $(INCLUDE_DIR)/config.h \
    $(INCLUDE_DIR)/log.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DKVS_EBPF_STARTUP_EXPERIMENT -c $< -o $@

$(BUILD_DIR)/replication_ebpf.o: \
    $(REPLICATION_DIR)/replication.c \
    $(INCLUDE_DIR)/ebpf_capture.h \
    $(INCLUDE_DIR)/ebpf_event.h \
    $(INCLUDE_DIR)/replication.h \
    $(INCLUDE_DIR)/persistence.h \
    $(INCLUDE_DIR)/server.h \
    $(INCLUDE_DIR)/rdma_transport.h \
    $(INCLUDE_DIR)/log.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DKVS_EBPF_STARTUP_EXPERIMENT -c $< -o $@

$(BUILD_DIR)/ebpf_capture.o: \
    $(REPLICATION_DIR)/ebpf_capture.c \
    $(INCLUDE_DIR)/ebpf_capture.h \
    $(INCLUDE_DIR)/ebpf_event.h \
    $(INCLUDE_DIR)/log.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -c $< -o $@

$(BIN_DIR)/kvstore-ebpf: \
    $(filter-out $(BUILD_DIR)/kvstore.o $(BUILD_DIR)/replication.o,$(OBJS)) \
    $(BUILD_DIR)/kvstore_ebpf.o \
    $(BUILD_DIR)/replication_ebpf.o \
    $(BUILD_DIR)/ebpf_capture.o \
    $(BUILD_DIR)/ebpf_capture.bpf.o \
    $(NTYCO_DIR)/libntyco.a | $(BIN_DIR)
	$(CC) $(filter-out $(BUILD_DIR)/kvstore.o $(BUILD_DIR)/replication.o,$(OBJS)) \
	    $(BUILD_DIR)/kvstore_ebpf.o $(BUILD_DIR)/replication_ebpf.o \
	    $(BUILD_DIR)/ebpf_capture.o $(LDFLAGS) $(LDLIBS) -luring -lbpf -o $@

$(BIN_DIR)/test_ebpf_online: \
    $(TEST_DIR)/test_ebpf_online.c \
    $(REPLICATION_DIR)/replication.c \
    $(SRC_DIR)/log/log.c \
    $(INCLUDE_DIR)/ebpf_capture.h \
    $(INCLUDE_DIR)/ebpf_event.h \
    $(INCLUDE_DIR)/replication.h \
    $(INCLUDE_DIR)/persistence.h \
    $(INCLUDE_DIR)/server.h \
    $(INCLUDE_DIR)/rdma_transport.h \
    $(INCLUDE_DIR)/log.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -DKVS_EBPF_STARTUP_EXPERIMENT \
	    -ffunction-sections -fdata-sections -fsanitize=address,undefined \
	    -fno-omit-frame-pointer $(TEST_DIR)/test_ebpf_online.c \
	    $(SRC_DIR)/log/log.c -Wl,--gc-sections -o $@

test-ebpf-online: $(BIN_DIR)/test_ebpf_online
	./$(BIN_DIR)/test_ebpf_online

# 普通单测不加载 BPF；真实专项必须具备 BPF 权限和 ACTIVE iWARP 设备。
test-unit: test-ebpf-online

test-ebpf-replication: $(BIN_DIR)/kvstore-ebpf
	KVS_EBPF_OBJECT=$(abspath $(BUILD_DIR)/ebpf_capture.bpf.o) \
	    python3 -B $(TEST_DIR)/test_ebpf_replication.py .

# 可选诊断：独立临时目录，不改变普通构建或 make test 的工具链要求。
FUZZ_RUNS ?= 50000
.PHONY: fuzz-protocol test-sanitizers
fuzz-protocol:
	python3 -B $(TEST_DIR)/run_protocol_fuzz.py --runs $(FUZZ_RUNS)

test-sanitizers:
	python3 -B $(TEST_DIR)/run_sanitizers.py

# 会议验收分层：普通 make test 保持功能/边界回归；大数据与可选客户端显式运行。
PERSISTENCE_STOP_METHOD ?= kill
.PHONY: test-functional-bulk test-persistence-100k test-clients test-clients-strict test-acceptance

test-functional-bulk: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_functional_bulk.py

test-persistence-100k: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_persistence_100k.py --stop-method $(PERSISTENCE_STOP_METHOD)

test-clients: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_client_compatibility.py

# 严格会议对照入口：默认 benchmark PING 不支持时明确非零退出。
test-clients-strict: $(BIN_DIR)/kvstore
	python3 -B $(TEST_DIR)/test_client_compatibility.py --require-default-ping

test-acceptance:
	$(MAKE) --no-print-directory test-functional-bulk
	$(MAKE) --no-print-directory test-persistence-100k
