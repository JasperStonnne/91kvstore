#include "kvstore.h"
#include "persistence.h"
#include "kvs_allocator.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
extern kvs_array_t global_array;
extern kvs_rbtree_t global_rbtree;
extern kvs_hash_t global_hash;
extern kvs_skiplist_t global_skiplist;

/* 独立测试连接真实 allocator。 */
void *kvs_malloc(size_t size)
{
    return kvs_allocator_alloc(size);
}

void kvs_free(void *ptr)
{
    kvs_allocator_free(ptr);
}

static void create_engines(void)
{
    memset(&global_array, 0, sizeof(global_array));
    memset(&global_rbtree, 0, sizeof(global_rbtree));
    memset(&global_hash, 0, sizeof(global_hash));
    memset(&global_skiplist, 0, sizeof(global_skiplist));

    assert(kvs_array_create(&global_array) == 0);
    assert(kvs_rbtree_create(&global_rbtree) == 0);
    assert(kvs_hash_create(&global_hash) == 0);
    assert(kvs_skiplist_create(&global_skiplist) == 0);
}

static void destroy_engines(void)
{
    kvs_array_destroy(&global_array);
    kvs_rbtree_destroy(&global_rbtree);
    kvs_hash_destroy(&global_hash);
    kvs_skiplist_destroy(&global_skiplist);
}

static void expect_value(const char *actual, const char *expected)
{
    assert(actual != NULL);
    assert(strcmp(actual, expected) == 0);
}

static void expect_empty(void)
{
    assert(kvs_array_get(&global_array, "name") == NULL);
    assert(kvs_rbtree_get(&global_rbtree, "name") == NULL);
    assert(kvs_hash_get(&global_hash, "name") == NULL);
    assert(kvs_skiplist_get(&global_skiplist, "name") == NULL);
}

static void test_rejects_corruption(const char *path)
{
    int fd = open(path, O_RDWR);
    assert(fd >= 0);

    off_t size = lseek(fd, 0, SEEK_END);
    assert(size > 24);

    destroy_engines();
    create_engines();

    /* 修改最后一条记录的内容，保留原校验值。 */
    unsigned char original;
    assert(pread(fd, &original, 1, size - 5) == 1);

    unsigned char damaged = original ^ 1u;
    assert(pwrite(fd, &damaged, 1, size - 5) == 1);

    assert(kvs_snapshot_store_load(path) == -1);
    expect_empty();

    /* 恢复原内容，确认文件又能正常加载。 */
    assert(pwrite(fd, &original, 1, size - 5) == 1);
    assert(kvs_snapshot_store_load(path) == 0);
    expect_value(kvs_skiplist_get(&global_skiplist, "name"), "Skiplist");

    destroy_engines();
    create_engines();

    /* 缺少最后一个字节，应拒绝。 */
    assert(ftruncate(fd, size - 1) == 0);
    assert(kvs_snapshot_store_load(path) == -1);
    expect_empty();

    /* 零字节文件不是合法的空数据快照。 */
    assert(ftruncate(fd, 0) == 0);
    assert(kvs_snapshot_store_load(path) == -1);
    expect_empty();

    assert(close(fd) == 0);
    puts("snapshot corruption tests passed");
}

