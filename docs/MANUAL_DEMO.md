# 91KV 手动演示全流程操作手册

按本次已经实际走通的顺序整理。你手动启动、SAVE、宕机、重启；C 测试用例一次只负责一个操作。
所有命令在 Linux 开发机执行，HTML 可以在 Mac 或 Linux 浏览器中离线阅读。
**逐步执行，看到本步预期结果后再继续，不要一次粘贴整章。**

本文只使用你自己的 config/standalone.conf、config/primary.conf、config/replica.conf。
clone 在哪个目录都可以；每个窗口先进入各自的仓库根目录，不需要另外复制程序或新建运行目录。

## 0. 窗口、机器和演示顺序

| 窗口 | 在哪台机器 | 用途 |
| --- | --- | --- |
| A：Master 服务窗口 | 第一台 Linux，172.16.145.135 | 先做单机演示，之后启动 Master；启动后保持前台运行 |
| B：客户端窗口 | 第一台 Linux，172.16.145.135 | 运行 testcase、redis-cli、hiredis、benchmark；查询 PID 并手动 kill |
| C：Slave 服务窗口 | 第二台 Linux，172.16.145.136 | 主从演示时才打开；在第二台自己的仓库中启动 Slave |

| 顺序 | 这一项要证明什么 | 你亲自控制的动作 |
| --- | --- | --- |
| 1. 全量持久化 | 十万条 SAVE 后，宕机重启能恢复并逐条一致 | 启动 → 写入 → SAVE → kill -9 → 重启 → verify |
| 2. AOF 增量持久化 | 不 SAVE，十万条仍能从 AOF 恢复 | 改配置 → 启动 → 写入 → kill -9 → 重启 → verify |
| 3. 独立功能用例 | 万级修改、存在性、删除，以及结果校验 | 逐条运行指定功能的 C 用例 |
| 4. redis-cli / hiredis | 现成客户端能操作 kvstore，特殊字符不丢失 | 亲自发送命令、编译和运行 hiredis 客户端 |
| 5. benchmark / pipeline | PING、HGET 可用，连续批量请求可以处理 | 运行指定命令；-P 16 展示流水线 |
| 6. 双机主从 | 先五万条 → 启动 Slave → 再五万条 → Slave 校验十万条 | 保持两台机器独立，按时机启动 Slave |
| 7. 源码说明 | NtyCo、io_uring、mmap 的实现位置，以及用例为何可信 | 打开源码说明数据生成、网络请求、逐字节比较 |

地址采用本次已验证的两台主机。如果现场换了 IP，配置与客户端参数必须一起改。
下文 PASS、文件大小和日志是检查标准或本次观察示例，HTML 页面本身不会执行命令或宣布新一轮通过。

## 1. 开始前：目录、编译、工具和配置

### 1.1 每个终端先进入仓库

**操作位置：A、B；第二台 C 连接后也单独执行。**

```bash
cd ~/dev/91kvstore
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `cd` | 切换当前目录 |
| `~` | 当前用户的主目录；本次为 /home/jaspersao |
| `~/dev/91kvstore` | 本次仓库路径；如果你 clone 到别处，只把这里换成实际仓库路径 |

**看到什么再继续：** 命令提示符位于仓库根目录，能访问 Makefile、config、tests 和 bin。

### 1.2 不确定目录时查看当前位置

**操作位置：任意窗口。**

```bash
pwd
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `pwd` | 打印当前工作目录，无需参数 |

**看到什么再继续：** 路径应是你自己的仓库目录。kvstore 的数据文件保存在它启动时的工作目录，不是配置文件目录。

### 1.3 编译服务端和 C 用例

**操作位置：Master · B。**

```bash
make all testcase
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `make` | 读取当前目录的 Makefile，按规则编译 |
| `all` | 构建服务程序 bin/kvstore |
| `testcase` | 构建测试程序 bin/testcase，源文件是 tests/testcase.c |

**看到什么再继续：** 编译成功，无 error。已经是最新时可能提示无需构建，也属于正常结果。

**make testcase 只编译测试程序，不启动服务，不执行测试，不插数据。** make all testcase 则把服务与测试程序都准备好。

现场重新 clone 前，要确保本次修改的 C 用例已提交并推送；未提交的本地修改不会自动出现在新 clone 中。

### 1.4 确认 redis-cli 已安装

**操作位置：Master · B。**

```bash
redis-cli --version
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-cli` | 命令行客户端工具，向 kvstore 发送 RESP 命令 |
| `--version` | 只显示版本，不连接服务 |

**看到什么再继续：** 显示工具版本。你本次使用的开发机已经安装；若换机器后找不到命令，需要先安装。

### 1.5 确认 benchmark 已安装

**操作位置：Master · B。**

```bash
redis-benchmark --version
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-benchmark` | Redis 的压力测试客户端工具，不是服务端 |
| `--version` | 只显示版本 |

**看到什么再继续：** 显示版本。它不是 make 编译项目生成的，也不需要另外启动 Redis 服务端；它连接你的 kvstore。

### 1.6 手动检查你自己的 standalone.conf

操作：在编辑器打开 config/standalone.conf。全量演示开始前保持以下配置：

```ini
role = standalone
bind_ip = 127.0.0.1
service_port = 29002
log_level = info
persistence_mode = snapshot
```

| 配置项 | 含义 |
| --- | --- |
| `role = standalone` | 单机模式 |
| `bind_ip = 127.0.0.1` | 本机回环地址，仅供本机连接 |
| `service_port = 29002` | 客户端连接的端口 |
| `log_level = info` | 显示启动等信息；若要进一步减少正常日志，可手动设为 error |
| `persistence_mode = snapshot` | 只加载 snapshot.bin；手动 SAVE 时生成全量快照 |

**此时没有启动任何程序。后续改配置前先停服；修改配置不会自动改变正在运行的进程。**

### 1.7 只有重新演示、且旧测试数据不再需要时才清理

首次新 clone 一般没有数据文件，可跳过。若沿用这次练习目录，先停掉该目录的服务，再在 B 手动清理旧单机测试数据：

```bash
rm -f snapshot.bin commands.aof
```

| 参数 | 含义 |
| --- | --- |
| `rm` | 删除文件，不是停止进程 |
| `-f` | 不询问，文件不存在时不报错 |
| `snapshot.bin` | 旧单机快照 |
| `commands.aof` | 旧单机 AOF |

**这是真正删除测试数据的命令。只在开始一轮新演示前使用；宕机与重启之间绝对不要执行。**

## 2. 全量持久化：十万条 → SAVE → 宕机 → 重启 → 校验

### 2.1 启动单机服务

**操作位置：Master · A。**

```bash
./bin/kvstore config/standalone.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/kvstore` | 运行服务端程序；当前窗口保持前台运行 |
| `config/standalone.conf` | 读取你原有的配置文件，不使用额外 demo 配置 |

**看到什么再继续：** 服务启动、没有报错，窗口保持运行。配置此时必须是 snapshot，端口 29002。

### 2.2 确认服务可以响应

**操作位置：Master · B。**

```bash
redis-cli -p 29002 PING
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-cli` | 直接运行客户端 |
| `-p 29002` | 连接端口 29002；未写 -h 时默认连接本机 |
| `PING` | 发送存活检查命令，不修改数据 |

**看到什么再继续：** 回复 PONG。若连接失败，先看 A 的报错，不要继续。

### 2.3 插入十万个不同 key

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 write 100000 0 demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `write` | 写入指定编号范围的不同 key，检查每条写命令的 OK 回复 |
| `100000` | 本次操作 100000 个不同编号 |
| `0` | 从编号 0 开始，到 99999 结束 |
| `demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** 最后显示 PASS op=write expected=100000 passed=100000，错误计数为 0。

