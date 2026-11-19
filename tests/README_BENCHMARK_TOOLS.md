# 性能对照工具与正确性预检

这些程序只增加测试入口，不修改服务端业务命令、持久化或复制逻辑。2026-11-19 正式矩阵已完成测量：SAVE、单机 QPS/Pipeline、RSS、socket 复制与文件传输取得有效结果；真实 eBPF 持续负载三轮均失败，不能记作有效复制 QPS。完整数据、失败与限制见[本批报告](../docs/performance/2026-11-19.md)。小负载预检仅用于确认工具可用，不替代压力测试。

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

独立 ECHO 程序复用项目 NtyCo 网络、RESP 解析、输出缓冲与 pool 分配器，原样返回一个 bulk 参数。它不执行存储引擎操作，不将该结果当作 HSET QPS。P1 为 ECHO、kvstore none/aof、Redis 无 AOF/AOF always 五组；Pipeline 为两端无 AOF、深度 `10 / 20 / 40 / 80 / 160`。本批两端每档均为 10 万请求、3 轮，正式前预热至少 1 万次，向完整批次取整。kvstore HSET / Redis SET 都覆盖同一个 32B 键、128B 值，不能当作唯一键插入吞吐。默认 redis-benchmark 的 inline PING 是已有的不支持项，使用显式 RESP 命令。具体命令数组和服务配置保留在[数据文件](../docs/performance/data/measurements-2026-11-19.json)的 `provenance` 中；时钟异常的旧批次不计成绩。

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
  --save-every 1000000 100000 10000 --rounds 3 \
  --output /tmp/91kv-save-frequency-results
# 1k 档单独一轮；本批实际由原排程调整，完整保留已验完的首轮 1k。
python3 -B tests/benchmark_save_frequency.py --entries 1000000 \
  --save-every 1000 --rounds 1 --output /tmp/91kv-save-frequency-1k
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

本批测量还使用既有 `tests/benchmark_memory.py` 完成三分配器各三轮百万条四阶段核对；双机每种模式各三轮 c1/P1、10 万条热点覆盖写，并另测 1,000 次可见性探针。所有文件传输使用同一份真实百万条快照、两端 SHA-256 一致，每方向每路径热身一次、正式三轮；iperf3 每方向 3 × 30 秒单流。结果均取中位数，SAVE 1k 单轮除外。

正式计时已结束：交互式特权助手已清理三个采集实例及 socket，恢复 135 的 NTP 与 VMware 周期校时。未调整 136 原有工作区，测试用独立副本；未为性能测试修改服务端、扩容缓存或限速生产者。完整测量报告保留 eBPF 缓存溢出/事件缺口、Pipeline 退化和时钟诊断，不写成整批全绿。

## 2026-11-19 修复后正式重测

修复提交 `846c87d` 已合并到 main 并推送两个远端。基于更新后的 main 全量以 `-O2 -g` 重编译，仅重跑单机 QPS/Pipeline（45/45）和双机实时复制（9/9）；真实 eBPF 三轮通过，oversize/ring_full/read_error 均零。SAVE、百万条三分配器 RSS、RDMA/sendfile/iperf3 沿用修复前样本。

报告保留修复前后中位数、波动及全部原始轮次，包括本轮 none/socket 吞吐下降；未把失败轮 Primary 回复当成成功复制吞吐。新样本与脚本、二进制 SHA、校时暂停/恢复信息位于 `measurements-2026-11-19.json` 的 `retune` 字段，旧顶层数据保持原样。原始参数、配置与日志在 135 `/home/jaspersao/dev/91kvstore-performance-fix-20261119`。

五张新图使用实际 JSON 离线生成，不启动服务、不重跑测试，需已有 matplotlib：

```bash
python3 -B docs/performance/plot_results.py \
  --data docs/performance/data/measurements-2026-11-19.json
```

未指定 `--data` 时仍生成历史五图；新图以 `20261119-` 前缀命名，避免覆盖历史图。
