# 91kvstore

```text
 ███     █    █   █  █   █   ████  █████   ███   ████   █████
█   █   ██    █  █   █   █  █        █    █   █  █   █  █
█   █    █    █ █    █   █  █        █    █   █  █   █  █
 ████    █    ██     █   █   ███     █    █   █  ████   ████
    █    █    █ █     █ █       █    █    █   █  █ █    █
   █     █    █  █    █ █       █    █    █   █  █  █   █
███    █████  █   █    █    ████     █     ███   █   █  █████
```

**A persistent key-value store in C, built from engines to replication.**

`Linux` · `GNU C11` · `4 storage engines` · `3 network backends` · `AOF + Snapshot` · `Primary-Replica`

91kvstore 是一个用于学习数据库系统的键值存储项目。它不只实现单个数据结构，而是将 **TCP 字节流 → 命令解析 → 存储引擎 → 持久化 → 主从复制** 接成一条可运行、可测试的链路。

> 本项目用于学习和实验，不建议用于生产环境。它接受 RESP 风格的长度前缀**请求**，但目前不是完整的 Redis/RESP2 实现，不能保证与 `redis-cli` 兼容。

## 一分钟体验

### 1. 构建并启动

需要 Linux、GCC、GNU Make、liburing，以及仓库中的 NtyCo 子模块。Ubuntu/Debian 可先安装：

```bash
sudo apt update
sudo apt install -y build-essential git liburing-dev netcat-openbsd
```

```bash
git clone --recursive https://github.com/JasperStonnne/91kvstore.git
cd 91kvstore
make

KVSTORE_BIN="$PWD/bin/kvstore"
mkdir -p /tmp/91kv-standalone
cd /tmp/91kv-standalone
"$KVSTORE_BIN" 2000
```

服务在前台运行。另开终端发送命令。AOF 和 Snapshot 文件会写入**启动服务时的工作目录**，因此示例使用独立的 `/tmp/91kv-standalone`。

已有仓库缺少子模块时，运行 `git submodule update --init --recursive`。

### 2. 写入与读取

旧的行命令仍可使用：

```bash
printf 'HSET username jasper\r\n' | nc -N 127.0.0.1 2000
# OK

printf 'HGET username\r\n' | nc -N 127.0.0.1 2000
# jasper
```

长度前缀命令能保留字段中的空格。下面写入 key `user name` 和 value `hello world`：

```bash
printf '*3\r\n$4\r\nHSET\r\n$9\r\nuser name\r\n$11\r\nhello world\r\n' |
  nc -N 127.0.0.1 2000
# OK
```

```bash
printf '*2\r\n$4\r\nHGET\r\n$9\r\nuser name\r\n' |
  nc -N 127.0.0.1 2000
# $11
# hello world
```

`$9` 和 `$11` 是后续字段的**字节长度**，不是命令语法中的固定数字；换成其他内容时也要相应修改。

## 系统如何工作

```text
                         TCP client
                             │
                             ▼
             Reactor / Proactor / NtyCo
                             │
                             ▼
                动态输入缓冲区
                             │
                             ▼
            行命令 / 长度前缀命令解析
                             │
                             ▼
                       命令执行
                  ┌──────────┴──────────┐
                  ▼                     ▼
          内存存储引擎          AOF 写入 / Snapshot
                  │                     │
                  └──────────┬──────────┘
                             ▼
                    动态输出缓冲区
                             │
                             ▼
                         TCP client

          Primary Snapshot + AOF
                    │
                    ▼
          Replica 全量同步 + 增量追赶
```

网络后端只负责连接、收发和缓冲区；长度前缀解析集中在协议模块。命令执行层根据命令前缀选择引擎，成功的写命令进入 AOF。复制模块复用 Snapshot 和 AOF，将 Primary 的状态传给 Replica。