只插入数据，不自动 SAVE。每万条显示一次进度；不是打印十万行数据。

### 2.4 你手动发 SAVE

**操作位置：Master · B。**

```bash
redis-cli -p 29002 SAVE
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-cli` | 用现成客户端给 kvstore 发命令 |
| `-p 29002` | 连接本机服务的 29002 端口 |
| `SAVE` | 让 kvstore 将当前内存中的数据写成全量快照 |

**看到什么再继续：** 必须回复 OK。不是运行 C 用例的 save，也不是脚本替你自动保存。

### 2.5 查看生成的文件

**操作位置：Master · B。**

```bash
ls -lh snapshot.bin
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `ls` | 查看文件信息，不读取完整内容 |
| `-l` | 显示权限、所有者、大小和修改时间等详细信息 |
| `-h` | 大小用 K、M、G 等易读单位显示 |
| `snapshot.bin` | 当前目录下的单机快照文件 |

**看到什么再继续：** 看到文件存在且非空。本次 demo 十万条生成约 8.3M；大小不是通过标准，不同内容大小会变。

### 2.6 可选：现场想查看文件里有什么

### 2.6a 看可读文本片段

**操作位置：Master · B。**

```bash
strings snapshot.bin | head -n 20
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `strings snapshot.bin` | 从二进制中提取可打印的文本片段 |
| `|` | 把左侧输出送给右侧命令 |
| `head -n 20` | 只显示前 20 行，避免刷屏 |

**看到什么再继续：** 能看到部分 key/value 文本。

strings 不了解快照记录边界，可能把长度等二进制元数据中的可打印字符、key、value 粘在一起。
它不是完整快照解析器；不能凭这些片段数出十万条记录。

### 2.6b 看文件标识

**操作位置：Master · B。**

```bash
od -An -tc -N8 snapshot.bin
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `od` | 按指定形式查看文件字节 |
| `-An` | 不显示地址偏移 |
| `-tc` | 按字符形式显示 |
| `-N8` | 只读取前 8 个字节 |
| `snapshot.bin` | 要查看的快照文件 |

**看到什么再继续：** 前 8 个字符是 91KVSNAP。此项只说明文件标识，不替代恢复校验。

### 2.7 ① 查 PID

**操作位置：Master · 终端 B。**

```bash
pgrep kvstore
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `pgrep` | 查找正在运行的进程 |
| `kvstore` | 按进程名匹配；输出进程编号 PID |

**看到什么再继续：** 看到当前 kvstore 的 PID。若有多个结果，用下方的 -a -x 命令确认目标，不要猜。

**多个 PID 时才需要：**

```bash
pgrep -a -x kvstore
```

| 参数 | 含义 |
| --- | --- |
| `-a` | 连同完整启动命令一起显示 |
| `-x` | 精确匹配进程名 |
| 如何选择 | 找到命令行中带 config/standalone.conf 的进程 |

### 2.7 ② 手动强制宕机

**操作位置：Master · 终端 B。**

```bash
kill -9 PID
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `kill` | 向进程发送信号 |
| `-9` | 发送 SIGKILL，立即强制终止，进程不能执行退出清理 |
| `PID` | 占位文字！必须换成刚查到的实际数字，例如 kill -9 12345；每次启动 PID 可能不同 |

**看到什么再继续：** 终端 A 显示 Killed，并回到命令提示符。

**这里只终止进程，不删除数据文件。** 保留快照或 AOF，再从同一工作目录重启。SIGKILL 是进程宕机模拟，不是断电测试。

### 2.8 你在同一目录重启

**操作位置：Master · A。**

```bash
./bin/kvstore config/standalone.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/kvstore` | 运行服务端程序；当前窗口保持前台运行 |
| `config/standalone.conf` | 读取你原有的配置文件，不使用额外 demo 配置 |

**看到什么再继续：** 自动加载原来的 snapshot.bin，恢复完成后开始监听。无需手动 LOAD，不要切换目录。

### 2.9 从服务读回并逐条比较十万条

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 verify 100000 0 demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `verify` | 只读数据，与原始 value 逐字节比较 |
| `100000` | 本次操作 100000 个不同编号 |
| `0` | 从编号 0 开始，到 99999 结束 |
| `demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS op=verify expected=100000 passed=100000 missing=0 mismatched=0 errors=0。

verify 不会写入、不会补数据、不会发送 SAVE。

**这一项可以对老师说明：**“我写入十万个不同 key，手动 SAVE，强制宕机后重启；从服务逐条读回，完整 value 全部一致。”

### 2.10 停服，准备切换 AOF

**操作位置：Master · A。按 Ctrl+C。** 这是键盘快捷键，发送中断信号，让当前前台服务停止并返回提示符。
正常切换模式时使用它；前面模拟突然宕机用的是 kill -9。
保留 snapshot.bin，下一项可以展示 AOF 模式不依赖这个文件。

## 3. AOF：十万条 → 不 SAVE → 宕机 → 重启 → 校验

### 3.1 停服后改你自己的配置

在编辑器打开 config/standalone.conf，只把 persistence_mode 从 snapshot 改成 aof，端口仍为 29002：

```ini
persistence_mode = aof
```

**含义：** 这次只回放 commands.aof。demo / aof_demo 这些测试标记不决定恢复模式，配置才决定。

