#include "kvstore.h"
#include "persistence.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
extern kvs_array_t global_array;
extern kvs_rbtree_t global_rbtree;
extern kvs_hash_t global_hash;
extern kvs_skiplist_t global_skiplist;

/* 独立测试不链接包含 main 的 kvstore.c。 */
void *kvs_malloc(size_t size)
{
    return malloc(size);
}

void kvs_free(void *ptr)
{
    free(ptr);
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
    kvs_array_destory(&global_array);
    kvs_rbtree_destory(&global_rbtree);
    kvs_hash_destory(&global_hash);
    kvs_skiplist_destory(&global_skiplist);
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

int main(void)
{
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
    destroy_engines();

    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);

    puts("snapshot public API tests passed");
    return 0;
}