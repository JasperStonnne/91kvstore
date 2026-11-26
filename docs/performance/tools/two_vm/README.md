# 固定c8/P1双机对照工具源码

用于复现49b8d3d的Stage 4.4补充实验；不是服务业务代码。入口、人工权限步骤、固定网络地址与清理说明见[Stage 5复现文档](../../STAGE5-REPRODUCING.md#双机c8固定编排源码)。

- prepare.py：构建检查、临时驱动自检、同版本离线bundle与相同ELF分发。
- client.c：c8/P1 HSET吞吐、完整HGET校验、独立c8可见性；READY/GO缩小CPU观察边界。
- controller.py / agent.py / common.py：串行12轮，固定操作的认证控制代理，按PID身份清理自己的子进程。
- summarize.py：每轮值/三轮中位数/波动率，以及诊断摘要。
- selftest.py：临时C驱动的loopback模拟协议自检，不代替真实复制验收。

运行前需要干净49b8d3d副本，不能因当前main已前进而跳过版本检查。默认只支持本次c8四档入口；压力观察phase不是正式100000次吞吐成绩。原始脚本支持的c1客户端参数不意味着c1编排流程已在此入口复现。