若仅重演本章，且旧 AOF 测试数据不再需要，可在服务停止时执行下面这条；不要在宕机和重启之间执行：

```bash
rm -f commands.aof
```

| 参数 | 含义 |
| --- | --- |
| `rm -f` | 直接删除指定文件；不存在时不报错 |
| `commands.aof` | 上一轮单机 AOF，不是 snapshot.bin |

### 3.2 使用同一启动命令

**操作位置：Master · A。**

```bash
./bin/kvstore config/standalone.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/kvstore` | 运行服务端程序；当前窗口保持前台运行 |
| `config/standalone.conf` | 读取你原有的配置文件，不使用额外 demo 配置 |

**看到什么再继续：** 服务启动。命令相同，配置已变为 aof；不会加载之前的 snapshot.bin。

### 3.3 写入十万条 AOF 演示数据

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 write 100000 0 aof_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `write` | 写入指定编号范围的不同 key，检查每条写命令的 OK 回复 |
| `100000` | 本次操作 100000 个不同编号 |
| `0` | 从编号 0 开始，到 99999 结束 |
| `aof_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS、passed=100000。

AOF 每条成功回复前会同步磁盘，可能比 Snapshot 写入明显慢。
**从这一项开始到校验结束，全程不发送 SAVE。**

### 3.4 展示 AOF 文件

**操作位置：Master · B。**

```bash
ls -lh commands.aof
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `ls -lh` | 显示文件详细信息，大小用易读单位 |
| `commands.aof` | 单机模式的增量写命令日志 |

**看到什么再继续：** 看到非空文件，本次约 11M。没有 SAVE 也会产生，因为写入时就在追加日志。

### 3.5 ① 查 PID

**操作位置：Master · 终端 B。**

```bash
pgrep kvstore
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `pgrep` | 查找正在运行的进程 |
| `kvstore` | 按进程名匹配；输出进程编号 PID |

**看到什么再继续：** 看到当前 kvstore 的 PID。若有多个结果，用下方的 -a -x 命令确认目标，不要猜。

**多个 PID 时才需要：**

```bash
pgrep -a -x kvstore
```

| 参数 | 含义 |
| --- | --- |
| `-a` | 连同完整启动命令一起显示 |
| `-x` | 精确匹配进程名 |
| 如何选择 | 找到命令行中带 config/standalone.conf 的进程 |

### 3.5 ② 手动强制宕机

**操作位置：Master · 终端 B。**

```bash
kill -9 PID
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `kill` | 向进程发送信号 |
| `-9` | 发送 SIGKILL，立即强制终止，进程不能执行退出清理 |
| `PID` | 占位文字！必须换成刚查到的实际数字，例如 kill -9 12345；每次启动 PID 可能不同 |

**看到什么再继续：** 终端 A 显示 Killed，并回到命令提示符。

**这里只终止进程，不删除数据文件。** 保留快照或 AOF，再从同一工作目录重启。SIGKILL 是进程宕机模拟，不是断电测试。

### 3.6 保持 aof 配置，手动重启

**操作位置：Master · A。**

```bash
./bin/kvstore config/standalone.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/kvstore` | 运行服务端程序；当前窗口保持前台运行 |
| `config/standalone.conf` | 读取你原有的配置文件，不使用额外 demo 配置 |

**看到什么再继续：** 自动回放 commands.aof，恢复完成后监听。不要改回 snapshot，不要删除日志。

### 3.7 校验这一轮十万条

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 verify 100000 0 aof_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `verify` | 只读数据，与原始 value 逐字节比较 |
| `100000` | 本次操作 100000 个不同编号 |
| `0` | 从编号 0 开始，到 99999 结束 |
| `aof_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS、passed=100000、missing=0、mismatched=0、errors=0。

**这一项可以对老师说明：**“全程未发 SAVE，写完十万条后强制宕机；只靠 AOF 回放恢复，完整内容一致。”

全量与增量已分别使用独立模式验证，不是混合恢复。

### 3.8 正常停服

在 Master · A 按 Ctrl+C。停止后再修改配置。

## 4. 一个功能一个万级 C 用例

### 4.1 配置改回 Snapshot，再启动

停服后，将 config/standalone.conf 改回 persistence_mode = snapshot，其他项不动。
会加载之前的 demo 快照；本章用 basic_demo 生成另一批 key，不与 demo 重复。

### 4.1a 启动服务

**操作位置：Master · A。**

```bash
./bin/kvstore config/standalone.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/kvstore` | 运行服务端程序；当前窗口保持前台运行 |
| `config/standalone.conf` | 读取你原有的配置文件，不使用额外 demo 配置 |

**看到什么再继续：** 正常启动。服务保持运行到 benchmark 和流水线演示结束，不需要 SAVE。

### 4.2 写入一万个功能测试 key

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 write 10000 0 basic_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `write` | 写入指定编号范围的不同 key，检查每条写命令的 OK 回复 |
| `10000` | 本次操作 10000 个不同编号 |
| `0` | 从编号 0 开始，到 9999 结束 |
| `basic_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS、passed=10000。

这是为下面的修改、删除准备数据。原始值读取校验此前已用十万条展示过，这里不再重复 verify。

### 4.3 先看清楚修改的内容

例如编号 0，key 始终为 manual basic_demo hash 00000000；只是 value 中的 original 变成 modified。

```text
修改前：
value basic_demo 00000000 original
second line \"quoted\"

修改后：
value basic_demo 00000000 modified
second line \"quoted\"
```

### 4.4 修改用例

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 modify 10000 0 basic_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `modify` | 只修改已有 key，把 value 的 original 改为 modified |
| `10000` | 本次操作 10000 个不同编号 |
| `0` | 从编号 0 开始，到 9999 结束 |
| `basic_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS、passed=10000，说明一万条修改命令都回复成功。

它发送 HMOD，只负责修改和检查 OK 回复。实际内容是否全部正确，下一步单独读取校验。

### 4.5 单独读取并核对修改结果

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 verify-modified 10000 0 basic_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `verify-modified` | 只读数据，与修改后的完整预期 value 逐字节比较 |
| `10000` | 本次操作 10000 个不同编号 |
| `0` | 从编号 0 开始，到 9999 结束 |
| `basic_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS、passed=10000，说明读回的完整 value 全部等于修改后预期。

verify 预期 original；verify-modified 预期 modified。二者都只读不改。
这时如果用原来的 verify，会因为预期值不同而报告不一致。