| 层 | 位置 | 职责 |
| --- | --- | --- |
| 协议 | `src/protocol/` | 长度前缀命令解析与编码 |
| 网络 | `src/network/` | 三种后端、连接与动态缓冲区 |
| 引擎 | `src/engines/` | Array、Hash、RBTree、Skip List |
| 持久化 | `src/persistence/` | AOF、Snapshot、启动恢复 |
| 复制 | `src/replication/` | 握手、全量同步、增量同步 |
| 内存池 | `src/memory/` | 固定大小节点分配 |

## 命令与协议

### 命令

命令前缀选择存储引擎。`SET` 创建新键；要修改已有值，使用对应的 `MOD`。

| 操作 | Array | Red-Black Tree | Hash Table | Skip List |
| --- | --- | --- | --- | --- |
| 创建 | `SET` | `RSET` | `HSET` | `SSET` |
| 读取 | `GET` | `RGET` | `HGET` | `SGET` |
| 修改 | `MOD` | `RMOD` | `HMOD` | `SMOD` |
| 删除 | `DEL` | `RDEL` | `HDEL` | `SDEL` |
| 判断存在 | `EXIST` | `REXIST` | `HEXIST` | `SEXIST` |

创建、修改：`命令 key value`。读取、删除、判断存在：`命令 key`。`SAVE` 手动保存 Snapshot。

### 两种请求格式

**行命令**以换行结束，字段由现有文本命令处理器拆分：

```text
HSET username jasper\r\n
```

因此行命令不适合包含空格的 key/value。

**长度前缀命令**以 `*` 开头，每个字段携带字节长度：

```text
*3\r\n
$4\r\nHSET\r\n
$9\r\nuser name\r\n
$11\r\nhello world\r\n
```

解析器面对的是 TCP 字节流：一次 `recv` 可能只收到半条命令，也可能收到多条命令。它只在命令完整时交给执行层，并返回本次消费的字节数；未消费的字节继续留在连接的输入缓冲区。

**兼容性边界：** 请求格式借鉴 RESP 的数组和 bulk string，但回复并未全部采用 RESP2 类型。例如部分状态仍返回项目自身的文本回复。因此“能接收长度前缀请求”不等于“可直接用 `redis-cli` 操作所有命令”。字段最终会转换为 C 字符串，目前不支持嵌入 `\0` 的二进制 key/value。

## 持久化

91kvstore 同时使用 AOF 和 Snapshot：

```text
客户端写入 ──► 内存引擎
                   └──► appendonly.aof

SAVE ────────► snapshot.db
                  └── 首行记录 AOF_OFFSET
```

AOF 按顺序记录写命令；Snapshot 保存当前各引擎的全量状态。命令记录采用长度前缀格式，因此包含空格的字段在落盘和恢复时仍有明确边界。

Snapshot 首行示意：

```text
AOF_OFFSET 189
```

启动恢复顺序：

```text
加载 snapshot.db
      │
      ▼
读取 Snapshot 对应的 AOF_OFFSET
      │
      ▼
从该 offset 重放 appendonly.aof
      │
      ▼
启动网络服务
```

Snapshot 先写入临时文件，再替换正式文件，避免半成品覆盖旧快照。新版长度前缀持久化文件与旧版文本文件不应直接混用；升级已有数据时需要单独规划迁移。

## Primary-Replica 复制

Primary 接收客户端写入；Replica 可处理客户端读取，但拒绝客户端写入。复制是**异步**的：Primary 不等待 Replica 确认即可向客户端返回。

```text
CONNECTING
    │ 建立连接
    ▼
HANDSHAKE       PING / PONG / PSYNC
    │
    ▼
FULL_SYNC       发送并安装 Snapshot
    │
    ▼
CATCH_UP        从 Snapshot 的 AOF offset 补齐增量
    │
    ▼
ONLINE          持续读取新的 AOF 数据
```

分别在两个终端、两个不同工作目录中运行，避免共用 AOF 和 Snapshot：

```bash
# Primary：19000 为客户端端口，19100 为复制端口
mkdir -p /tmp/91kv-primary
cd /tmp/91kv-primary
/absolute/path/to/91kvstore/bin/kvstore primary 19000 19100
```

