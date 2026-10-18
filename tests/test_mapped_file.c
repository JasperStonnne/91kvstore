#include "mapped_file.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    /* 创建本测试自己的临时文件。 */
    char path[] = "/tmp/91kv-mapped-file-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    /*
     * 使用包含 NUL、空格和 CRLF 的原始字节。
     * 映射模块不解析字符串，应该完整保留所有字节。
     */
    const unsigned char expected[] = {
        0x00, 'A', ' ', '\r', '\n', 0xff
    };
    assert(write(fd, expected, sizeof(expected)) ==
           (ssize_t)sizeof(expected));

    kvs_mapped_file_t file = {0};

    /* 1. 映射非空文件，逐字节检查内容。 */
    assert(kvs_mapped_file_open(path, &file) == 0);
    assert(file.data != NULL);
    assert(file.length == sizeof(expected));
    assert(memcmp(file.data, expected, sizeof(expected)) == 0);

    /* 已持有映射时，不能再次打开并覆盖管理信息。 */
    const unsigned char *original_data = file.data;
    errno = 0;
    assert(kvs_mapped_file_open(path, &file) == -1);
    assert(errno == EINVAL);
    assert(file.data == original_data);
    assert(file.length == sizeof(expected));

    /* 释放成功后清零；重复释放空对象也允许。 */
    assert(kvs_mapped_file_close(&file) == 0);
    assert(file.data == NULL);
    assert(file.length == 0);
    assert(kvs_mapped_file_close(&file) == 0);
    puts("[PASS] nonempty file and mapping lifecycle");

    /* 2. 先释放映射，再把测试文件截断为空。 */
    assert(ftruncate(fd, 0) == 0);
    assert(kvs_mapped_file_open(path, &file) == 0);
    assert(file.data == NULL);
    assert(file.length == 0);
    assert(kvs_mapped_file_close(&file) == 0);
    puts("[PASS] empty file");

    /* 3. 删除文件后，打开应返回明确的不存在错误。 */
    assert(close(fd) == 0);
    assert(unlink(path) == 0);

    errno = 0;
    assert(kvs_mapped_file_open(path, &file) == -1);
    assert(errno == ENOENT);
    assert(file.data == NULL);
    assert(file.length == 0);
    puts("[PASS] missing file");

    puts("mapped file tests passed");
    return 0;
}