### 4.6 存在性用例

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 exists 10000 0 basic_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `exists` | 检查 key 存在，每条存在性回复应为整数 1 |
| `10000` | 本次操作 10000 个不同编号 |
| `0` | 从编号 0 开始，到 9999 结束 |
| `basic_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS，表示一万个 HEXIST 回复均为整数 1。

只检查是否存在，不读取完整 value，不修改数据。

### 4.7 删除用例

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 delete 10000 0 basic_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `delete` | 删除 key，每条删除回复必须为整数 1 |
| `10000` | 本次操作 10000 个不同编号 |
| `0` | 从编号 0 开始，到 9999 结束 |
| `basic_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS，表示每条 HDEL 都实际删除了一个 key。

“回复 0，原本就不存在”不算本用例通过。

### 4.8 删除后读取用例

**操作位置：Master · B。**

```bash
./bin/testcase 127.0.0.1 29002 missing 10000 0 basic_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `missing` | 读取 key，要求每条都为空值 |
| `10000` | 本次操作 10000 个不同编号 |
| `0` | 从编号 0 开始，到 9999 结束 |
| `basic_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS，表示一万次 HGET 全部返回空值，确认这些 key 已经读不到。

这里 missing 是用例名称，预期就是 key 不存在；不要和 verify 汇总里的 missing（校验时发现缺失的错误计数）混淆。
不存在时的 HEXIST 返回 0 可用 absent 额外覆盖；本次现场已选择不重复演示，附录给出命令。

## 5. redis-cli 与 hiredis：客户端兼容及特殊字符

本章继续使用正在运行的 29002 服务，不要停服或切换目录。

### 5.1 用 redis-cli 写入带空格和 CRLF 的数据

**操作位置：Master · B。**

```bash
redis-cli -p 29002 HSET 'special key' $'line one\r\nline two'
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-cli` | 现成的命令行客户端 |
| `-p 29002` | 连接本机 29002 端口 |
| `HSET` | 本项目的 Hash 引擎写入命令 |
| `'special key'` | 整个带空格字符串是一个 key；引号避免被 shell 分成两个参数 |
| `$'line one\r\nline two'` | Bash 将反斜杠转义转换为真实 CRLF，作为一个完整 value 发送 |

**看到什么再继续：** 回复 OK。special key 就是在这里写入的，不是自动挂载，也不是程序预置的数据。

### 5.2 直接读取特殊字符

**操作位置：Master · B。**

```bash
redis-cli -p 29002 --no-raw HGET 'special key'
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-cli -p 29002` | 连接当前 kvstore |
| `--no-raw` | 以可读的转义形式显示回复，方便看清换行 |
| `HGET` | Hash 引擎读取命令 |
| `'special key'` | 刚才实际写入的完整 key |

**看到什么再继续：** 显示 "line one\r\nline two"。带空格的 key 与带 CRLF 的 value 均能正常传输。

### 5.3 先分清 hiredis 和 test_hiredis

| 名称 | 是什么 | 作用 |
| --- | --- | --- |
| redis-cli | 可直接运行的命令行客户端 | 你手动输入命令，它发给 kvstore |
| hiredis | C 语言客户端库 | 提供 redisConnect、redisCommand 等连接、发送和解析函数 |
| tests/test_hiredis.c | 仓库里的 C 测试代码 | 调用 hiredis，安排各项操作并判断实际回复 |
| bin/test_hiredis | 测试代码编译后的程序 | 你手动运行它，连接已经启动的 kvstore |

这里没有另外启动 Redis 服务端。hiredis 是库，test_hiredis 是使用这个库的程序。

### 5.4 编译 hiredis C 客户端

**操作位置：Master · B。**

```bash
cc tests/test_hiredis.c $(pkg-config --cflags --libs hiredis) -o bin/test_hiredis
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `cc` | C 编译器 |
| `tests/test_hiredis.c` | 要编译的源文件，已加中文注释 |
| `$(...)` | shell 先执行括号内命令，把它的输出填回本条命令 |
| `pkg-config` | 查询已安装库的编译、链接配置 |
| `--cflags` | 查询头文件等编译参数 |
| `--libs` | 查询链接库参数 |
| `hiredis` | 查询的库名称 |
| `-o bin/test_hiredis` | 输出程序保存到 bin/test_hiredis |

**看到什么再继续：** 返回提示符、没有编译错误。只编译，不连接 kvstore。换机器需要先安装 hiredis 开发依赖及 pkg-config。

### 5.5 手动运行 hiredis 客户端

**操作位置：Master · B。**

```bash
./bin/test_hiredis 29002
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/test_hiredis` | 运行刚编译的 C 客户端测试程序 |
| `29002` | 连接本机 kvstore 的端口 |

**看到什么再继续：** 出现下面 8 个 PASS；程序不负责服务启停，不发送 SAVE。

| 输出 | 实际做了什么 |
| --- | --- |
| PING | 要求返回 PONG |
| SET | 用独立测试 key 写入 a b + CRLF + c |
| GET with spaces and CRLF | 读回完整 value，逐字节比较 |
| duplicate SET overwrites | 同一个 key 再写 other，检查 OK |
| GET overwritten value | 读取确认内容确实是 other |
| DEL | 删除它自己的测试 key，要求整数 1 |
| missing GET | 删除后读取，要求空值 NIL |
| unknown command | 发送 BOGUS，要求正确返回 ERR unknown command；不是支持该命令 |

它的 key 是 hiredis-probe-加进程编号。正常完成前会删除自己的 key，不会删除前面 Hash 中的 special key。
每个 PASS 都先收到实际网络回复并检查类型、文本或整数值；并不是只打印固定成功文字。

## 6. redis-benchmark 与 pipeline 批量处理

继续使用 29002 服务，所有命令在 B 执行。当前机器已经装好 redis-benchmark 及运行依赖。
PING 不改数据；HGET 只读已有的 special key；不要在本章中途重启 Snapshot 服务，因为 special key 尚未 SAVE。

### 6.1 一万次 PING

**操作位置：Master · B。**

```bash
redis-benchmark -h 127.0.0.1 -p 29002 -n 10000 -c 1 PING
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-benchmark` | 现成的压力测试客户端，不是 Redis 服务端 |
| `-h 127.0.0.1` | 连接本机 IP |
| `-p 29002` | 连接 kvstore 端口 |
| `-n 10000` | 总共发一万次请求 |
| `-c 1` | 使用一个并发客户端连接 |
| `PING` | 本次具体命令；这里显式指定 RESP PING |

**看到什么再继续：** 出现 10000 requests completed，并显示 throughput 与 latency 汇总。

### 6.2 一万次 HGET

**操作位置：Master · B。**

```bash
redis-benchmark -h 127.0.0.1 -p 29002 -n 10000 -c 1 HGET 'special key'
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-benchmark` | 现成的压力测试客户端，不是 Redis 服务端 |
| `-h 127.0.0.1` | 连接本机 IP |
| `-p 29002` | 连接 kvstore 端口 |
| `-n 10000` | 总共发一万次请求 |
| `-c 1` | 使用一个并发客户端连接 |
| `HGET` | 本次具体命令，读取 Hash 引擎 |
| `'special key'` | 第 5.1 步写入的 key，包含空格，作为一个参数传入 |

**看到什么再继续：** 完成 10000 requests。

HGET 不存在的 key 通常也是合法的空值回复，因此完成请求数不等于内容校验通过。这里沿用前面已亲自写入、读出的 special key，不重复加额外抽查步骤。

### 6.3 每批 16 个 PING，展示流水线

**操作位置：Master · B。**

```bash
redis-benchmark -h 127.0.0.1 -p 29002 -n 10000 -c 1 -P 16 PING
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `redis-benchmark` | 现成的压力测试客户端，不是 Redis 服务端 |
| `-h 127.0.0.1` | 连接本机 IP |
| `-p 29002` | 连接 kvstore 端口 |
| `-n 10000` | 总共发一万次请求 |
| `-c 1` | 使用一个并发客户端连接 |
| `-P 16` | pipeline 深度 16：连续发出一批 16 个请求后收取回复，不必每发一个就等一个 |
| `PING` | 每个请求都是 PING |

