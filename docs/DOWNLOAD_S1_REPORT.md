# S1 实现与交付报告

日期：2026-11-18。状态：实现与自动化验证通过，交付包等待人工验证；未进入 S2。

## 实现范围

- [策略模型](../src/services/download_policy.h)与[配置存储](../src/app/config_store.cpp)：全局/任务速率、连接上限、下载/做种/元数据/校验槽位、队列优先级；严格类型/范围/枚举校验，0/null 继承语义，原子持久化。
- [共享带宽控制](../src/librats/src/librats/bittorrent/bandwidth_controller.h)：请求发出前限速配额与在途预约，接收背压及内容/控制流量分离；全局、任务与公平份额共同约束；超时、取消、断连释放预约。请求与接收使用独立预算，不会把同一块双重扣在同一桶中；取消不返还已消耗的请求速率配额。
- [传输层](../src/librats/src/librats/bittorrent/peer_connection.cpp)：TCP/uTP 共用内容帧记账；部分写按实际接受字节结算；MSE 加密前保留帧归属。配额耗尽关闭读/写就绪订阅并预约唤醒，热更新立即唤醒等待者。高水位继续限制发送缓存。
- [Client](../src/librats/src/librats/bittorrent/client.cpp)：入站/出站、握手中连接及 TCP pending 均纳入总/任务连接预算；失败、移除和热降限清理资源。磁盘重检由共享槽位控制，后台元数据与前台任务使用同一内容/控制边界。
- [调度服务](../src/services/download_service.cpp)：非活动创建后统一准入，用户暂停与调度暂停分离，下载/做种/元数据/校验分类预算，优先级/FIFO/等待老化和停滞冷却，队列前移、恢复前批量加载意图及顺序。
- [会话存储](../src/services/torrent_session_store.cpp)：版本、队列顺序、任务策略和意图；旧数组安全迁移并暂停，损坏文件保护，历史完成状态不能覆盖重检。
- [API](../src/rest/api_router.cpp)：download.policy.get/set、download.taskPolicy.set、download.queue.move、download.operation.get；修订冲突拒绝且不修改状态，持久化失败明确返回 appliedNotPersisted，保留旧命令。操作同步等待 reactor 确认，返回终态及可短期查询的 operationId。
- [设置页](../src/ui/settingsdialog.cpp)、[任务属性](../src/ui/taskdownloaddialog.cpp)与[下载列表](../src/ui/downloadswidget.cpp)：KiB/s 单位、全局与任务设置、队列优先级/前移、排队原因、有效限额和上下行速率。设置失败不关闭对话框。

## 实现取舍与验证边界

令牌桶及带宽控制采用头文件实现，未额外引入空壳 cpp。连接预算直接从 Client 持有的连接/pending 容器统计，并在所有销毁路径释放，未建立会与资源生命周期漂移的第二套计数器。

公平策略为活动任务等份预算；队列优先级不改变带宽份额。下载份额不计入纯做种任务。空闲份额暂不动态借用，具体效果与限制见[人工验证清单](DOWNLOAD_S1_VALIDATION.md)。校验槽位等待仍显示 checking，实际磁盘校验数量受独立共享槽位限制。

上传统计为实际交给 TCP 内核/uTP 发送队列的内容字节，不是确认到达远端的 ACK 字节，也不是整个进程的网卡流量。S1 不实现真实文件选择、可调分块、做种比率/时间退出或时间计划。

## 自动化验证

[S1 专项测试](../tests/test_download_s1.cpp)覆盖：

- 假时钟桶容量、时间回退、整数安全、低速公平份额；在途硬上限、连接中预算、热降限和移除清理。
- 10 个任务限 3 个下载、降为 1、用户暂停/恢复、优先级、修订冲突、应用后保存失败和操作查询。
- 新 API 命令恰好回调一次、非法策略不修改状态；partial-write 不提前统计上传。
- 真实本地 TCP 下载限速与 uTP+MSE 上传限速：稳定来源，预热后测量 60 秒，要求平均不高于限额 5%，且达到至少 80% 配额；解除上限后完成下载并逐字节比对。

原有 Qt/引擎回归继续覆盖 UTF-8 路径、磁盘错误、恢复、metadata-only、uTP 和 MSE。10 任务准入使用可控引擎替身；真实公网多任务吞吐与界面交互仍由人工验证。

最终 Release 构建、25/25 CTest（包含 118 项引擎用例）全部通过，总耗时 126.79 秒。两个真实传输场景均在解除限速后完成并逐字节比对；专项测试也通过 API 和 partial-write 验证。

| 60 秒测量 | 上限 B/s | 内容字节 | 时间 | 平均 B/s |
|---|---:|---:|---:|---:|
| TCP 下载 | 32768 | 1949696 | 60.003s | 32493.31 |
| uTP+MSE 上传 | 32768 | 1966080 | 60.001s | 32767.45 |

完整测量值见 [JSON](validation/download-s1-transport.json)。打包后的程序使用仅包含包目录和 Windows 系统目录的 PATH，执行 `--console --version` 成功，版本为 2.0.0.1516。原稳定/S0 ZIP 校验值保持不变。

独立 [S1 依赖补丁](../patches/librats-download-s1.patch)叠加在 UTF-8/S0 补丁之后；已在干净的固定版本依赖副本中顺序应用，核对 10 个变更文件与当前实现一致。

## 人工审查门槛

交付 [Windows x64 ZIP](../dist/RatsSearch-Windows-x64-s1.zip)、[SHA-256](../dist/RatsSearch-Windows-x64-s1.zip.sha256)及[人工验证清单](DOWNLOAD_S1_VALIDATION.md)。使用 [一键脚本](../build-windows.cmd)可重复构建，默认生成独立 s1 包；稳定/S0 包保留。

人工验证通过前不进入 S2，不把文件勾选宣传为真实选择性下载。

最终 ZIP SHA-256：`495e40797972f66cd8437ac3e0e2796758dde1f47a5afb500fd9577833b4172d`。
