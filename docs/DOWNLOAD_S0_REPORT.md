# 下载优化 S0 实施与验收记录

日期：2026-11-16。状态：S0 实施和技术验证完成，待人工阶段验收；未开始 S1/S2。

依据：[细化执行计划](DOWNLOAD_EXECUTION_PLAN.md) S0-01～S0-04。之前的 UTF-8 修复、用户审查记录和稳定 ZIP 均保留。

## 实施结果

| 任务 | 已落地内容 | 主要位置 |
|---|---|---|
| S0-01 契约 | DownloadPhase、DownloadError、OperationResult 及 JSON 映射；保留旧字段与创建接口 | [download_status.h](../src/common/download_status.h) |
| S0-02 计量 | 累计接收与有效校验字节分离；以真实字节而非 piece 数量计算进度；按文件统计有效区间；重复校验不重复累计 | [Torrent](../src/librats/src/librats/bittorrent/torrent.cpp)、[Client](../src/librats/src/librats/bittorrent/client.cpp) |
| S0-02 错误 | 在失败 CRT 操作处捕获线程局部 errno、操作和 UTF-8 路径，按值传回 reactor；永久错误停止请求并保持错误状态，普通恢复不能消除错误 | [fs.cpp](../src/librats/src/librats/util/fs.cpp)、[DiskIo](../src/librats/src/librats/bittorrent/disk_io.cpp)、[错误分类](../src/librats/src/librats/bittorrent/download_error.h) |
| S0-02 短暂失败 | 写入/分片读回校验的可重试错误最多退避 3 次（1/2/4 秒），随后停止；哈希不匹配单独计数与重新取块，不归为磁盘错误 | 同上 |
| S0-03 受控创建 | Inactive / MetadataOnly / Automatic；按标识创建、明确开始元数据或内容；创建结果不把跨线程裸指针交给服务 | [Client](../src/librats/src/librats/bittorrent/client.h)、[TorrentEngine](../src/net/torrent_engine.h) |
| S0-03 共享引擎 | 后台查询仅获取元数据；不覆盖用户任务回调/内容门禁；只清理查询自己创建且仍存活的任务 | [Bittorrent::fetch_metadata_impl](../src/librats/src/librats/subsystems/bittorrent.cpp) |
| S0-04 应用接入 | 先创建非活动任务、登记服务，再允许内容传输；恢复时重新校验，不强制恢复完成标志；保留元数据尚未到达时的选择和预览大小 | [DownloadService](../src/services/download_service.cpp) |
| S0-04 展示/API | 注入单调时钟，分开接收/有效速率，使用平滑有效速率估算 ETA；GUI 显示校验进度和错误；增加失败/阶段变更事件 | [下载界面](../src/ui/downloadswidget.cpp)、[ApiRouter](../src/rest/api_router.cpp)、[API 说明](API.md) |
| S0-04 生命周期 | 用代次和存活标记隔离旧磁盘回调，避免停启/移除后污染状态；析构时不持有服务 mutex 等待引擎保存 | Torrent / DownloadService |

实现选择：DiskIo 增加详细结果的虚方法，保留原 bool 虚接口并提供兼容适配；没有删除旧虚函数或破坏现有 fake。保留旧 Client 自动开始入口，以新增受控入口承接应用恢复与添加。原私有 computeSpeed 已由 updateRates 替换，其余 S2 删除项尚未动。

## 冻结的兼容语义

- `downloaded` / `downloadedBytes` / `downloadSpeed` 继续表示累计接收与接收速率，重试可使接收量大于文件大小。
- 新 `verifiedBytes` 表示当前通过完整 piece 校验的有效数据；重检后可以降低。`progress` 以该字节数和总大小计算，每文件同理。
- `phase`、`enginePaused`、`error`、`hashFailures` 是增量字段；GUI 不再把已接收字节当作落盘证明。
- `queuedUploadBytes` / `uploadQueueSpeed` 明确是加入发送队列的计量，**不是实际 socket 发送量**；真实上下行预算和发送结算在 S1 接入 PeerConnection/PeerLink。
- 新失败事件 `downloadFailed`、阶段事件 `downloadStateChanged` 通过现有 WebSocket 广播通道传递，HTTP/WS 的原命令入口未移除。
- S0 保留添加任务后开始下载的既有用户体验，但先完成服务登记；“等待文件选择确认”的默认交互在 S2 上线。

S1 接口边界延续原方案：用户意图与运行暂停分离、全局/任务策略分层、0/null 语义固定、下载/元数据/做种槽位分开。其具体策略类型、修订持久化和调度器尚未实现，不能将 S0 的创建门禁视为并行限制。

## 共享资源归属

应用 P2PTransport 挂载一个 Bittorrent 子系统，用户下载、创建做种与后台元数据获取复用其中 Client 和 BT Peer 连接。DHT 由独立 DhtDiscovery 提供，Rats 自有节点连接亦独立于 BT Peer 连接。

S0 已隔离后台查询的内容下载及生命周期。S1 的内容令牌桶应挂在共用 Client 层，TCP/uTP 使用同一预算；DHT、Tracker、元数据与 Rats 自有协议的控制预算另列。不能用 ConfigStore 的 Rats P2P 连接数代替 BT 的连接预算。

对同 hash 已存在的任务：若已有元数据则直接复用副本且不接管任务；若仍在获取元数据则返回 `metadata already in progress`，由调用方重试，不覆盖已有回调。多订阅者合并机制暂未扩展。

## 测试证据