**看到什么再继续：** 一万次请求完成。总量仍为 10000，不是只有 16 次；本次为 625 批。

### 6.4 怎样读 benchmark 输出

| 输出 | 含义 |
| --- | --- |
| 10000 requests completed | 工具完成的一万次请求；不是插入了一万个不同 key |
| 1 parallel clients | 一个客户端连接 |
| throughput summary | 请求吞吐，单位 requests per second |
| avg / min / max | 平均、最小、最大延迟，表中单位 msec |
| p50 / p95 / p99 | 有相应比例的延迟样本不超过该值 |
| Could not fetch server CONFIG | 工具尝试读取 Redis CONFIG，但 kvstore 不支持这一配置查询；本次 PING/HGET 仍已完成 |

本次现场记录：普通 PING 约 2.98 万次/秒，HGET 约 3.01 万次/秒，P16 PING 约 27.03 万次/秒。
这些仅为本次环境观察，不能作为下一轮必须达到的固定指标。

目前保证的是已经展示的兼容场景：默认 redis-benchmark -t ping 的 inline 请求、redis-cli --pipe 的结束探测尚不兼容，不作为本手册命令。
项目 HSET 是选择 Hash 引擎，并非 Redis 的 hash 字段语义。key/value 不支持嵌入 NUL。

### 6.5 结束单机服务

在 A 按 Ctrl+C。接下来要在第一台启动 Master、第二台启动 Slave。

## 7. 双机演示前：地址、版本、构建和配置

### 7.1 打开第二台机器的终端

**操作位置：新窗口，或尚未运行测试的 B。**

```bash
ssh jaspersao@172.16.145.136
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `ssh` | 远程登录另一台机器 |
| `jaspersao` | 第二台机器的登录用户名 |
| `@172.16.145.136` | 第二台机器的 IP |

**看到什么再继续：** 登录成功，得到第二台 Linux 的命令提示符。它就是窗口 C。需要密码时输入第二台 Linux 的密码，输入字符不显示属于正常情况。

两台机器可能都叫 ubuntu-dev，不能只看主机名认机器。第一台不能绑定第二台的 IP；此前 Cannot assign requested address 就是在 Master 上误启动 Slave 导致的。

### 7.2 第二台也要进入自己的仓库

**操作位置：Slave · C。**

```bash
cd ~/dev/91kvstore
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `cd` | 切换目录 |
| `~/dev/91kvstore` | 第二台本地的仓库目录；不共享第一台的工作区，第一台编辑不会自动同步过来 |

**看到什么再继续：** 位于第二台自己的仓库根目录。

### 7.3 两台各查一次本机 IP

**操作位置：Master · B、Slave · C 分别执行。**

```bash
hostname -I
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `hostname` | 查询本机主机相关信息 |
| `-I` | 大写 I，列出本机配置的 IP 地址 |

**看到什么再继续：** Master 包含 172.16.145.135，Slave 包含 172.16.145.136。若不同，按现场实际地址修改后续所有配置和连接参数。

### 7.4 启动前核对两端源码版本

**操作位置：Master · B、Slave · C 分别执行。**

```bash
git rev-parse --short HEAD
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `git` | 查询当前仓库 |
| `rev-parse` | 解析版本引用 |
| `--short` | 显示简短提交号 |
| `HEAD` | 当前检出的提交 |

**看到什么再继续：** 两个提交号相同。本轮成功版本是 ea5c363；将来更新后可以是另一个版本，但两端必须一致。

提交号相同还需要确保程序已重新编译。我们曾遇到 Master=ea5c363、Slave=207f4ef：新版全量走 RDMA，旧版只认识旧 TCP 全量协议，导致十万条全部缺失。
本步骤放在演示前做，不要等数据校验失败才检查。

### 7.5 只有版本落后时才更新，先停止那台服务

### 7.5a 更新并强制重编译

**操作位置：版本落后的机器，服务已停止。**

```bash
git pull --ff-only && make -B all
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `git pull` | 从当前分支的远端获取并合入更新 |
| `--ff-only` | 只允许快进更新，不自动创建合并提交 |
| `&&` | 前一条成功才运行后一条 |
| `make -B all` | -B 强制重新构建，all 构建 bin/kvstore，避免仍运行旧程序 |

**看到什么再继续：** 更新与编译都成功，再查提交号确认与另一台相同。若有 error，不要继续启动。

### 7.5b 若更新被 replica.conf 的本地修改拦住

**操作位置：Slave · C，仅在出现同样冲突时执行。**

```bash
git stash push -m "keep-replica-config" -- config/replica.conf && git pull --ff-only && git stash pop && make -B all
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `git stash push` | 临时保存本地修改 |
| `-m "keep-replica-config"` | 给这次暂存取个可辨认的名字 |
| `-- config/replica.conf` | 只暂存这个配置文件，不把其他文件一并暂存 |
| `git pull --ff-only` | 更新代码，只允许快进 |
| `git stash pop` | 把刚暂存的配置改动恢复回来；出现冲突则先处理，不能当已完成 |
| `&&` | 任意一步失败，后续命令不执行 |
| `make -B all` | 配置恢复成功后强制重新编译服务端 |