```bash
# Replica：19001 为自身客户端端口，连接 Primary 的 19100
mkdir -p /tmp/91kv-replica
cd /tmp/91kv-replica
/absolute/path/to/91kvstore/bin/kvstore replica 19001 127.0.0.1 19100
```

将示例中的 `/absolute/path/to/91kvstore` 换成实际仓库路径。

## 网络后端

三种后端共用协议和复制逻辑，区别在于等待网络事件的方式：

| 后端 | 模型 | 主要机制 |
| --- | --- | --- |
| Reactor | 就绪事件 | `epoll` |
| Proactor | 完成事件 | `io_uring` |
| NtyCo | 协程 | 协程调度与网络 hook |

默认使用 NtyCo。要切换后端，在 `include/kvstore.h` 修改 `NETWORK_SELECT`，然后重新构建：

```c
#define NETWORK_REACTOR  0
#define NETWORK_PROACTOR 1
#define NETWORK_NTYCO    2

#define NETWORK_SELECT NETWORK_NTYCO
```

```bash
make clean
make
```

动态输入缓冲区容纳分段到达的长命令；动态输出缓冲区容纳较长的回复。网络层仍需要处理部分发送，不能假设一次发送就写完全部字节。

## 内存管理

Hash、Red-Black Tree、Skip List 的固定大小节点可使用项目内存池；可变长度的 key/value 等仍使用通用分配器。构建时可以关闭相应内存池，便于测试和比较：

```bash
make clean
make HASH_USE_MEMORY_POOL=0 \
     RBTREE_USE_MEMORY_POOL=0 \
     SKIPLIST_USE_MEMORY_POOL=0
```

| 构建变量 | 默认值 | 作用 |
| --- | ---: | --- |
| `HASH_USE_MEMORY_POOL` | `1` | Hash 节点 |
| `RBTREE_USE_MEMORY_POOL` | `1` | 红黑树节点 |
| `SKIPLIST_USE_MEMORY_POOL` | `1` | 跳表节点 |
| `ARRAY_USE_JEMALLOC` | `0` | Array 基准测试中的 jemalloc 选项 |

## 测试

核心验证：

```bash
make test-protocol
bash tests/test_replication.sh
```

协议测试覆盖完整与不完整命令、错误输入和较长字段。复制集成测试覆盖 Snapshot 全量同步、ONLINE 增量同步、包含空格的字段及 2048 字节 value。复制测试使用端口 `29000`、`29001`、`29100`，运行前需确保端口空闲。

内存相关测试：

```bash
make test-memory-pool
make test-hash-memory-pool
make test-rbtree-memory-pool
make test-skiplist-memory-pool
make test-array-memory
```

仓库还提供 Array、Hash、RBTree、Skip List 的 allocator/memory benchmark 目标；具体目标可在 `Makefile` 中查看。

## 项目结构

```text
91kvstore/
├── clients/                 多语言 TCP 客户端示例
├── include/                 公共头文件
├── src/
│   ├── engines/             四种内存引擎
│   ├── memory/              内存池
│   ├── network/             Reactor / Proactor / NtyCo / 缓冲区
│   ├── persistence/         AOF / Snapshot
│   ├── protocol/            长度前缀编解码
│   ├── replication/         主从复制
│   └── kvstore.c            命令执行与程序入口
├── tests/                   测试与基准测试
├── third_party/NtyCo/       Git 子模块
└── Makefile
```

## 当前限制

- 不完整兼容 Redis 命令语义和 RESP2 回复；`redis-cli` 不是当前受保证的客户端
- key/value 不支持嵌入 `\0`；行命令也不支持字段中包含空格
- Replica 尚无断线自动重连、复制 backlog 或部分重同步
- 当前主要面向单 Replica 场景；尚无 ACK、心跳和自动故障转移
- Snapshot、AOF 的同步文件操作可能阻塞事件处理
- 尚未提供认证、访问控制或 TLS

## 许可证

仓库目前未声明开源许可证。在发布许可证前，保留所有权利。