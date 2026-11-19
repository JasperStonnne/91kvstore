# 性能对照工具与正确性预检

这些程序只增加测试入口，不修改服务端业务命令、持久化或复制逻辑。正式性能结果需要另行运行矩阵；下面的小负载结果只用于确认工具可用。

## 构建与外部工具

在 Linux 仓库根目录构建。为防止沿用旧优化级别的对象文件，第一次统一使用 `-B`：

```sh
make -B -j4 CFLAGS='-std=gnu11 -Wall -Wextra -O2 -g' \
  FLAG="-lpthread -O2 -g -ldl -I$PWD/third_party/NtyCo/core" \
  all testcase ebpf-capture benchmark-tools
```

135 的外部工具放在 `/home/jaspersao/dev/91kvstore-tools/bin`，不依赖修改系统服务：

- `redis-server`、`redis-cli`、`redis-benchmark`：Redis 7.0.15，源码使用 `OPTIMIZATION=-O2 MALLOC=jemalloc` 构建。
- `iperf3`：3.16，用户目录内的包装脚本加载随包解出的共享库；136 相同位置也可用。
- 下载文件、SHA-256 记录和构建日志保留在上述工具目录。下载后记录的哈希用于复现，不表示已与独立可信来源校验。

正式 Redis AOF 对照应显式配置 `appendonly yes`、`appendfsync always`，关闭自动 rewrite；关闭 AOF 对照使用 `appendonly no`，两者均关闭周期 RDB。不要将默认 `everysec` 的成绩混入同耐久性对比。

## ECHO 网络基线

```sh
bin/benchmark_echo 127.0.0.1 6390
# 另一终端；正式执行需记录轮次、请求量、返回错误和完整输出。
/home/jaspersao/dev/91kvstore-tools/bin/redis-benchmark \
  -h 127.0.0.1 -p 6390 -c 1 -P 1 -n 100000 ECHO payload
```

独立 ECHO 程序复用项目 NtyCo 网络、RESP 解析、输出缓冲与 pool 分配器，原样返回一个 bulk 参数。它不执行存储引擎操作，不将该结果当作 HSET QPS。Pipeline 深度使用 `1 / 10 / 20 / 40 / 80 / 160`。默认 redis-benchmark 的 inline PING 是已有的不支持项，使用显式 RESP 命令。

## RDMA / sendfile 相同负载对照

先在接收端运行，再在发送端指定同一个现成文件：

```sh
# 136；mode 可分别替换成 sendfile、rdma。RDMA 还会占用 PORT+1。
bin/benchmark_transfer receive rdma 172.16.145.136 29800
# 135
bin/benchmark_transfer send rdma 172.16.145.136 29800 /absolute/path/snapshot.bin
```

两种路径固定 1 MiB 分块、window=1，使用 TCP 逐块确认；sendfile 路径实际调用 Linux `sendfile()`，RDMA 路径复用项目 transport。接收数据进入内存，校验后丢弃，不含磁盘落盘。两端均输出 JSON，包含字节数、CRC32、`verified` 和计时。

比较时采用发送端 `transfer_seconds`（发送首块前至最后一块 ACK）；`setup_seconds` 与 `checksum_seconds` 单独报告。源文件事先 mmap 计算 CRC，因此这是两种路径一致的热页缓存口径。接收端 setup 含等待客户端的时间，不能直接用作建连性能。该工具对照固定窗口的传输路径，不代表无限窗口或硬件 RDMA 的上限。当前 siw 为软件 RDMA。

iperf3 只作为链路参考，单独记录：

```sh
# 136
/home/jaspersao/dev/91kvstore-tools/bin/iperf3 -s -1 -B 172.16.145.136 -p 29802
# 135；正式矩阵采用预先约定的持续时间和轮次。
/home/jaspersao/dev/91kvstore-tools/bin/iperf3 -c 172.16.145.136 -p 29802 -t 30 -J
```

## SAVE 频率

```sh
python3 -B tests/benchmark_save_frequency.py --entries 1000000 \
  --save-every 1000000 100000 10000 1000 --rounds 3 \
  --output /tmp/91kv-save-frequency-results
```

每次运行新建独立目录及 snapshot/file/pool 配置，使用固定 32 字节 key、128 字节 value 的 Hash 负载和一条持久连接。计时包含数据生成、串行写入及同步 SAVE；记录每次 SAVE 耗时/大小、总耗时、写 QPS 与累计快照字节。区间顺序按轮次轮转。最后不足一个区间也执行 SAVE。计时后重启并逐条核对所有键，保留配置、二进制/脚本哈希、快照和 JSON；失败返回非零，不能把失败轮算作性能样本。输出目录必须事先不存在。

## 预检与故障回归

```sh
python3 -B tests/test_benchmark_tools.py . --rdma-host 172.16.145.135 \
  --redis-server /home/jaspersao/dev/91kvstore-tools/bin/redis-server \
  --redis-benchmark /home/jaspersao/dev/91kvstore-tools/bin/redis-benchmark
make test-capture-faults
```

工具预检覆盖二进制/空 ECHO、分包及 Pipeline、传输边界和坏 CRC 拒绝、Redis 两种 AOF 设置，以及小规模 SAVE 恢复。`test-capture-faults` 的 BPF 事件源是显式文件替身，复用真实独立采集进程的缓存/IPC 与真实 RDMA；覆盖跨环末尾、缺口、缓存溢出、最后一条事件丢失且无后续写入、进程退出及丢缓存后重新全量。socket 模式另测全量期间缓存溢出、拒绝 ONLINE 和重新全量。

替身不能证明内核挂载成功；真实 BPF 验收另行执行：

```sh
sudo -v
RDMA_TEST_IP=172.16.145.135 python3 -B tests/test_capture_replication.py .
```

本轮只运行正确性验收与小负载预检，百万条 SAVE 和正式性能矩阵尚未运行。