**看到什么再继续：** 全部完成，配置仍是你的本机地址。若出现 CONFLICT，先处理冲突，再继续；不要用强制覆盖丢掉自己的修改。

这条是本次遇到“仅 replica.conf 阻止更新”时的处理命令；若报错列出了其他文件，需要先另行保留那些修改。

### 7.6 两台确认 RDMA 设备可用

**操作位置：Master · B、Slave · C 分别执行。**

```bash
rdma link show
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `rdma` | 查看 Linux RDMA 设备 |
| `link show` | 列出链路和状态 |

**看到什么再继续：** 对应 iWARP 链路是 ACTIVE，网卡地址与配置对应。本次第一台为 siw0。

当前项目全量快照使用 RDMA；只有两台能互相 SSH 或 TCP 能连通还不够。设备不可用时先处理环境，不能把未同步当已完成。

### 7.7 查看你自己的 Master 配置

**操作位置：Master · B。**

```bash
cat config/primary.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `cat` | 显示文件内容，不修改文件 |
| `config/primary.conf` | 第一台机器的主节点配置 |

**看到什么再继续：** 核对下面这些值；需要改时用编辑器修改原文件。

```ini
role = primary
bind_ip = 172.16.145.135
service_port = 29000
rdma_port = 29747
log_level = info
persistence_mode = legacy_hybrid
```

| 配置项 | 含义 |
| --- | --- |
| role=primary | 第一台作为 Master |
| bind_ip | 必须是 Master 自己拥有的地址 |
| service_port=29000 | 客户端、复制控制与 TCP AOF 增量所用端口 |
| rdma_port=29747 | 全量快照使用的独立 RDMA 端口 |
| log_level=info | 保留同步阶段日志，方便现场确认 |
| persistence_mode=legacy_hybrid | 当前主从要求的模式，使用二进制快照和 AOF，不代表恢复旧文本快照 |

### 7.8 查看第二台自己的 Slave 配置

**操作位置：Slave · C。**

```bash
cat config/replica.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `cat` | 显示文件内容 |
| `config/replica.conf` | 第二台本地的配置，不是第一台同名文件 |

**看到什么再继续：** 核对下面这些实际配置项。文件顶部旧注释中的 29100 不决定端口，实际以 primary_port 为准。

```ini
role = replica
bind_ip = 172.16.145.136
service_port = 29001
primary_host = 172.16.145.135
primary_port = 29000
log_level = info
persistence_mode = legacy_hybrid
```

| 配置项 | 含义 |
| --- | --- |
| role=replica | 第二台作为 Slave |
| bind_ip=172.16.145.136 | Slave 自己的本机地址 |
| service_port=29001 | 之后从 Slave 读取数据时连接的端口 |
| primary_host=172.16.145.135 | 连接的 Master 地址 |
| primary_port=29000 | 连接 Master 的 service_port，不是 Slave 自己的端口 |
| log_level=info | 观察 FULL_SYNC、CATCH_UP、ONLINE 日志 |
| persistence_mode=legacy_hybrid | 主从模式要求；Slave 的 RDMA 端口由 Master 通知，不另配 rdma_port |

## 8. 双机正式顺序：五万 → Slave ONLINE → 再五万 → 校验十万

**起点：两个服务都停止，两台版本已一致并重新编译，配置已核对。**
如果版本修好时 Master 已有十万条，再启动 Slave 并校验通过，只证明现有十万条的全量同步。
下面从空数据重来，明确把后五万条放在 Slave ONLINE 以后，才完整展示老师要求的过程。

### 8.1 清除上一轮主从测试数据

**操作位置：Master、Slave 各自仓库根目录分别执行；两端服务都必须停止。**

```bash
rm -f snapshot.db appendonly.aof
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `rm` | 删除指定文件；本次你已选择旧主从测试数据不用备份 |
| `-f` | 文件不存在时不报错 |
| `snapshot.db` | 主从模式的二进制快照；有时 Master 尚未生成，所以不存在是正常的 |
| `appendonly.aof` | 主从模式的 AOF；必须和配套快照一起清理，避免恢复上一轮数据 |

**看到什么再继续：** 命令无错误，两台旧主从数据已清除。

这条不删除单机的 snapshot.bin / commands.aof，它们也不会被 legacy_hybrid 加载。
**只在新一轮开始前清理；本轮服务启动后不要再删。**

### 8.2 只启动 Master

**操作位置：Master · A。**

```bash
./bin/kvstore config/primary.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/kvstore` | 运行服务端程序；当前窗口保持前台运行 |
| `config/primary.conf` | 读取你原有的配置文件，不使用额外 demo 配置 |

**看到什么再继续：** 启动成功。Slave 此时仍保持停止。

### 8.3 写入前五万条

**操作位置：Master · B。**

```bash
./bin/testcase 172.16.145.135 29000 write 50000 0 repl_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `172.16.145.135` | 连接第一台 Master |
| `29000` | 目标服务端口，必须与对应配置一致 |
| `write` | 写入指定编号范围的不同 key，检查每条写命令的 OK 回复 |
| `50000` | 本次操作 50000 个不同编号 |
| `0` | 从编号 0 开始，到 49999 结束 |
| `repl_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS、passed=50000。这批范围是 0～49999，此时 Slave 还没有启动。

### 8.4 到第二台启动 Slave

**操作位置：Slave · C。**

```bash
./bin/kvstore config/replica.conf
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/kvstore` | 运行服务端程序；当前窗口保持前台运行 |
| `config/replica.conf` | 读取你原有的配置文件，不使用额外 demo 配置 |

**看到什么再继续：** 连接第一台 Master，随后出现完整的同步阶段日志。**先别写第二批。**

### 8.5 你观察 Slave 日志，必须等 ONLINE

```text
replication: enter FULL_SYNC ...
replication: Replica RDMA connection established
replication: Snapshot installed, enter CATCH_UP ...
replication: Replica RDMA closed, resume TCP catch-up
replication: enter ONLINE ...
```

| 日志 | 证明什么 |
| --- | --- |
| connect to primary | 开始连接 Master；单独这一行不能证明同步成功 |
| FULL_SYNC | 开始全量同步已有的前五万条 |
| RDMA connection established | 快照传输的 RDMA 连接已建立 |
| Snapshot installed | 快照已安装 |
| CATCH_UP | 追赶快照边界之后的 AOF |
| ONLINE | 追赶结束，进入正常持续复制状态 |