static void test_save_failure_preserves_old_snapshot(const char *path)
{
    /* 1. 备份旧快照的字节内容。 */
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);

    struct stat before;
    assert(fstat(fd, &before) == 0);
    assert(before.st_size > 0);

    size_t length = (size_t)before.st_size;
    unsigned char *original = malloc(length);
    unsigned char *actual = malloc(length);
    assert(original != NULL && actual != NULL);
    assert(pread(fd, original, length, 0) == (ssize_t)length);

    /* 2. 修改内存数据，让它与旧快照不同。 */
    assert(kvs_hash_mod(
        &global_hash, "name", "must not replace old snapshot") == 0);

    /* 3. 保存测试进程原来的限制和信号处理方式。 */
    struct rlimit original_limit;
    assert(getrlimit(RLIMIT_FSIZE, &original_limit) == 0);

    struct sigaction original_action;
    struct sigaction ignored_action = {0};
    ignored_action.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignored_action.sa_mask) == 0);
    assert(sigaction(
        SIGXFSZ, &ignored_action, &original_action) == 0);

    /* 4. 临时限制文件大小，让本次 SAVE 的写入失败。 */
    struct rlimit limited = original_limit;
    limited.rlim_cur = 0;
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);

    int save_result = kvs_snapshot_store_save(path);
    int save_errno = errno;

    /* 5. 立即恢复原来的测试环境。 */
    assert(setrlimit(RLIMIT_FSIZE, &original_limit) == 0);
    assert(sigaction(SIGXFSZ, &original_action, NULL) == 0);

    assert(save_result == -1);
    assert(save_errno == EFBIG);

    /* 6. 正式文件没有被替换，长度和内容也没有变化。 */
    struct stat after;
    assert(stat(path, &after) == 0);
    assert(after.st_dev == before.st_dev);
    assert(after.st_ino == before.st_ino);
    assert(after.st_size == before.st_size);

    assert(pread(fd, actual, length, 0) == (ssize_t)length);
    assert(memcmp(actual, original, length) == 0);

    assert(close(fd) == 0);
    free(actual);
    free(original);

    /* 7. 清空内存，确认旧快照仍能恢复旧值。 */
    destroy_engines();
    create_engines();
    assert(kvs_snapshot_store_load(path) == 0);
    expect_value(kvs_hash_get(&global_hash, "name"), "Hash");

    puts("snapshot write failure preserves old snapshot");
}

static void test_v1_compatibility(const char *path)
{
    /* 固定 v1 文件：Hash 引擎中的 name=v1，包含原格式 CRC。 */
    static const unsigned char fixture[] = {
        0x39, 0x31, 0x4b, 0x56, 0x53, 0x4e, 0x41, 0x50,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x02, 0x6e, 0x61, 0x6d,
        0x65, 0x76, 0x31, 0x49, 0x26, 0xcd, 0x56
    };

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, fixture, sizeof(fixture)) ==
           (ssize_t)sizeof(fixture));
    assert(close(fd) == 0);

    kvs_snapshot_checkpoint_t checkpoint = {
        .has_offset = -1,
        .offset = -1
    };

    assert(kvs_snapshot_store_load_with_checkpoint(
        path, &checkpoint) == 0);
    expect_value(kvs_hash_get(&global_hash, "name"), "v1");
    assert(checkpoint.has_offset == 0);
    assert(checkpoint.offset == 0);

    destroy_engines();
    create_engines();

    puts("snapshot v1 compatibility test passed");
}

static unsigned int fixture_crc(const unsigned char *data, size_t length)
{
    unsigned int crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0);
    }
    return crc ^ 0xffffffffu;
}

static void expect_snapshot_rejected_unchanged(const char *path,
                                              const unsigned char *data, size_t length)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, data, length) == (ssize_t)length);
    assert(kvs_snapshot_store_load(path) == -1);
    expect_empty();
    unsigned char actual[64];
    assert(length < sizeof(actual));
    assert(pread(fd, actual, sizeof(actual), 0) == (ssize_t)length);
    assert(memcmp(actual, data, length) == 0);
    assert(close(fd) == 0);
}

