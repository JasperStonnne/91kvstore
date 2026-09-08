// 验证 Array 及其 key/value 的完整内存生命周期

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvstore.h"

// 独立测试没有链接 src/kvstore.c，
// 因此在这里提供 kvs_malloc/kvs_free。
void *kvs_malloc(size_t size) {
    return malloc(size);
}

void kvs_free(void *ptr) {
    free(ptr);
}

int main(void) {
    // 所有字段初始化为 0
    kvs_array_t array = {0};

    /*
     * 1. 创建 Array
     * create 会一次性申请整个 item 数组。
     */
    assert(kvs_array_create(&array) == 0);
    assert(array.table != NULL);
    assert(array.total == 0);

    /*
     * 2. 插入两个键值对
     * item 槽位来自已经创建好的 table；
     * key/value 字符串仍然单独动态分配。
     */
    assert(kvs_array_set(&array, "name", "Jasper") == 0);
    assert(kvs_array_set(&array, "city", "Shanghai") == 0);
    assert(array.total == 2);

    /*
     * 3. 验证查询结果
     */
    char *value = kvs_array_get(&array, "name");
    assert(value != NULL);
    assert(strcmp(value, "Jasper") == 0);

    value = kvs_array_get(&array, "city");
    assert(value != NULL);
    assert(strcmp(value, "Shanghai") == 0);

    /*
     * 4. 重复插入相同 key 应当失败
     * 返回 1 表示 key 已经存在，元素数量不能变化。
     */
    assert(kvs_array_set(&array, "name", "Another") == 1);
    assert(array.total == 2);

    value = kvs_array_get(&array, "name");
    assert(value != NULL);
    assert(strcmp(value, "Jasper") == 0);

    /*
     * 5. 修改 value
     * 正确顺序：
     * 先申请并复制新 value，
     * 成功后再释放旧 value。
     */
    assert(kvs_array_mod(&array, "name", "Stone") == 0);

    value = kvs_array_get(&array, "name");
    assert(value != NULL);
    assert(strcmp(value, "Stone") == 0);

    /*
     * 6. 删除 name
     *
     * Array 会释放 name 的 key/value，
     * 然后把最后一个有效元素 city 移到空出来的位置。
     */
    assert(kvs_array_del(&array, "name") == 0);
    assert(array.total == 1);
    assert(kvs_array_get(&array, "name") == NULL);

    /*
     * 删除和移动之后，city 仍然必须能够正常查询。
     */
    value = kvs_array_get(&array, "city");
    assert(value != NULL);
    assert(strcmp(value, "Shanghai") == 0);

    /*
     * 7. 删除不存在的 key
     * 返回 1，元素数量不能变化。
     */
    assert(kvs_array_del(&array, "missing") == 1);
    assert(array.total == 1);

    /*
     * 8. 销毁 Array
     *
     * 此时故意保留 city，让 destroy 负责释放：
     * - city 的 key
     * - city 的 value
     * - 整个 table
     */
    kvs_array_destory(&array);

    /*
     * 销毁后清空管理状态，避免留下悬空指针。
     */
    assert(array.table == NULL);
    assert(array.total == 0);
    assert(array.idx == 0);

    printf("array memory lifecycle test passed\n");
    return 0;
}