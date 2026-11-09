# 可靠性回归与失败契约

普通功能回归运行 make test。诊断和真实 BPF 入口另行列出；
没有相应环境或权限的运行不能记为通过。
所有故障测试只使用专属临时目录、端口、连接和 Popen 子进程；
不填满磁盘、不改系统网络、不关闭 VM、不访问正式数据。
证据目录保留，测试退出核对本轮服务进程和端口释放。

## 入口

| 命令／脚本 | 覆盖 | 环境 |
| --- | --- | --- |
| make test-hybrid-recovery | 缺日志、日志过短、两种普通检查点错位 | 无 SIW/BPF/root 要求 |
| make test-aof-write-failure | AOF 部分写、无假成功、残缺日志拒绝恢复 | Linux 同 UID 子进程限制 |
| make test-aof-sync-failure | 同步函数 EIO、完整未确认命令重启恢复 | 独立测试程序，无 root |
| make test-snapshot-save-failure | 临时文件同步、rename、目录同步错误 | 独立测试程序，无 root |
| make test-snapshot-crash | 七个崩溃窗口，四引擎新增/修改/删除 | 只 SIGKILL 专属子进程 |
| make test-replication-faults | 五种断线、四种主/副本 SIGKILL | ACTIVE iWARP/SIW |
| make test-rdma | 原有真实 RDMA 加 Primary AOF 错误回归 | ACTIVE iWARP/SIW |
| make test-sanitizers | 独立临时构建的 15 个 ASan/UBSan 单测 | GCC sanitizer 运行库 |
| make fuzz-protocol | 有界 RESP 覆盖引导诊断，默认 50000 次 | clang 与 libclang-rt |
| tests/test_ebpf_replication.py | 真实 BPF payload、溢出、消费错、版本拒收等 | BPF 权限与 SIW |

前三类持久化故障、Snapshot、复制故障和固定单测已接入 make test。
可选 sanitizer/fuzz 不强制加入普通回归。
Primary 写入与同步错误复用单机脚本的 --primary-hybrid 模式，并由 test-rdma 调用。

真实中断入口先普通用户 make ebpf-startup，再使用已授权的 BPF 权限运行：

~~~bash
sudo env KVS_EBPF_OBJECT="$PWD/build/ebpf_capture.bpf.o" RDMA_TEST_IP=<本机SIW的IPv4> \
  python3 -B tests/test_replication_faults.py . \
  --binary "$PWD/bin/kvstore-ebpf" --require-ebpf
~~~

该模式要求真实附着；ONLINE 后案例还要求实际 payload 日志；
出现 ONLINE source=AOF 降级即失败，不能用模拟单测代替。
不要向终端粘贴提示符或历史输出。

## Snapshot-only SAVE

| 失败位置 | 回复／进程 | 正式文件与普通重启 |
| --- | --- | --- |
| 临时文件同步 EIO | SAVE 错误，服务存活 | 保留旧快照，重启恢复上次成功 SAVE |
| rename EIO | SAVE 错误，服务存活 | 保留旧快照，重启恢复上次成功 SAVE |
| rename 成功后目录同步 EIO | SAVE 错误，服务存活 | 新快照已替换，不自动回滚；本轮普通重启恢复新版本 |

SAVE 错误表示整套流程未全部成功，不表示正式文件一定未改变。
三种错误返回情况下内存仍保留最新修改，临时文件正常清理。
测试在函数边界模拟 EIO，不验证真实介质故障。

七个进程崩溃位置：SAVE 前、写入中、文件同步前后、rename 前后、目录同步后。
SAVE 前在客户端确认内存修改后直接结束进程；其余位置通过包装器标记和 SIGSTOP
确定到达位置，再 SIGKILL 本轮子进程。写入中真正写出前 16 字节才暂停；
同步后暂停以真实调用成功为前提，不伪造内核完成结果。
每例核对四引擎 12 个读取结果、退出码、文件和重启状态。
rename 前恢复旧版本；rename 后的普通重启恢复新版本。
崩溃遗留临时文件可能残缺或完整，当前启动忽略并保留它们，不宣称自动清理。

SIGTERM 不能替代成功 SAVE；SIGKILL 只模拟进程退出，不等于断电。
真实磁盘故障与断电按用户要求不实测，保留为未验证。

## AOF 与 hybrid

- 内存操作先发生，然后 AOF 写入和文件同步；成功后才推进提交 offset 并允许回复成功。
- 写入或同步失败：当前命令不回复成功，进程报错退出；不回滚内存或自动修剪日志。
- 残缺尾部：重启拒绝恢复、文件保持原样；完整前缀可能已经回放，但失败启动不对外服务。
- 完整命令写入后同步报错：普通重启仍可能恢复这条命令；无成功回复不代表未执行。
- Primary hybrid 有快照但缺配套 AOF，或 AOF 短于快照检查点：拒绝启动。
- AOF 等于检查点时无需回放，大于时继续解析检查点之后的完整命令。
- 空目录首次启动和 Snapshot-only 不受配套 AOF 检查影响。
- 有意用独立快照初始化 hybrid 没有新增专用入口；普通恢复不能自行猜测用户意图。
- 合法重复 AOF 记录按顺序执行，不自动去重，不宣称 exactly-once。
- 两种普通错位有测试，不保证识别所有不配套文件、合法整记录丢失或任意伪造边界。

