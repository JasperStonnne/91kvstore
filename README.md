# 91kvstore

```text
  999   1   K  K  V   V   SSS  TTTTT   OOO   RRRR   EEEEE
 9   9 11   K K   V   V  S       T    O   O  R   R  E
  9999  1   KK    V   V   SSS    T    O   O  RRRR   EEEE
     9  1   K K    V V       S   T    O   O  R  R   E
  999  111  K  K    V    SSSS    T     OOO   R   R  EEEEE
```

**一个用 C 实现的持久化键值存储实验项目**

存储引擎 · TCP 协议 · AOF / Snapshot · 主从复制


91kvstore 用于实践键值数据库的核心链路：命令解析、内存存储、网络事件处理、持久化恢复和 Primary-Replica 复制。项目面向 Linux，目前提供四种内存引擎和三种可选网络后端。

> 这是学习与实验项目，不建议用于生产环境。

## 功能概览

- Array、Hash Table、Red-Black Tree、Skip List 四种存储引擎
- Reactor/epoll、Proactor/io_uring、NtyCo 协程三种网络后端
- CRLF 行命令及长度前缀命令；处理 TCP 半包、粘包和部分发送
- AOF 写入与启动重放、Snapshot 保存及基于 AOF offset 的恢复
- 基于 Snapshot 全量同步和 AOF 增量流的异步主从复制
- 固定大小节点内存池、协议测试、内存测试和基准测试

## 环境与构建

需要 Linux、GCC（GNU C11）、GNU Make、liburing 和 pthread。Ubuntu/Debian 可安装：

```bash
sudo apt update
sudo apt install -y build-essential git liburing-dev netcat-openbsd
```

```bash
git clone --recursive https://github.com/JasperStonnne/91kvstore.git
cd 91kvstore
make
```

已有仓库缺少 NtyCo 子模块时，运行 `git submodule update --init --recursive`。

## 快速开始

在独立的工作目录启动服务；`appendonly.aof` 和 `snapshot.db` 保存在**启动时的工作目录**：

```bash
mkdir -p /tmp/91kv-standalone
cd /tmp/91kv-standalone
/path/to/91kvstore/bin/kvstore 2000
```

另开终端发送旧格式行命令：

```bash
printf 'HSET username jasper\r\n' | nc -N 127.0.0.1 2000
printf 'HGET username\r\n' | nc -N 127.0.0.1 2000
```

也可以发送长度前缀命令。下面的 key 为 `user name`，value 为 `hello world`，字段中的空格不会被拆开：

```bash
printf '*3\r\n$4\r\nHSET\r\n$9\r\nuser name\r\n$11\r\nhello world\r\n' |
  nc -N 127.0.0.1 2000
printf '*2\r\n$4\r\nHGET\r\n$9\r\nuser name\r\n' |
  nc -N 127.0.0.1 2000
```

长度前缀请求采用 `*字段数\r\n`，每个字段采用 `$字节数\r\n内容\r\n`。解析器按字节长度识别完整命令，网络层可以分多次接收同一条命令。**这不是完整的 Redis/RESP2 实现**：部分回复仍使用项目自身的文本状态，不能保证 `redis-cli` 兼容；字段也暂不支持嵌入 `\0`。

## 命令

命令前缀选择存储引擎；`SET` 创建新键，`MOD` 修改已有键。

| 操作 | Array | Red-Black Tree | Hash Table | Skip List |
| --- | --- | --- | --- | --- |
| 创建 | `SET` | `RSET` | `HSET` | `SSET` |
| 读取 | `GET` | `RGET` | `HGET` | `SGET` |
| 修改 | `MOD` | `RMOD` | `HMOD` | `SMOD` |
| 删除 | `DEL` | `RDEL` | `HDEL` | `SDEL` |
| 判断存在 | `EXIST` | `REXIST` | `HEXIST` | `SEXIST` |

创建和修改使用 `命令 key value`；读取、删除和判断存在使用 `命令 key`。`SAVE` 手动保存 Snapshot。行命令中的字段仍按空格拆分；需要在 key/value 中保留空格时使用长度前缀格式。

## 架构

```text
TCP client
    ↓
Reactor / Proactor / NtyCo
    ↓
输入缓冲区 → 命令帧解析 → 命令执行 → 输出缓冲区
                              ├→ Array / Hash / RBTree / SkipList
                              ├→ AOF
                              └→ Snapshot
Primary AOF + Snapshot ──复制状态机──→ Replica
```

```text
include/             公共接口
src/protocol/         长度前缀命令编解码
src/network/          网络后端与缓冲区
src/engines/          四种存储引擎
src/persistence/      AOF 与 Snapshot
src/replication/      主从复制状态机
src/memory/           节点内存池
tests/                测试与基准测试
third_party/NtyCo/    Git 子模块
```

默认网络后端为 NtyCo。可在 `include/kvstore.h` 修改 `NETWORK_SELECT`，然后运行 `make clean && make` 重新构建。

## 持久化

成功的写命令追加到 `appendonly.aof`；`SAVE` 将当前各引擎的数据写入 `snapshot.db`。两者的命令记录使用长度前缀格式，因此包含空格的字段仍能按原样恢复。

Snapshot 首行记录对应的 `AOF_OFFSET`。服务启动时先加载 Snapshot，再从该 offset 重放 AOF。Snapshot 通过临时文件写入并在完成后替换正式文件。旧版文本格式的持久化文件不应直接与新版混用；迁移前请单独处理原有数据。

## 主从复制

Primary 接收客户端写入；Replica 提供客户端读取并拒绝客户端写入。复制是**异步**的，Primary 不等待 Replica 确认即返回写入结果。

```text
CONNECTING → HANDSHAKE → FULL_SYNC → CATCH_UP → ONLINE
                         Snapshot    AOF 增量    持续同步
```

分别在两个不同的工作目录启动，避免共用 AOF 和 Snapshot：

```bash
# Primary
mkdir -p /tmp/91kv-primary && cd /tmp/91kv-primary
/path/to/91kvstore/bin/kvstore primary 19000 19100
```

```bash
# Replica，在另一个终端
mkdir -p /tmp/91kv-replica && cd /tmp/91kv-replica
/path/to/91kvstore/bin/kvstore replica 19001 127.0.0.1 19100
```

`19000` 和 `19001` 是客户端端口；`19100` 是 Primary 的复制端口。

## 测试

```bash
make test-protocol
bash tests/test_replication.sh
make test-memory-pool
make test-hash-memory-pool
make test-rbtree-memory-pool
make test-skiplist-memory-pool
make test-array-memory
```

协议测试覆盖完整、分段、错误和较长的命令。复制集成测试覆盖 Snapshot 全量同步、ONLINE 增量同步、含空格字段及 2048 字节 value。复制脚本使用固定测试端口 `29000`、`29001` 和 `29100`，运行前应确保端口空闲。

## 当前限制

- 不提供完整 Redis 命令语义或 RESP2 回复兼容性
- 命令字段会转换为 C 字符串，不支持嵌入 `\0` 的二进制 key/value
- Replica 尚无断线自动重连、部分重同步或自动故障转移
- 当前主要面向单 Replica 场景；没有 ACK、心跳和认证/TLS
- Snapshot 与 AOF 文件操作可能阻塞网络事件处理

## 许可证

仓库目前未声明开源许可证；在许可证公布前，保留所有权利。