**只有看到 ONLINE 才执行下一步。** 如果停在连接提示、出现错误或反复断开，先处理，不继续写第二批。

### 8.6 Slave ONLINE 后，向 Master 再写后五万条

**操作位置：Master · B。**

```bash
./bin/testcase 172.16.145.135 29000 write 50000 50000 repl_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `172.16.145.135` | 连接第一台 Master |
| `29000` | 目标服务端口，必须与对应配置一致 |
| `write` | 写入指定编号范围的不同 key，检查每条写命令的 OK 回复 |
| `50000` | 本次操作 50000 个不同编号 |
| `50000` | 从编号 50000 开始，到 99999 结束 |
| `repl_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS、passed=50000。这次是编号 50000～99999，与前一批不同。

命令里两个 50000 作用不同：第一个是数量，第二个是起始编号。
这一批是在 Slave 已正常运行后新增，用于展示 ONLINE 后的增量同步。

### 8.7 从第二台 Slave 校验完整十万条

**操作位置：Master · B。**

```bash
./bin/testcase 172.16.145.136 29001 verify 100000 0 repl_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `172.16.145.136` | 连接第二台 Slave；读取的是副本上的数据 |
| `29001` | 目标服务端口，必须与对应配置一致 |
| `verify` | 只读数据，与原始 value 逐字节比较 |
| `100000` | 本次操作 100000 个不同编号 |
| `0` | 从编号 0 开始，到 99999 结束 |
| `repl_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

**看到什么再继续：** PASS op=verify expected=100000 passed=100000 missing=0 mismatched=0 errors=0。

读取地址必须是 Slave 的 172.16.145.136:29001，不能填回 Master。
这条连接第二台读数据，逐条核对两批完整 value；本次跨机校验约 43 秒，仅供参考。
复制是异步的，若第二批尚未追上，先看日志，追赶完成后手动重试，失败不能当通过。

### 8.8 这一轮可以怎样向老师说明

“Master 从空数据开始，先写五万条；我到第二台启动 Slave，看到 ONLINE 后，再给 Master 写后五万条。
最后从 Slave 读完整十万条，全部 value 一致，无缺失、无错误。
这展示了初始全量同步，以及 Slave 正常运行后的增量同步。”

演示结束后，在 A 和 C 分别按 Ctrl+C 停服。

## 9. 用例里的数据、代码和结果到底是什么

### 9.1 key 的命名方式，不是数据库新字段

```text
manual demo hash 00078888
```

| 部分 | 含义 |
| --- | --- |
| manual | 测试程序加的固定文字，英文含义是手动；kvstore 没有特殊处理 |
| demo / aof_demo / basic_demo / repl_demo | 测试标记，用于区分不同演示生成的 key |
| hash | 测试选择的引擎名称，拼在 key 里仅用于辨认 |
| 00078888 | 数据编号，至少 8 位，不足补 0 |

**对 kvstore 来说，整个带空格的字符串才是一个普通 key，不会把这些单词拆成数据库字段。**
value 也是程序生成的具体字符串，含标记、编号、original 或 modified，以及 CRLF 等特殊字符。
测试程序没有改 Snapshot 格式，也没有给快照增加“数据集字段”。

### 9.2 Snapshot 和 AOF 文件是什么

| 模式 | 启动时读取 / 使用的文件 | 内容 |
| --- | --- | --- |
| 单机 snapshot | snapshot.bin | 自定义二进制快照，保存当时的完整 key/value 和引擎等信息 |
| 单机 aof | commands.aof | RESP 风格的长度前缀写命令流，重启时回放 |
| 主从 legacy_hybrid | snapshot.db + appendonly.aof | 带 AOF 检查点的二进制快照与配套日志 |

Snapshot 的记录包含引擎类型、key/value 长度和内容，文件带版本、记录数、校验等信息。
.db 只是扩展名，不表示 SQLite；.bin 也不是另一种数据集字段。
真正由 persistence_mode 选择恢复方式，而不是由测试标记 demo / aof_demo 选择。

### 9.3 打开 tests/testcase.c，按这条线读中文注释

| 函数 | 看什么 |
| --- | --- |
| main | 从命令参数取得操作、数量、起始编号、测试标记与引擎，选中一个用例 |
| run_testcase | 从 start 循环到 start+count-1；每次只执行选中的一个用例 |
| make_record | 同一个编号生成相同的 key 和预期 value；不同编号得到不同 key |
| testcase_write | 真实发送 SET（Hash 下为 HSET），检查实际收到的 OK |
| testcase_verify | 真实发送 GET（Hash 下为 HGET），发现 nil 记缺失，否则比较完整预期值 |
| text_equals | 回复类型、长度和 memcmp 逐字节比较，必须全部符合 |
| request / command / read_reply | 拼引擎前缀、编码 RESP、通过 socket 发送并读取真实回复 |

```c
for (long i = start; i < start + count; ++i) {
    case_result_t result = entry->run(test, i);
    // 只有这条实际检查返回 CASE_PASS，passed 才加 1。
}
```

start=0、count=100000 时，编号是 0～99999。不是重复十万次访问同一个 key。

读取校验中的关键比较：

```c
reply->type == type &&
reply->length == strlen(expected) &&
!memcmp(reply->data, expected, reply->length)
```

expected 是客户端按写入时相同规则重新生成的预期 value；reply 是通过网络从 kvstore 读回的实际值。
长度和每个字节都一致才通过。即使服务器回复了一个非空但错误的 value，也不会被当作通过。

### 9.4 汇总输出如何解释

| 输出 | 含义 |
| --- | --- |
| BEGIN | 显示本次实际选择的目标地址、端口、引擎、测试标记、编号和数量 |
| progress | 已处理的条数和通过数；每万条显示一次，避免无效刷屏 |
| expected | 本次要求处理的总数 |
| passed | 该用例按自身预期检查通过的条数 |
| missing | verify / verify-modified 读取时发现 key 缺失的数量 |
| mismatched | 回复或完整内容不符合当前用例预期的数量 |
| errors | 连接、协议或服务端错误数 |
| seconds | 客户端这轮执行用时 |
| PASS / FAIL | 全部指定条数通过且无错误才 PASS；程序对应返回 0 / 非 0 |

### 9.5 NtyCo、io_uring、mmap 的源码证据