test_aof_replay 覆盖逐位置截断、固定错误尾部、文件保持和重复记录顺序；
test_snapshot_store 覆盖逐位置截断及 CRC 正确但结构不合法的固定文件。
已有重复 key、v2 检查点损坏、合法版本恢复继续复用。
test_protocol 覆盖逐字节半包、粘包、NUL/CRLF、4096 字节精确限额、
128 字段容量、错误数字/结尾和过小输出缓冲区。
控制边界单测覆盖通知与 ONLINE 的全部半包前缀及错误字段/offset。
传输替身不代表真实 CM/CQ。

## 复制失败契约

五种连接场景：追赶命令中间、ONLINE 前、ONLINE 中间、错误 ONLINE offset、
ONLINE 后命令中间。四种进程场景：Primary/Replica 在 ONLINE 前和实时命令中途退出。
闸门仅代理本轮 TCP 控制/增量，快照仍走真实 RDMA。
暂停全量通知后先写四引擎 MOD/DEL/SET，形成确定的非空追赶。
逐字节比较两端 AOF 后缀，检查只有完整命令应用；断线后拒绝数据读取。
SIGKILL 先确认完整前缀已应用，再结束专属进程并释放代理。
人工重启重新全量，验证新同步 ID、四引擎数据、快照字节及新 offset。
没有自动重连、断点续传、逐条增量 ACK 或端到端 exactly-once。
Primary 发送游标不是 Replica 已应用确认。

## P1：SIW 重启建连偶发失败

首次证据：/tmp/91kv-replication-faults-fdp_8fbt/primary-crash-online。
Primary 服务端口 58397、RDMA 45577，Replica 服务端口 34177，
前后代理端口 35655、40259。
Primary 在 ONLINE 被杀后数据恢复正确；下一轮全量建连报
Address already in use，Replica 明确断开、没有假 ONLINE。
首次结果 passed=false，原始文件及日志保留。

当时没有同步监听进程/内核日志，不能断言是旧进程占用或 TIME_WAIT。
之后原样独立三次、九例合跑以及最终两 VM 冻结回归均通过，不代表根因已修复。
后续自动保存本轮端口、PID、启动时刻和单调事件时序；明确失败立即报错，
不自动换端口、重试、SKIP 或覆盖首次失败。
该问题作为 P1 恢复可用性风险保留；没有本轮证据证明它导致静默数据丢失。

## 验收范围与证据

被测代码冻结于源树指纹
1c30b6b84a1eb250721154f98d12db22119c22d2808a4c6f439afd4d3c100e09；
验收后仅此说明文档补充最终结果，业务与测试代码不变。

- VM-A 普通完整回归：/tmp/91kv-stage11-frozen-yezvmiun/vm-a-make-test.log。
- VM-B 普通完整回归：/tmp/91kv-stage11-isolated.UAkbM3/vm-b-make-test.log。
  两端退出 0，源码/程序指纹前后相同，11 组集成通过。
- VM-A 真实中断：/tmp/91kv-replication-faults-07n5k_cf。
- VM-B 真实中断：/tmp/91kv-replication-faults-z4j0bi28。
  各九例通过；三个 ONLINE 后案例各有真实 payload，其余验证附着下的追赶边界。
- VM-A 既有真实专项：/tmp/91kv-ebpf-acceptance.g19ipm7m。
- 最新隔离 15 个 ASan/UBSan 单测：/tmp/91kv-sanitizers-x4xpbo8w。
- 有界协议诊断：/tmp/91kv-protocol-fuzz-zbrsuhto，50000 次退出 0。
  按用户偏好未扩大随机测试；文件及控制行没有独立覆盖引导 fuzz。

sanitizer 不覆盖真实 NtyCo 协程服务、CM/CQ 收发或 BPF 内核运行。
fuzz 未发现错误不代表所有输入安全。编译器或运行库缺失时，
诊断脚本明确 SKIP/77；其他构建、断言、运行错误非零 FAIL。
真实运行缺权限/设备不记为 PASS。

最终双 VM 5万＋5万、完整命令流对账、无探针基线、Git 与用户资产状态，
见随阶段交付的验收报告与机器可读清单。性能数字仅供下一阶段对照，
不提供硬件 RDMA 结论，不用 sanitizer/bpftrace/perf 期间吞吐作为成绩。
