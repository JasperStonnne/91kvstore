//内存池 自身能否申请 归还 复用 扩容 与销毁
#include "memory_pool.h"                    // 引入内存池接口
#include <assert.h>                         // assert 用来验证条件
#include <stdio.h>                          // printf

int main(void) {
    memory_pool_t pool;                     // 在栈上创建内存池管理对象

    int result = memory_pool_init(
        &pool,                              // 把 pool 的地址传给 init
        32,                                 // 每个 block 是 32 字节
        2                                   // 每个 chunk 包含 2 个 block
    );

    assert(result == 0);                    // 初始化必须成功

    void *a = memory_pool_alloc(&pool);      // 第一次分配
    void *b = memory_pool_alloc(&pool);      // 第二次分配

    assert(a != NULL);                      // a 必须是有效地址
    assert(b != NULL);                      // b 必须是有效地址
    assert(a != b);                         // 两个正在使用的块不能重叠

    memory_pool_free(&pool, a);             // 把 a 放回 free_list
    void *c = memory_pool_alloc(&pool);      // 再分配一个 block

    assert(c == a);                         // 应该复用刚刚释放的 a
    void *d=memory_pool_alloc(&pool);
    assert(d!=NULL);
    assert(d!=b);
    assert(d!=c);
    printf("a = %p\n", a);
    printf("b = %p\n", b);
    printf("c = %p (same as a)\n", c);
    printf("d = %p (new chunk)\n", d);
    memory_pool_free(&pool, b);             // 把 b 放回内存池
    memory_pool_free(&pool, c);          // 把 c（原来的 a）放回内存池
    memory_pool_free(&pool,d);
    memory_pool_destory(&pool);             // 真正释放所有 chunk

    printf("memory pool test passed\n");
    return 0;
}