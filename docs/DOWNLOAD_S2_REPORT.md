# S2 实施与交付报告

状态：实现及自动验证完成，等待人工验证。用户已确认 S1 验证暂未发现问题并授权继续 S2；不进入 S3。

## 实施结果

- 文件优先级为跳过/普通/高，保持原始元数据索引；重叠分块取所关联文件的最高优先级。进度与 ETA 使用所选文件已校验字节加权。
- [FileSelection](../src/librats/src/librats/bittorrent/file_selection.h) 负责映射与验证；[BoundaryStore](../src/librats/src/librats/bittorrent/boundary_store.cpp) 将未选文件的边界字节存入 `.parts/<hash>/`，采用原子清单记录 hash、优先级与版本。新目录不创建跳过文件；取消选择不删除已有最终文件。
- [磁盘队列](../src/librats/src/librats/bittorrent/disk_io.cpp) 使用独占栅栏切换映射，等待旧读写完成；选择版本隔离旧写入、哈希、校验与上传读取回调。只有全分块校验成功才发布 HAVE。
- [Torrent](../src/librats/src/librats/bittorrent/torrent.cpp) 区分所选完成与完整下载完成，增加部分做种状态。Tracker left 使用整个种子的已校验分块字节；所选完成不发整种完成事件，也不触发完成后移除。
- [服务层](../src/services/download_service.cpp) 提供异步版本化选择、重新校验与重试；服务锁不跨 reactor 调用。操作结果依次经过引擎确认、会话保存、事件发布；保存失败明确返回 appliedNotPersisted。
- 会话保留选择确认与版本，resume 保留文件优先级；部分下载重启后重新校验辅助/最终数据。旧会话缺少确认信息时暂停等待人工处理。保持 schemaVersion 1，新增字段向后兼容；未知版本仍拒绝覆盖。
- [任务设置](../src/ui/taskdownloaddialog.cpp) 承载新建后的文件确认和后续编辑：三态目录、优先级、名称/扩展名筛选、匹配批量选择、每文件进度、校验/重试。复用现有任务设置窗口，未另建独立 AddDownloadDialog。新任务元数据就绪前不调度内容，确认前保持等待。
- [文件预览](../src/ui/torrentfileswidget.cpp) 改为显式打开选择窗口，删除原先浏览勾选立即提交的辅助函数，避免排序视图误改任务。[API](API.md#s2-selective-downloads)、[使用说明](MANUAL.md)、[服务端说明](SERVER.md) 和中文主要界面文案已更新。

## 自动验证

最终通过 [一键构建](../build-windows.cmd) 的 MinGW x64 Release 构建及 **25/25 CTest**，耗时 **125.30 秒**。包含 118 项原有底层 GTest 回归；S1 TCP 下载/uTP+MSE 上传限速仍通过。

新增自动验证包括：

- 只选中间文件并跨两侧分块，最终内容逐字节比对，左右跳过文件不落盘；重启恢复选择版本，重新选择时物化边界数据，空文件创建与全部跳过，损坏辅助清单拒绝恢复。
- 真实 localhost TCP 选择性下载；所选完成为部分做种；由部分做种端向另一端上传；删除最终文件后校验量下降；过期选择版本被拒绝。
- 服务层确认前无内容准入、完整优先级数组验证、版本冲突、操作忙状态、异步终态查询、所选完成只通知一次且不发送整种完成。
- 原 UTF-8 路径、磁盘错误、恢复、metadata-only、队列并发及 API 单次响应测试继续通过。

自动场景在 [存储测试](../tests/test_utf8_download_paths.cpp)、[真实传输测试](../tests/test_download_creation.cpp)、[服务指标测试](../tests/test_download_metrics.cpp)；运行中复杂切换、断电/磁盘满现场、真实公网资源差异与 GUI 交互仍需按人工清单验证。未将这些人工项目标记为自动通过。

## 性能门槛

同一 [32 MiB localhost TCP 夹具](../benchmarks/download_benchmark.cpp)，256 KiB 分块、Release、关闭 MSE/uTP/DHT，每次完成后逐字节比较。S1 使用从锁定依赖加 UTF-8/S0/S1 补丁构建的独立库；S2 使用最终库。

| 测量 | S1 中位数 MiB/s | S2 中位数 MiB/s | 相对变化 |
|---|---:|---:|---:|
| 首次，每版本 3 次，存在其他测试/打包干扰 | 5.666 | 5.183 | -8.52% |
| 空闲复测，每版本 3 次，反转运行顺序 | 5.318 | 5.748 | +8.08% |
| 全部 6 次合并中位数 | 5.467 | 5.320 | **-2.68%** |

保留 [首次 S1](validation/download-s2-s1-benchmark.json)、[首次 S2](validation/download-s2-benchmark.json)、[空闲 S1](validation/download-s2-s1-benchmark-idle.json)、[空闲 S2](validation/download-s2-benchmark-idle.json) 全部原始样本。合并结果满足不超过 5% 的回退门槛；样本量与波动不足以推断普遍提速。逻辑写放大均约 1.00000003，不是物理磁盘写放大。

## 可复现包与审查门槛

[独立 S2 补丁](../patches/librats-download-s2.patch) 追加在 UTF-8/S0/S1 补丁之后；在干净固定依赖副本顺序应用后，核对 **20 个生产文件**与工作树内容一致。旧阶段补丁保留。

[Windows x64 S2 ZIP](../dist/RatsSearch-Windows-x64-s2.zip) 与 [SHA-256](../dist/RatsSearch-Windows-x64-s2.zip.sha256) 已生成；脚本默认输出 s2 包。包内 build-info 标记 S2-awaiting-human-validation、Qt 6.10.1、Release、testsRun=true。

ZIP SHA-256：`7577d65fd6213d081b35b5973020e4641dd3c61619267f2f1b8b76435e79b0ea`。

已解压到中文路径，在仅包含包目录和 Windows 系统目录的 PATH 下执行 `--console --version` 成功，版本为 2.0.0.1516。此项是解压包启动验证，真实传输由同次构建的测试程序覆盖，未宣称已完成解压包 GUI 实际下载验证。

原稳定、S0、S1 ZIP 的 SHA-256 保持不变。请按 [人工验证清单](DOWNLOAD_S2_VALIDATION.md) 验证，通过后再进入下一阶段。