static void test_fixed_snapshot_boundaries(const char *path)
{
    /* 与已有兼容测试相同的完整 v1 快照：Hash name=v1。 */
    static const unsigned char fixture[] = {
        0x39,0x31,0x4b,0x56,0x53,0x4e,0x41,0x50,
        0,0,0,1, 0,0,0,0,0,0,0,1, 3, 0,0,0,4, 0,0,0,2,
        'n','a','m','e','v','1', 0x49,0x26,0xcd,0x56
    };
    destroy_engines();
    create_engines();
    for (size_t length = 0; length < sizeof(fixture); length++)
        expect_snapshot_rejected_unchanged(path, fixture, length);

    /* 重算正确 CRC，让结构检查负责拒绝，而非仅依赖校验和。 */
    const size_t positions[] = {8, 12, 20, 21, 25};
    for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
        unsigned char altered[sizeof(fixture)];
        memcpy(altered, fixture, sizeof(fixture));
        altered[positions[i]] = 0xff;
        unsigned int crc = fixture_crc(altered, sizeof(altered) - 4);
        for (size_t byte = 0; byte < 4; byte++)
            altered[sizeof(altered) - 4 + byte] = (unsigned char)(crc >> (24 - byte * 8));
        expect_snapshot_rejected_unchanged(path, altered, sizeof(altered));
    }
    puts("snapshot all truncation positions and CRC-valid structural boundaries passed");
}

static void fixture_u32(unsigned char *bytes, uint32_t value)
{
    for (unsigned int i = 0; i < 4; i++) {
        bytes[i] = (unsigned char)(value >> (24 - i * 8));
    }
}

static void fixture_write(const char *path, const unsigned char *bytes, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, bytes, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

/* 固定 v2 布局独立生成，避免用新写入器充当兼容性证据。 */
static void test_v2_compatibility(const char *path)
{
    const uint64_t offsets[] = {0, (UINT64_C(1) << 40) + 123};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        unsigned char bytes[51] = "91KVSNAP";
        fixture_u32(bytes + 8, 2);
        fixture_u32(bytes + 12, 1);
        bytes[23] = 1;
        for (unsigned int j = 0; j < 8; j++) {
            bytes[24 + j] = (unsigned char)(offsets[i] >> (56 - j * 8));
        }
        bytes[32] = KVS_SNAPSHOT_ENGINE_HASH;
        fixture_u32(bytes + 33, 4);
        fixture_u32(bytes + 37, 2);
        memcpy(bytes + 41, "namev2", 6);
        fixture_u32(bytes + 47, fixture_crc(bytes, 47));
        destroy_engines();
        create_engines();
        fixture_write(path, bytes, sizeof(bytes));
        kvs_snapshot_checkpoint_t loaded = {.has_offset = -1, .offset = -1};
        assert(kvs_snapshot_store_load_with_checkpoint(path, &loaded) == 0);
        assert(loaded.has_offset == 1 && loaded.offset == (long long)offsets[i]);
        expect_value(kvs_hash_get(&global_hash, "name"), "v2");

        destroy_engines();
        create_engines();
        bytes[31] ^= 1;
        fixture_write(path, bytes, sizeof(bytes));
        assert(kvs_snapshot_store_load(path) == -1);
        expect_empty();
        bytes[31] ^= 1;
        fixture_write(path, bytes, sizeof(bytes));
        assert(kvs_snapshot_store_load_with_policy(path, KVS_SNAPSHOT_LOAD_REQUIRE_FILE) == 0);
    }
    destroy_engines();
    create_engines();
    puts("snapshot v2 checkpoint compatibility and corruption passed");
}