Windows x64、MinGW 13.1、Qt 6.10.1、Release：

- 完整构建与 CTest：**23 个目标通过**，包含 **118 项底层 GoogleTest 回归用例**；无失败。
- 新增 [test_download_metrics](../tests/test_download_metrics.cpp)、[test_download_creation](../tests/test_download_creation.cpp)、[test_download_errors](../tests/test_download_errors.cpp)，合计 13 个行为场景（不含 init/cleanup）。最终新增的字节加权进度用例另行增量编译并通过。
- 底层回归覆盖文件系统、磁盘、下载、暂停恢复、resume、piece picker、store buffer、元数据、子系统、uTP、MSE；使用与应用链接相同的 rats 库，避免另编一份不同配置的引擎。

新增关键断言：

1. 接收 300 字节但只校验 60 字节时，保留接收计数，进度/ETA 使用有效数据。
2. 一块 16 KiB 有效、最后一块损坏的 20,000 字节文件，进度为 16384/20000，不是 1/2。
3. 磁力拿到元数据后在 Waiting 状态无内容请求、无最终文件；明确允许内容后逐字节下载正确。
4. 保存为完成的任务，其内容被同长度替换后，恢复校验结果为未完成；旧完成标志不能覆盖结果。
5. 旧校验回调在停启后被丢弃，不把新任务错误标为完成。
6. 只读文件、非法父路径在工作线程捕获具体 I/O 错误；出错后不继续请求，普通恢复不能清除错误。
7. 人为提供错误哈希内容时，累计接收增加、有效进度为零、哈希失败单独记录。
8. API 错误只在错误变化时发一次；单调时钟重置不产生负速率；元数据未到时不丢失恢复选择。
9. 后台元数据查询不能移除或启动已存在的用户非活动任务。

磁盘满使用 errno 分类测试，没有填满用户磁盘。1/2/4 秒瞬时 I/O 退避逻辑未做全部系统错误组合的端到端故障注入；当前故障集以永久错误、真实 Windows 只读行为和哈希错误为重点。Linux/macOS 本阶段未实机验证。

## 基准

夹具：[benchmark 源码](../benchmarks/download_benchmark.cpp)；原始结果：[JSON 记录](validation/download-s0-benchmark.json)。

同一机器、同一 MinGW Release 配置，32 MiB、256 KiB pieces、localhost TCP、无 MSE/uTP/DHT，源/目标均由同一进程运行；每版本 3 次，最终内容逐字节验证。基线是锁定的 librats commit 加上已交付的 UTF-8 补丁；当前版本是 S0。基线使用独立构建目录，未修改实际工作子模块。

| 指标 | S0 前 | S0 后 |
|---|---:|---:|
| 吞吐中位数 | 6.163 MiB/s | 6.313 MiB/s |
| 三次合计进程 CPU 时间 | 4.078 秒 | 3.969 秒 |
| 观察到的最高工作集 | 110.930 MiB | 109.617 MiB |
| 每次逻辑内容写入字节 | 33,554,433 | 33,554,433 |
| 逻辑写放大 | 1.00000003 | 1.00000003 |

本夹具未观察到吞吐回退，不能据 3 个样本声称普遍提速。CPU 时间包含本地 seed 与下载两端；工作集为整个进程峰值，包含测试缓冲区。写入计数使用链接包装器统计成功的 write_file_chunk 和预分配尾字节，不是 SSD 物理写放大，也不含设备内部行为。后续 S1/S2 必须用同一夹具复测并增加多任务、低速和边界文件场景。

## 可复现构建与阶段包

- [一键构建入口](../build-windows.cmd) 默认运行应用与底层测试；GoogleTest v1.17.0 缓存在项目工具目录，离线构建不隐式联网。
- 依赖顺序为 [UTF-8 补丁](../patches/librats-windows-utf8.patch) → [独立 S0 补丁](../patches/librats-download-s0.patch)。已验证在干净锁定依赖上依序应用，并逐文件比较 10 个生产文件与工作树一致；原补丁未覆盖重写。
- 阶段包：`dist/RatsSearch-Windows-x64-s0.zip` 与 `.sha256`。之前 `dist/RatsSearch-Windows-x64.zip` 保持原文件不变。
- 已验证解压到中文路径、移除工具链 PATH 后以 `--console --version` 启动，校验 ZIP SHA256；这属于加载/启动烟测，不等于 GUI 人工验收。

```powershell
.\build-windows.cmd -Offline -Jobs 4
```

## S0 门槛与后续

- [x] 有效进度与累计接收量分离；重检与重复回调不重复累计。
- [x] 永久 I/O 错误停止重取且可观察；哈希错误单独处理。
- [x] 非活动/仅元数据创建不先下载内容；共享任务生命周期有隔离。
- [x] 回归、基准、依赖补丁和阶段包证据已保存。
- [ ] 人工阶段验收。

未实现且不宣称已支持：速度上限、并行队列、真实文件选择、文件优先级、边界存储、手动 recheck/retry API、S3 高级设置。S0 出错后应先修复路径/空间/权限，再移除并重新添加任务；移除不会删除已有内容。上传统计仍是队列口径。旧会话原子 schema 迁移在 S1 实施。

S0 后重新评估：S1 保留 7–12 人日，S2 保留 10–15 人日。详细 I/O 兼容适配降低了 fake 迁移风险；发送队列真实计量、请求预算释放、跨文件边界存储仍是主要不确定项。范围扩大或门槛失败应调整估算，不自动转入下一阶段。