| 要求 | 打开的源文件 | 说明 |
| --- | --- | --- |
| NtyCo 网络 | src/network/ntyco.c | 服务监听、连接、协程网络处理 |
| Snapshot 落盘 | src/persistence/snapshot_store.c | 保存时调用 uring 写入、同步并安装快照 |
| AOF 落盘 | src/persistence/aof.c | 追加写命令并进行 uring 同步 |
| io_uring 调用 | src/persistence/uring_io.c | io_uring_prep_write 等操作 |
| mmap 加载 | src/persistence/mapped_file.c | mmap / munmap；Snapshot 加载、AOF 回放复用此接口 |

这类实现要求应打开源码说明，不能只凭测试输出 PASS 证明。
Replica 接收临时快照目前仍使用 write/fsync，因此不要说所有复制落盘都通过 io_uring。

## 10. 可选补充：其他用例与现场故障处理

### 10.1 absent 与 missing 的区别（本次正式流程已跳过）

```bash
./bin/testcase 127.0.0.1 29002 absent 10000 0 basic_demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `absent` | 检查 key 不存在，每条存在性回复应为整数 0 |
| `10000` | 本次操作 10000 个不同编号 |
| `0` | 从编号 0 开始，到 9999 结束 |
| `basic_demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

missing 检查 HGET 返回空值；absent 检查 HEXIST 返回整数 0。
这覆盖另一个命令分支；若老师不要求逐一展示所有不存在分支，可以保持跳过。

### 10.2 其他存储引擎

```bash
./bin/testcase 127.0.0.1 29002 write 10000 0 tree_demo rbtree
```

| 参数 | 含义 |
| --- | --- |
| ./bin/testcase、127.0.0.1、29002 | 运行客户端并连接单机服务 |
| write、10000、0 | 写一万个不同编号，范围 0～9999 |
| tree_demo | 本轮 key/value 的测试标记 |
| rbtree | 新增的最后一个可选参数，选择 RBTree，实际发送 RSET；也可填 skiplist 或 array |

后续读取、修改、删除同一批时，引擎参数也必须一致。
**本次主流程默认 Hash，不能据此声称四种引擎全部通过同样演示。**
Array 当前容量只有 1024，不能同时存一万个不同 key；可在容量内演示，例如 1000 条。
若老师要求每种引擎均达到万级容量，这项仍有容量限制需要另行解决。

### 10.3 可选：证明校验器确实能发现缺失（只读，不改数据）

仅当单机 Snapshot 模式仍在运行，且 demo 只写了编号 0～99999 时，可以执行：

```bash
./bin/testcase 127.0.0.1 29002 verify 1 100000 demo
```

| 命令 / 参数 | 含义 |
| --- | --- |
| `./bin/testcase` | 运行 C 测试客户端，不负责服务启停 |
| `127.0.0.1` | 连接本机正在运行的 kvstore |
| `29002` | 目标服务端口，必须与对应配置一致 |
| `verify` | 只读数据，与原始 value 逐字节比较 |
| `1` | 本次操作 1 个不同编号 |
| `100000` | 从编号 100000 开始，到 100000 结束 |
| `demo` | 测试标记，拼进 key/value；写入、校验时必须一致，不是持久化模式 |
| 未填写引擎参数 | 默认 hash，实际发送 HSET/HGET/HMOD/HEXIST/HDEL 等命令 |

预期 FAIL、missing=1，因为编号 100000 没有写入。
这是有意设计的反向检查，不是本轮恢复失败。它只读，不会修改已有十万条数据。

### 10.4 常见问题速查

| 现象 | 含义和下一步 |
| --- | --- |
| make 找不到目标或 bin 不存在 | 确认在仓库根目录；新 clone 的版本是否包含本次 C 用例修改；先 make all testcase |
| Cannot assign requested address | bind_ip 不是当前机器本机地址；hostname -I 核对，确认没有在 Master 上启动 Slave 配置 |
| 连接被拒绝 / PING 失败 | 检查服务窗口是否启动成功，以及 IP、端口是否与当前配置一致 |
| 进程找不到 / PID 变了 | 每次启动都可能换 PID；重新 pgrep，不照抄上次数字 |
| SAVE 不生成 .db | 单机 snapshot 生成 snapshot.bin；主从才用 snapshot.db |
| 重启后数据全没了 | 是否换了工作目录、改错模式或误删文件；是否在 Snapshot 模式下忘记 SAVE |
| verify 全部 missing | 先确认目标地址、端口、标记、引擎；双机再检查版本、编译产物、FULL_SYNC/ONLINE 日志 |
| 只有 connect to primary | 不能当同步成功；应继续看到 FULL_SYNC、Snapshot installed、ONLINE |
| Master、Slave 提交号不同 | 停服后先统一版本、重新编译；旧 TCP 全量与新 RDMA 全量不兼容 |
| git pull 被 replica.conf 拦住 | 用 7.5b 暂存并恢复配置；出现 CONFLICT 时先处理，不跳过 |
| Slave 报 LOADING | 正在全量同步或追赶，等待 ONLINE，不要改成读取 Master 来假装通过 |
| strings 中 key/value 粘在一起 | 二进制结构被按可打印字符抽取，不是快照突然加了新字段 |
| benchmark 报 CONFIG 警告 | 当前 CONFIG 查询不支持；以指定 PING/HGET 是否实际完成为准 |
| 同名 special key 不存在 | 它来自 5.1 的 HSET；Snapshot 模式没 SAVE 就重启会丢失，先按原命令写入 |
| Ctrl+C 与 kill -9 | Ctrl+C 用于正常结束当前前台演示；强制宕机演示使用 kill -9；两者都不等于删除数据文件 |

### 10.5 现场讲解时最短的检查清单

| 演示项 | 现场必须看到 |
| --- | --- |
| Snapshot | write 十万 PASS → SAVE OK → 文件生成 → kill -9 → 同目录重启 → verify 十万 PASS |
| AOF | write 十万 PASS → AOF 非空 → 全程不 SAVE → kill -9 → 同模式重启 → verify 十万 PASS |
| 修改 | modify 万条 PASS → verify-modified 万条 PASS |
| 存在 / 删除 | exists 万条 PASS → delete 万条 PASS → missing 万条 PASS |
| 特殊字符 | 亲自 HSET → HGET 读出空格与 CRLF |
| hiredis | 编译 C 客户端 → 手动运行 → 8 项实际回复检查通过 |
| benchmark | 显式 PING / HGET 完成一万次；-P 16 流水线完成一万次 |
| 双机 | 空数据 → Master 前五万 → Slave ONLINE → Master 后五万 → 从 Slave 校验十万 PASS |