/* 每个引擎各一条，同名 key 用于核对固定四段的归属。 */
static void test_v3_modes(const char *path)
{
    char *values[] = {"Array", "Tree", "Hash", "Skiplist"};
    for (int mode = KVS_SNAPSHOT_CHECKSUM_FILE; mode <= KVS_SNAPSHOT_CHECKSUM_RECORD; mode++) {
        destroy_engines();
        create_engines();
        assert(kvs_array_set(&global_array, "name", values[0]) == 0);
        assert(kvs_rbtree_set(&global_rbtree, "name", values[1]) == 0);
        assert(kvs_hash_set(&global_hash, "name", values[2]) == 0);
        assert(kvs_skiplist_set(&global_skiplist, "name", values[3]) == 0);
        assert(kvs_snapshot_store_save_with_checksum(path, (kvs_snapshot_checksum_t)mode) == 0);
        unsigned char bytes[256];
        int fd = open(path, O_RDONLY);
        assert(fd >= 0);
        ssize_t length = read(fd, bytes, sizeof(bytes));
        assert(length > 52 && close(fd) == 0);
        assert(bytes[11] == 3 && bytes[12] == mode);
        size_t starts[4];
        size_t position = 48;
        uint32_t structure = 0xffffffffu;
        /* fixture_crc 返回完成后的值，可用串接结构字节独立校验。 */
        unsigned char structure_bytes[64];
        memcpy(structure_bytes, bytes, 48);
        for (size_t section = 0; section < 4; section++) {
            assert(bytes[23 + section * 8] == 1);
            starts[section] = position;
            size_t body_length = 8 + 4 + strlen(values[section]);
            if (mode == KVS_SNAPSHOT_CHECKSUM_RECORD) {
                unsigned char expected[4];
                fixture_u32(expected, (uint32_t)body_length);
                assert(memcmp(bytes + position, expected, 4) == 0);
                memcpy(structure_bytes + 48 + section * 4, bytes + position, 4);
                position += 4;
                uint32_t checksum = fixture_crc(bytes + position, body_length);
                fixture_u32(expected, checksum);
                assert(memcmp(bytes + position + body_length, expected, 4) == 0);
            }
            assert(bytes[position + 3] == 4);
            assert(memcmp(bytes + position + 8, "name", 4) == 0);
            assert(memcmp(bytes + position + 12, values[section], strlen(values[section])) == 0);
            position += body_length;
            if (mode == KVS_SNAPSHOT_CHECKSUM_RECORD) {
                position += 4;
            }
        }
        assert(position + 4 == (size_t)length);
        structure = fixture_crc(structure_bytes, sizeof(structure_bytes));
        unsigned char expected[4];
        uint32_t checksum = structure;
        if (mode == KVS_SNAPSHOT_CHECKSUM_FILE) {
            checksum = fixture_crc(bytes, (size_t)length - 4);
        }
        fixture_u32(expected, checksum);
        assert(memcmp(expected, bytes + length - 4, 4) == 0);

        /* 每一个截断位置均拒绝，且不开始恢复。 */
        destroy_engines();
        create_engines();
        for (size_t cut = 0; cut < (size_t)length; cut++) {
            fixture_write(path, bytes, cut);
            assert(kvs_snapshot_store_load(path) == -1);
            expect_empty();
        }
        fixture_write(path, bytes, (size_t)length);
        if (mode == KVS_SNAPSHOT_CHECKSUM_RECORD) {
            assert(kvs_snapshot_store_load_with_policy(path, KVS_SNAPSHOT_LOAD_REQUIRE_FILE) == -1);
            expect_empty();
        }
        assert(kvs_snapshot_store_load(path) == 0);
        expect_value(kvs_array_get(&global_array, "name"), values[0]);
        expect_value(kvs_rbtree_get(&global_rbtree, "name"), values[1]);
        expect_value(kvs_hash_get(&global_hash, "name"), values[2]);
        expect_value(kvs_skiplist_get(&global_skiplist, "name"), values[3]);

        /* 首、中、末记录内容损坏：record 仅跳过本条，file 全拒绝。 */
        for (size_t section = 0; section < 4; section++) {
            size_t value_offset = starts[section] + 12;
            if (mode == KVS_SNAPSHOT_CHECKSUM_RECORD) {
                value_offset += 4;
            }
            bytes[value_offset] ^= 1;
            destroy_engines();
            create_engines();
            fixture_write(path, bytes, (size_t)length);
            if (mode == KVS_SNAPSHOT_CHECKSUM_RECORD) {
                assert(kvs_snapshot_store_load(path) == 0);
                const char *actual[] = {
                    kvs_array_get(&global_array, "name"), kvs_rbtree_get(&global_rbtree, "name"),
                    kvs_hash_get(&global_hash, "name"), kvs_skiplist_get(&global_skiplist, "name")
                };
                for (size_t other = 0; other < 4; other++) {
                    if (section == other) {
                        assert(actual[other] == NULL);
                    } else {
                        expect_value(actual[other], values[other]);
                    }
                }
            } else {
                assert(kvs_snapshot_store_load(path) == -1);
                expect_empty();
            }
            bytes[value_offset] ^= 1;
        }
        destroy_engines();
        create_engines();
        /* 文件头、段表、可信边界和尾部校验损坏，全份拒绝。 */
        const size_t positions[] = {0, 8, 12, 13, 23, 31, 39, 47, 48, (size_t)length - 1};
        for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
            bytes[positions[i]] ^= 1;
            fixture_write(path, bytes, (size_t)length);
            assert(kvs_snapshot_store_load(path) == -1);
            expect_empty();
            bytes[positions[i]] ^= 1;
        }
        /* 额外尾字节也属于结构破坏。 */
        bytes[length] = 0;
        fixture_write(path, bytes, (size_t)length + 1);
        assert(kvs_snapshot_store_load(path) == -1);
        expect_empty();

        if (mode == KVS_SNAPSHOT_CHECKSUM_RECORD) {
            /* 内部长度损坏可跳过：不能用坏 key_len 找下一条。 */
            bytes[starts[1] + 4] = 0xff;
            fixture_write(path, bytes, (size_t)length);
            assert(kvs_snapshot_store_load(path) == 0);
            assert(kvs_rbtree_get(&global_rbtree, "name") == NULL);
            expect_value(kvs_hash_get(&global_hash, "name"), "Hash");
            bytes[starts[1] + 4] = 0;

            /* CRC 自身损坏也只跳过本条。 */
            destroy_engines();
            create_engines();
            size_t body_length = 8 + 4 + strlen(values[1]);
            size_t checksum_offset = starts[1] + 4 + body_length;
            bytes[checksum_offset] ^= 1;
            fixture_write(path, bytes, (size_t)length);
            assert(kvs_snapshot_store_load(path) == 0);
            assert(kvs_rbtree_get(&global_rbtree, "name") == NULL);
            expect_value(kvs_skiplist_get(&global_skiplist, "name"), "Skiplist");
            bytes[checksum_offset] ^= 1;

            /* CRC 正确但内部长度或 NUL 不合法，仍应跳过。 */
            const size_t invalid_body_positions[] = {starts[1] + 4, starts[1] + 12};
            for (size_t i = 0; i < 2; i++) {
                destroy_engines();
                create_engines();
                size_t bad = invalid_body_positions[i];
                unsigned char saved = bytes[bad];
                bytes[bad] = 0;
                if (i == 0) {
                    bytes[bad] = 0xff;
                }
                fixture_u32(bytes + checksum_offset,
                            fixture_crc(bytes + starts[1] + 4, body_length));
                fixture_write(path, bytes, (size_t)length);
                assert(kvs_snapshot_store_load(path) == 0);
                assert(kvs_rbtree_get(&global_rbtree, "name") == NULL);
                expect_value(kvs_hash_get(&global_hash, "name"), "Hash");
                bytes[bad] = saved;
            }
            fixture_u32(bytes + checksum_offset,
                        fixture_crc(bytes + starts[1] + 4, body_length));
        } else {
            /* 重算正确文件 CRC，仍由结构解析拒绝超长字段。 */
            bytes[48] = 0xff;
            fixture_u32(bytes + length - 4, fixture_crc(bytes, (size_t)length - 4));
            fixture_write(path, bytes, (size_t)length);
            assert(kvs_snapshot_store_load(path) == -1);
            expect_empty();
        }
    }
    destroy_engines();
    create_engines();
    for (int mode = KVS_SNAPSHOT_CHECKSUM_FILE; mode <= KVS_SNAPSHOT_CHECKSUM_RECORD; mode++) {
        assert(kvs_snapshot_store_save_with_checksum(path, (kvs_snapshot_checksum_t)mode) == 0);
        struct stat info;
        assert(stat(path, &info) == 0 && info.st_size == 52);
        assert(kvs_snapshot_store_load(path) == 0);
        expect_empty();
    }
    errno = 0;
    assert(kvs_snapshot_store_save_with_checksum(path, (kvs_snapshot_checksum_t)99) == -1);
    assert(errno == EINVAL);
    puts("snapshot v3 file/record layout, skip, strict policy and structural rejection passed");
}

