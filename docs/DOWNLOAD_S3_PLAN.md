# S3 执行记录与细化计划

授权：2026-11-19，用户明确要求“继续完成 S3”。原 S0–S2 计划仅将 S3 列为后续；本次按新增指令实施，保留既有阶段报告，不代填先前人工审查记录。

## 实施单元

| 单元 | 待修改函数 | 新增函数/模块 | 目的与门槛 |
|---|---|---|---|
| S3-01 模型 | Global/TaskDownloadPolicy::validate/toJson/fromJson；会话 load/save/validate | BandwidthSchedule::resolve/nextChange；SeedingPolicy::shouldStop；scheduledPolicy | 严格校验、版本 1→2 迁移；时区、跨午夜、重叠规则、夏令时测试 |
| S3-02 策略 | Torrent::apply_piece_priorities/configure_storage/refill/on_piece/release_requests | set_download_strategy；RequestWindowPolicy::computeWindow | 稀有/顺序/所选文件首尾倾向；窗口 1–128，保留 S1 在途预算和磁盘背压；不改协议分块大小 |
| S3-03 存储 | ThreadedDiskIo::async_read/write/check/apply_file_selection；Client::stop | DiskCacheBudget::acquire/release/evict/invalidate；Client::set_cache_budget | 全 Client 共享、LRU、有界条目和数据预算；默认关闭，校验/写入/映射切换后失效；晚回调不能重新插入旧数据 |
| S3-04 服务 | DownloadService::reconcileSchedule/updateRates/resume/setTaskPolicy/applyEnginePolicies | setSchedule/setSeedingPolicy/scheduleStatus；Engine::setAdvancedPolicy/setCacheBudget | 分离计划暂停、做种条件与用户暂停；计时与停止状态可恢复；全局约束不可绕过 |
| S3-05 视图/API | ApiRouter 下载方法；SettingsDialog；TaskDownloadDialog；下载状态显示 | piece_ranges/pieceMap；PieceMapWidget；DownloadScheduleWidget；SeedingPolicyWidget | 有界聚合分片图，独立按需查询；高级设置、API 和中文文案 |
| S3-06 交付 | 构建脚本、说明与补丁序列 | 独立 S3 依赖补丁；download_package_smoke | 底层/业务回归、同机基准、解压包实际本地下载/重启验证、独立 ZIP，等待人工验证 |

删除函数：没有预定公开 API 删除。原有 S0–S2 入口保留。固定流水线常量可以在无调用后删除；不调整种子元数据 piece 长度或 BT block 大小。

## 冻结语义

- strategy：`rarestFirst` 默认、`sequential`、`firstLast`。首尾倾向仅提升已选文件边缘完整分块，已有在途请求自然收敛，不保证媒体一定可预览。
- 自动窗口默认 2–32；无测量时用 32。手动 1–128；上下界仍受共享在途字节预算、限速预算、错误/选择门禁与磁盘背压约束。
- 读缓存默认 0，最多 256 MiB 数据、4096 条目；写缓冲与读结果临时副本继续单独受已有请求和磁盘背压约束，不将缓存预算宣传为整个进程内存上限。
- 周计划采用 IANA 时区、本地星期和分钟。跨午夜部分归开始日期；拒绝重叠。夏令时跳过的时间不存在，重复时段按相同本地规则执行。额外速率上限与全局/任务上限取交集。
- 做种目标：分享率（千分比整数）、活动时长（秒），任一/全部已启用条件。0 不启用；全为 0 不停止。任务 null 继承全局。所选文件完成后才统计活动做种时间，暂停/排队不计时。达到目标不删除文件；手动重启做种重置本轮时长与上传计数基线。
- 会话版本 2；读取版本 1 保留 S2 已确认选择与用户暂停意图，为新增设置补安全默认值。未知版本/损坏配置拒绝覆盖。

阶段停止点：实现及自动验证后交付 [S3 报告](DOWNLOAD_S3_REPORT.md) 和 [人工清单](DOWNLOAD_S3_VALIDATION.md)，等待人工结果。