int main(void)
{
    assert(kvs_allocator_init(KVS_ALLOCATOR_POOL) == 0);

    char directory[] = "/tmp/91kv-snapshot-test-XXXXXX";
    assert(mkdtemp(directory) != NULL);

    char path[256];
    int length = snprintf(
        path, sizeof(path), "%s/snapshot.bin", directory);
    assert(length > 0 && (size_t)length < sizeof(path));

    /* 失败时保留目录，方便检查。 */
    printf("Snapshot test directory: %s\n", directory);
    fflush(stdout);

    create_engines();

    /* 不存在的文件与损坏文件是不同情况。 */
    assert(kvs_snapshot_store_load(path) == 1);
    expect_empty();
    test_v1_compatibility(path);

    /* 空数据也必须能保存和恢复。 */
    /* 空数据也必须能保存和恢复。 */
    int save_result = kvs_snapshot_store_save(path);
    if (save_result != 0) {
        perror("empty snapshot save");
    }
    assert(save_result == 0);
    destroy_engines();
    create_engines();
    assert(kvs_snapshot_store_load(path) == 0);
    expect_empty();

    /* 相同 key 分属四种引擎，应全部保留。 */
    assert(kvs_array_set(&global_array, "name", "Array") == 0);
    assert(kvs_rbtree_set(&global_rbtree, "name", "Tree") == 0);
    assert(kvs_hash_set(&global_hash, "name", "Hash") == 0);
    assert(kvs_skiplist_set(&global_skiplist, "name", "Skiplist") == 0);

    assert(kvs_snapshot_store_save(path) == 0);

    test_save_failure_preserves_old_snapshot(path);

    /* 更新、删除后再次保存，替换上一份快照。 */
    assert(kvs_hash_mod(
        &global_hash, "name", "updated value\r\nsecond line") == 0);

    assert(kvs_rbtree_set(
        &global_rbtree, "deleted", "temporary") == 0);
    assert(kvs_rbtree_del(&global_rbtree, "deleted") == 0);

    assert(kvs_snapshot_store_save(path) == 0);

    /* 清空内存，再通过公开接口恢复。 */
    destroy_engines();
    create_engines();

    assert(kvs_snapshot_store_load(path) == 0);

    expect_value(kvs_array_get(&global_array, "name"), "Array");
    expect_value(kvs_rbtree_get(&global_rbtree, "name"), "Tree");
    expect_value(
        kvs_hash_get(&global_hash, "name"),
        "updated value\r\nsecond line");
    expect_value(kvs_skiplist_get(&global_skiplist, "name"), "Skiplist");

    assert(kvs_rbtree_get(&global_rbtree, "deleted") == NULL);
    test_rejects_corruption(path);
    test_v2_compatibility(path);
    test_v3_modes(path);
    test_fixed_snapshot_boundaries(path);
    destroy_engines();

    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);

    /* 引擎与 Snapshot 临时对象都应该已经释放。 */
    kvs_pool_stats_t stats;
    assert(kvs_allocator_pool_stats(&stats) == 0);
    assert(stats.live_objects == 0);
    assert(stats.large_count == 0);

    assert(kvs_allocator_destroy() == 0);

    puts("snapshot public API tests passed allocator=pool");
    return 0;
    return 0;
}