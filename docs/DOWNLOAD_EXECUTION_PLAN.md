# 下载优化细化执行计划（待人工确认）

日期：2026-11-15。

依据：[优化方案第 7–8 节](DOWNLOAD_IMPROVEMENT_PLAN.md)。总体设计已获人工通过；本执行计划尚未确认，禁止据此直接开始功能实现。

批准范围按原方案勾选项理解为 S0–S2；S3 仅列后续设计任务，不纳入本轮实施授权。本次只新增本文，不改动源码、依赖补丁、构建产物或用户已填写的审查记录。

## 1. 阅读约定与执行边界

- **M（修改）**：已在当前源码核对存在的函数；表格中的多个函数均需检查和修改相应行为。
- **A（新增）**：拟新增函数名称和职责；签名是设计目标，实现前随所在任务冻结，不表示源码已有该函数。
- **D（删除）**：只删除确认无调用者的私有实现；所有对外接口默认保留兼容入口。
- 下列新增文件均为拟建文件；路径以代码字体表示。已有文件使用可点击链接。
- 每个任务先补可失败的行为测试，再实施，并记录证据。底层库→引擎适配→服务→API/UI 为默认顺序。
- 每阶段结束人工验收；门槛不通过则停在该阶段修复，不用后续 UI 掩盖底层缺陷。

建议类型在 S0 冻结：DownloadError、OperationResult、OperationId、DownloadPhase、PauseReason、UserIntent、GlobalDownloadPolicy、TaskDownloadPolicy、FilePriority、EffectivePolicy、SessionDocument、SessionLoadResult。Qt 类型和 librats 类型分离，以 TorrentEngine 为转换边界；后文同名 policy/result 不是跨层共享 Qt 依赖。

OperationResult 至少区分 applied、accepted、rejected、appliedNotPersisted；带 errorCode/message/revision，可选 operationId。JSON 中速率使用非负安全整数 B/s；null=继承、0=取消该层自身上限；超出可精确表示范围拒绝。全局约束不可被任务覆盖绕过。

## 2. 任务依赖、交付顺序与估算

| ID | 任务/依赖 | 交付与停止检查点 |
|---|---|---|
| S0-01 | 固定现状、结果与错误契约；无前置 | 兼容清单、共享引擎流量归属、基准数据；冻结状态/计数/错误语义 |
| S0-02 | 真实有效字节、磁盘错误；依赖 S0-01 | 计数不因重取虚增；持久错误停止请求 |
| S0-03 | 非活动创建、元数据与内容启动解耦；依赖 S0-01 | 创建和恢复期间不先下载内容 |
| S0-04 | 快照适配、回归夹具；依赖 S0-02/03 | S0 门槛报告并重新估算后续 |
| S1-01 | 策略模型、原子持久化；依赖 S0 | 配置校验、schema 迁移和错误可辨识 |
| S1-02 | 双层令牌桶、公平预算、连接预算；依赖 S1-01 | 假时钟测试、TCP/uTP 本地传输报告 |
| S1-03 | 下载/做种/元数据队列；依赖 S1-01、S0-03 | 任务数硬约束、用户暂停与排队分离 |
| S1-04 | 应用组装、API/UI、恢复联调；依赖 S1-02/03 | 端到端限速队列可用，重启保持策略 |
| S2-01 | 文件→分片映射和边界存储；依赖 S1 | 未选文件不生成最终文件，分片可校验/上传/恢复 |
| S2-02 | 真实文件选择、部分完成与重新校验；依赖 S2-01 | 运行中变更安全，Tracker/Peer 宣告正确 |
| S2-03 | 文件确认交互、API兼容和会话迁移；依赖 S2-02 | 默认先确认文件；旧客户端行为明确 |
| S2-04 | 发布验证与补丁固化；依赖 S2-03 | 测试、基准、ZIP 和已知限制，待阶段验收 |
| S3-* | 高级体验；依赖 S2 验收并另行批准 | 顺序/首尾优先、分片图、请求窗口、缓存、时间计划、做种条件 |

阶段预算沿用原方案：S0 3–5、S1 7–12、S2 10–15 人日；本轮合计 20–32 人日。S3 另计 7–12 人日。任务表不叠加额外估算；S0 后按底层验证结果调整，禁止把初估当作固定交付日期。

## 3. S0：可靠性与接口验证

### 3.1 待修改函数

| ID/位置 | 函数（M） | 修改内容 |
|---|---|---|
| S0-02 [torrent.h](../src/librats/src/librats/bittorrent/torrent.h) / [torrent.cpp](../src/librats/src/librats/bittorrent/torrent.cpp) | Torrent::on_piece、on_block_written、on_piece_hashed | 区分接收、写盘、校验；有效字节仅在校验成功后增加；记录具体 I/O 错误；持久错误停止新请求 |
| 同上 | Torrent::on_check_complete、progress、load_resume_data、generate_resume_data | 重检和恢复按实际 have-bitfield 重建有效字节；累计网络字节继续独立保留 |
| 同上 | Torrent::refill、refill_all、tick、pause、resume、stop | 错误/等待/用户暂停的状态门禁；无效恢复不能继续重取；取消任务安全终止异步回调 |
| S0-02 [disk_io.h](../src/librats/src/librats/bittorrent/disk_io.h) / [disk_io.cpp](../src/librats/src/librats/bittorrent/disk_io.cpp) | ThreadedDiskIo::ensure_file、write_range、read_range、async_write、async_read、async_hash、async_check_files | 捕获失败操作、系统错误码和位置；将 bool-only 完成结果迁移至结构化错误。同步修改 DiskIo 纯虚接口、所有派生类和测试 fake |
| S0-03 [client.cpp](../src/librats/src/librats/bittorrent/client.cpp) | Client::add_torrent_impl、add_magnet_impl、add_torrent_with_resume_impl | 增加创建模式，先应用初始化参数再允许运行；旧公开入口保留默认行为 |
| S0-03 [torrent.cpp](../src/librats/src/librats/bittorrent/torrent.cpp) | Torrent::start、try_complete_metadata、promote_to_downloading | 分离元数据成功与内容自动启动；等待确认时不得调用内容请求路径 |
| S0-04 [client.cpp](../src/librats/src/librats/bittorrent/client.cpp) | Client::torrent_status | 输出 verifiedBytes、计数、真实阶段、错误；整个快照在 reactor 上生成 |
| S0-04 [torrent_engine.cpp](../src/net/torrent_engine.cpp) | TorrentEngine::status、addMagnet、addMagnetResumed、addTorrentFile | 转换新增状态与错误；旧接口暂作兼容封装，服务新入口改走受控创建 |
| S0-04 [download_service.cpp](../src/services/download_service.cpp) | DownloadService::pollStatus、applySnapshot、progressJson、flushTransitions | 兼容旧 downloaded 字段；新增有效字节和错误；当前 UI 显示校验进度；不保留错误的完成缓存覆盖 |
| 同上 | Download::toJson、DownloadFile::toJson | 增量输出阶段、错误和有效字节，旧键继续存在 |
| 同上 | DownloadService::restore | 禁止恢复已完成标志永久覆盖引擎重检结果；使用 checking 状态保留合理展示 |
| S0-04 [downloadswidget.cpp](../src/ui/downloadswidget.cpp) | DownloadItemWidget::updateProgress；DownloadsWidget::onProgressUpdated、loadDownloads | 用校验有效字节显示进度，累计接收另列；ETA 不使用重传累计量 |

### 3.2 待新增函数

| ID/拟归属 | 函数（A，签名目标） | 说明 |
|---|---|---|
| S0-01 DownloadError / OperationResult | toJson() const；fromJson(...)；isRetryable() const | 跨服务/API的稳定错误语义；引擎 errno/GetLastError 适配，不把哈希失败归为 I/O |
| S0-02 Torrent | fail_io(error)、clear_error()、verified_bytes() const、rebuild_verified_state(have) | 中止持久 I/O 错误任务；重检计数去重；clear_error 只能在受控重试中使用 |
| S0-02 文件系统错误边界 | capture_fs_error(operation, path)、classify_fs_error(code) | 在底层调用失败后立即捕获线程局部错误，避免稍后日志/线程切换丢失；原 UTF-8 文件接口保留 |
| S0-03 Client | create_torrent(info, savePath, createOptions)、create_magnet(uri, savePath, createOptions) | 新增结构化创建入口；返回受控标识/结果，不把跨线程裸 Torrent 指针交给服务 |
| S0-03 Torrent | start_metadata_only()、start_content()、set_content_allowed(bool) | 元数据与内容启动门禁；createOptions 支持非活动/仅元数据/兼容立即运行 |
| S0-04 TorrentEngine | createDownload(source, options)、startMetadata(hash)、startContent(hash) | Qt 适配；新服务流程不使用自动开始的旧入口 |
| S0-04 DownloadService | updateRates(download, snapshot, monotonicNow)、onEngineFailure(hash, error) | 接收/有效/上传速率分开统计；可注入单调时钟，替换墙钟差分 |
| S0-04 DownloadService 信号 | downloadFailed(hash, error)、stateChanged(hash, state) | 异步失败和状态可见；原完成/进度信号保留 |

### 3.3 测试与门槛

新增 `tests/test_download_metrics.cpp`、`tests/test_download_errors.cpp`、`tests/test_download_creation.cpp`；底层增补现有 [下载测试](../src/librats/tests/test_bt_download.cpp) 与 [磁盘测试](../src/librats/tests/test_bt_disk_io.cpp)。验证重复块、校验失败、重检、只读/磁盘满、删除后回调、磁力元数据完成但内容未启动。引擎错误接口变更必须编译全部 fake 与现有测试。

建立本地可控 Peer、确定性多文件数据、假时钟和不限速吞吐/CPU/内存/写放大基线。核实后台爬取和用户下载复用 Client 的实际路径，书面列出预算归属。门槛：有效进度正确、持久错误不无限重取、受控创建无启动竞态。

## 4. S1：带宽与队列 MVP

### 4.1 待修改函数

| ID/位置 | 函数（M） | 修改内容 |
|---|---|---|
| S1-01 [config_store.cpp](../src/app/config_store.cpp) | ConfigStore::setDefaults、load、save、validateAndClamp、setValue、fromJson、toJson | 新增独立 downloadPolicy 配置段；新入口严格校验，不能复用 clamp 把错误请求静默改值；历史配置走显式迁移 |
| S1-01 [torrent_session_store.cpp](../src/services/torrent_session_store.cpp) | TorrentSessionStore::save、load | 新增文档级 schemaVersion，原子 QSaveFile；兼容旧数组；缺失与损坏返回不同诊断，空任务列表也保留版本文档 |
| S1-02 [torrent.cpp](../src/librats/src/librats/bittorrent/torrent.cpp) | Torrent::refill、on_piece、on_closed、check_request_timeouts、on_request | 下载请求预留预算、到达结算、超时/取消释放、上传请求预算；重复/迟到块也计网络流量 |
| S1-02 [peer_connection.h](../src/librats/src/librats/bittorrent/peer_connection.h) 及实现 | PeerConnection::send_piece、send_message、queue（两个重载）、flush、do_read、on_link_readable、on_link_writable、close | 区分内容帧和控制帧，实际发送结算；接收背压和关闭释放；MSE 密文与内容字节归属不能混淆；控制流量独立小额预算 |
| S1-02 [client.cpp](../src/librats/src/librats/bittorrent/client.cpp) | Client::connect_peer、connect_peer_tcp、connect_peer_utp、adopt_inbound、reap_closed、sample_rates | 入/出站连接统一检查总/任务预算；连接失败与回收释放；TCP/uTP 同一限额，不借 Rats P2P 参数代替 |
| S1-02 [torrent.cpp](../src/librats/src/librats/bittorrent/torrent.cpp) | Torrent::try_connect、tick、refill_all | 响应预算可用事件；不 sleep、不忙轮询；在途字节硬约束 |
| S1-03 [download_service.cpp](../src/services/download_service.cpp) | DownloadService 构造/析构；add、addWithInfo、addFromFile、registerSeed、pause、resume、togglePause、remove | 先创建策略化任务再调度；用户意图与执行暂停分离；移除/销毁释放槽位；旧 bool 方法转兼容封装 |
| 同上 | DownloadService::onUpdateTimer、pollStatus、flushTransitions、saveSession、loadSession、restore | 调度器消费快照；先恢复意图和队列再启动；计时/元数据/校验/做种各自占槽 |
| S1-04 [application.cpp](../src/app/application.cpp) | Application 构造；start、stop、applyConfig、wireSignals | 装配策略/调度器，先加载后启动；配置热更新和关停顺序明确 |
| S1-04 [api_router.cpp](../src/rest/api_router.cpp) | ApiRouter::registerMethods、wireEvents | 接入策略与队列命令，新增事件；respond 恰好一次，旧命令保留 |
| S1-04 [settingsdialog.cpp](../src/ui/settingsdialog.cpp) | SettingsDialog::setupUi、loadSettings、saveSettings、onAccepted | 新增下载页；先验证并等待应用结果，再关闭；失败保持用户输入 |
| S1-04 [downloadswidget.cpp](../src/ui/downloadswidget.cpp) | DownloadItemWidget::setupUi、updateProgress、setPaused、setCompleted；DownloadsWidget::setupUi、loadDownloads、onPauseToggled、onProgressUpdated | 显示排队/意图/有效限制/上传速率，保留现有动作兼容 |

传输适配注意：先使用 PeerConnection 和共用 PeerLink 抽象接入；若 TCP/uTP 的写缓冲无法维持内容预算元数据，则把所需接口加入对应适配类并补合同测试，不能仅限 TCP。具体适配函数以 S0 的调用图为准，不在未核对签名前伪造现有函数名。

### 4.2 待新增函数

| ID/拟新增文件或归属 | 函数（A） | 说明 |
|---|---|---|
| S1-01 `src/services/download_policy.{h,cpp}` | GlobalDownloadPolicy::validate/fromJson/toJson；TaskDownloadPolicy::validate/fromJson/toJson；resolveEffectivePolicy(global, task) | 值域、继承、有效上限与稳定序列化；队列与带宽优先级分离 |
| S1-01 ConfigStore | downloadPolicy() const、setDownloadPolicy(policy) | 整体原子校验/写入；旧 downloadPath()/setDownloadPath() 保留 |
| S1-01 TorrentSessionStore | loadDocument(path)、saveDocument(path, document)、migrateLegacyArray(array)、validateDocument(document) | 结构化加载诊断/修订号；原 load/save 做兼容包装，服务改用文档接口 |
| S1-02 `librats/bittorrent/rate_limiter.{h,cpp}` | TokenBucket::configure/refill/available/consume/nextReadyAt | 假时钟可控、整数溢出检查、桶容量明确；配额不足时预约唤醒 |
| S1-02 `librats/bittorrent/bandwidth_controller.{h,cpp}` | BandwidthController::set_global_policy/set_task_policy/reserve_download/settle_download/release_reservation/acquire_upload/commit_upload/next_ready_at/remove_task | 全局/任务双层预算事务；请求取消和 partial send 不重复消费，重传不免费 |
| 同上 | BandwidthController::select_next_task、grant_quantum、snapshot | 公平调度、低优先级最低机会、预算诊断；S0 冻结权重和等待界限 |
| S1-02 `librats/bittorrent/connection_budget.{h,cpp}` | ConnectionBudget::try_acquire/release/update_limits/snapshot | 建连中和已建立连接均计数；句柄式 reservation 保证失败路径释放 |
| S1-02 Client | set_global_policy、set_task_policy、wake_bandwidth_waiters | reactor 应用并返回确认；wake 使用定时器/可用事件，不能死循环 |
| S1-02 PeerConnection | set_content_budget、set_read_budget、on_budget_ready | 提供传输无关接入；不得使握手与必要控制消息饥饿 |
| S1-02 TorrentEngine | setGlobalPolicy、setTaskPolicy、effectivePolicy | Qt↔引擎映射；异步/同步应用结果区分清楚 |
| S1-03 `src/services/download_scheduler.{h,cpp}` | DownloadScheduler::upsertTask/removeTask/setLimits/moveQueueItem/evaluate/nextWakeup/restoreQueue | 纯策略输入快照和单调时间，输出 Start/Pause/StartMetadata/Recheck 指令；不直接操作 QWidget 或磁盘 |
| 同上 | DownloadScheduler::allocateSlots/releaseSlots/classifyTask | 下载/做种/元数据/校验分别预算；用户暂停优先于调度意图；stalled 冷却有上限 |
| S1-03 DownloadService | enqueueDownload、setGlobalPolicy、globalPolicy、setTaskPolicy、effectivePolicy、moveQueueItem、reconcileSchedule、applySchedulerActions、setUserIntent | typed 新入口；旧 resume 只能提交运行意图，不能绕过队列 |
| S1-03 DownloadService | persistPolicyChange、completeOperation、operationStatus | 引擎确认后提交快照和 revision；保存失败返回 appliedNotPersisted；短期 operation 结果可查询 |
| S1-04 ApiRouter | registerDownloadMethods、parseDownloadPolicy、parseExpectedRevision、replyDownloadResult | 从原 registerMethods 中抽出下载注册；命令兼容与结构化响应统一 |
| S1-04 SettingsDialog | createDownloadsTab、validateDownloadSettings、showDownloadSettingsError | 单位换算、非法值可见、有效设置展示 |
| S1-04 DownloadsWidget / DownloadItemWidget | onQueueMoveRequested、onTaskPolicyRequested；updateState、updateEffectiveLimits、updateTransferStats | 列表更新与用户操作；新增 `TaskDownloadDialog::loadPolicy/validateInput/submitPolicy/showApplyResult`（拟建 UI 类） |

S1 API：download.policy.get/set、download.taskPolicy.set、download.queue.move；增加 download.operation.get 查询异步操作终态，作为原 operationId 契约的配套接口。旧 download.add/pause/resume/remove/list 不删除。采用 expectedRevision 检测并发，元数据/文件完整校验前不得提交半份策略。

### 4.3 测试与门槛

新增 `test_download_policy`、`test_download_scheduler`、`test_download_session`、`test_download_api`；底层新增 `test_bt_rate_limiter`、`test_bt_bandwidth_controller`、`test_bt_connection_budget`。覆盖令牌桶时钟跳变/溢出/继承、预约回滚、TCP/uTP、partial send、MSE、end-game、10 任务限 3、降低限额、用户暂停、stalled 冷却、移除中回调、配置保存失败、旧会话与重启。

验收沿用原方案 R×T+B+I 和 60 秒饱和流量标准，必须公开 B/I；不以 GUI 速率显示代替真实字节测量。修订冲突不改任何字段；引擎已应用但保存失败有明确提示。S1 门槛通过后可独立交付，但文件勾选必须继续明确标注未提供真实选择能力，不能宣传 S2 已完成。

## 5. S2：真实文件选择

### 5.1 待修改函数

| ID/位置 | 函数（M） | 修改内容 |
|---|---|---|
| S2-01 [piece_picker.h](../src/librats/src/librats/bittorrent/piece_picker.h) 及实现 | PiecePicker::set_piece_priority、pick_blocks、is_interesting、piece_interesting、restore_piece | 热变更后的 wanted 状态、在途请求和 end-game 一致；不把跳过分片重新塞回请求队列 |
| S2-01 [disk_io.cpp](../src/librats/src/librats/bittorrent/disk_io.cpp) | ThreadedDiskIo::ensure_file、file_path、write_range、read_range、async_hash、async_check_files | 已选区间写最终文件，未选边界区间写 sidecar；同一映射用于校验/上传/恢复；不预分配未选文件 |
| S2-01 [torrent.cpp](../src/librats/src/librats/bittorrent/torrent.cpp) | Torrent::on_request、verify_piece | 上传/校验读取完整 piece 的统一映射，不遗漏 sidecar 数据 |
| S2-02 同上 | Torrent::is_complete（头文件内联）、progress、on_piece_hashed、on_check_complete、make_tracker_request、announce_trackers、update_interest、recompute_choker | 完整完成与所选完成分开；只对完整分片发 HAVE；Tracker left 仍按整个种子；部分任务可以上传但不冒充完整 seed |
| 同上 | Torrent::load_resume_data、generate_resume_data、try_load_resume_data、save_resume_data（两个重载） | 保存选择修订与边界存储标识，恢复后验证原始 hash/文件映射；取消选择不删除数据 |
| S2-02 [resume_data.cpp](../src/librats/src/librats/bittorrent/resume_data.cpp) | ResumeData::encode、decode | 可选新增字段、旧格式读取、损坏与不匹配处理；have-bitfield 不等于文件选择 |
| S2-02 [client.cpp](../src/librats/src/librats/bittorrent/client.cpp) | Client::torrent_status | 新增所选/全部完成和每文件有效字节；同一次快照中选择版本一致 |
| S2-03 [download_service.cpp](../src/services/download_service.cpp) | DownloadService::selectFiles、selectFilesJson、applySnapshot、pollStatus、flushTransitions、setRemoveOnDone、restore | 旧选择调用转 typed 核心；引擎确认后发布选择；快照不再 clear 后把全部文件强设 selected=true；旧 removeOnDone 仅完整完成 |
| 同上 | DownloadFile::toJson、Download::toJson、filesToJson、progressJson | 序列化优先级、wantedBytes、wantedVerifiedBytes、selectedComplete/fullTorrentComplete、selectionRevision |
| S2-03 [torrent_session_store.cpp](../src/services/torrent_session_store.cpp) | TorrentSessionStore::saveDocument、loadDocument、validateDocument（S1 新增后修改） | 文件索引稳定、元数据身份校验、选择确认状态；旧任务默认暂停待确认 |
| S2-03 [torrentfileswidget.cpp](../src/ui/torrentfileswidget.cpp) | TorrentFilesWidget::setFiles、setTorrent、setupUi、buildFileTree、addTreeNodeToWidget、onFileItemChanged、clear | 三态目录、多选、优先级和应用中状态；保留真实 fileIndex；仅浏览结果不错误修改活动任务 |
| S2-03 [downloadswidget.cpp](../src/ui/downloadswidget.cpp) | DownloadsWidget::onDownloadStarted、onDownloadCompleted、onProgressUpdated、loadDownloads；DownloadItemWidget::setCompleted、updateProgress | 新增所选完成展示；旧 full completed 信号与兼容 UI 路径保留 |
| S2-03 [api_router.cpp](../src/rest/api_router.cpp) | ApiRouter::registerDownloadMethods、wireEvents（S1 新增/已有） | files.set/recheck/retry、所选完成事件；旧 selectFiles 变成 skip/normal 适配 |

### 5.2 待新增函数

| ID/拟新增文件或归属 | 函数（A） | 说明 |
|---|---|---|
| S2-01 `librats/bittorrent/file_selection.{h,cpp}` | FileSelection::validate/set_priorities/build_piece_priorities/wanted_size/verified_file_bytes/selected_complete | 按元数据区间计算，跨文件取最高优先级；空文件与全不选分别定义；索引/重复/溢出拒绝 |
| S2-01 `librats/bittorrent/boundary_store.{h,cpp}` | BoundaryStore::open/read/write/flush/load_manifest/save_manifest/validate_manifest/storage_bytes | `.parts/<hash>/` 稳定边界存储，原子 manifest；校验、上传与重启共用 |
| 同上 | BoundaryStore::plan_selection_change/apply_selection_change/cleanup_unreferenced | 切换选择前计算迁移，在停写屏障后应用；先提交新映射再清理无引用 sidecar，默认不删最终内容 |
| S2-01 ThreadedDiskIo | map_storage_slices、apply_file_selection、drain_writes、storage_snapshot | 将逻辑区间映射到最终文件/sidecar；版本化读写，防止旧回调按新映射落盘 |
| S2-02 PiecePicker | wanted_complete() const、all_pieces_complete() const、cancel_unwanted_requests | 保留 is_finished 的旧 wanted 语义，新增明确方法避免新代码误用 |
| S2-02 Torrent | set_file_priorities、cancel_unwanted_requests、selected_complete、full_complete、file_progress、recheck、retry、on_selection_applied | 原子选择、部分状态、重检和可控重试；回调携带修订号/操作标识 |
| S2-02 Client | set_file_priorities、recheck_torrent、retry_torrent | reactor 调度并确认，避免暴露线程不安全 Torrent* |
| S2-02 TorrentEngine | setFilePriorities、recheck、retry | Qt 层转换与错误/操作结果映射 |
| S2-03 DownloadService | setFilePriorities、confirmSelection、recheck、retry、applySelectionResult | 校验与元数据确认门禁；全不选进入等待选择；更新成功后再推进 revision |
| 同上信号 | selectedCompleted、filesChanged、operationFinished | 原 downloadCompleted 继续表示全种子完成；一次选择版本仅发一次所选完成转移 |
| S2-03 `src/ui/add_download_dialog.{h,cpp}` | AddDownloadDialog::loadSource/showMetadata/setFilePriorities/validateSelection/submitTask/showOperationResult | 磁力先元数据后选择，种子文件立即显示；默认确认前无内容请求 |
| S2-03 TorrentFilesWidget | setDownloadFiles、collectFilePriorities、submitFilePriorities、applyFileSnapshot、setApplying、setFilter、selectByExtension | 浏览用 setFiles 保留；活动任务用新接口展示真实状态和文件进度 |
| S2-03 DownloadsWidget | onSelectedCompleted、onRecheckRequested、onRetryRequested、onSelectionRequired | 绑定新对话框与操作终态，不用 UI 自己猜测完成 |

一致性事务：校验输入/expectedRevision → reactor 停止相关新请求 → 排空或隔离旧版本写任务 → 应用文件映射/优先级 → 返回确认 → 更新服务 revision → 原子持久化 → 发布事件。任何步骤不得在服务 mutex 内同步等待 reactor；持久化失败不回滚已经生效的引擎状态，按 appliedNotPersisted 告知并允许重存。

### 5.3 测试与门槛

新增 `test_download_file_selection`、`test_download_operations`；底层新增 `test_bt_file_selection`、`test_bt_boundary_store`，扩展 [test_bt_disk_io.cpp](../src/librats/tests/test_bt_disk_io.cpp)、[test_bt_download.cpp](../src/librats/tests/test_bt_download.cpp)、[test_bt_resume_data.cpp](../src/librats/tests/test_bt_resume_data.cpp)。

必须覆盖：只选中间文件且 piece 跨两个边界；不同选择共享 piece；零字节与全不选；切换时写盘未完成；取消后迟到数据；高优先级资源不可用；重启/损坏 sidecar；UTF-8/非法 Windows 名称；重检已删除文件；所选完成后继续上传但不发完整完成通告。额外数据上界按“所需 piece 并集−所选区间并集”计算，不能声称未选文件相关字节绝不传输。

S2 门槛：最终路径无未选文件、所选文件逐字节匹配、恢复不先全选、Tracker left 和 Peer bitfield 正确、旧 API 客户端测试通过、不限速基准吞吐下降目标 ≤5%。

## 6. 待删除函数与兼容性清理

| 分类/阶段 | 函数（D） | 删除条件/替代 |
|---|---|---|
| 私有，S0/S1 收尾 | [DownloadService::computeSpeed](../src/services/download_service.cpp) | updateRates 已接管全部调用且旧 JSON 速率合同测试通过后删除；不能先删后补 |
| 私有，S2 收尾 | [TorrentFilesWidget::collectSelectedFiles](../src/ui/torrentfileswidget.cpp) | collectFilePriorities 已统一三态/稳定索引收集，确认不存在其余调用后删除 |
| 私有，S2 收尾 | [TorrentFilesWidget::getSelectedFileIndices](../src/ui/torrentfileswidget.cpp) | 同上；旧 selectFiles API 保留，UI 不再维护另一套 bool-only 收集逻辑 |
| 对外函数 | **本轮不删除** | DownloadService::add/addWithInfo/addFromFile/pause/resume/selectFiles/selectFilesJson、TorrentEngine 旧创建接口、TorrentSessionStore::load/save 均保留薄兼容层 |
| 底层兼容函数 | **本轮不删除** | PiecePicker::is_finished、Torrent::is_complete 继续存在，文档明确语义；新代码用明确的 full/selected 接口 |

需移除的是旧行为分支而非对外函数：服务“只改 selected 即返回成功”、恢复“强制保留已完成”、添加“自动启动后再补配置”、完成“所有条件合成一个 completed”、错误“永久 I/O 失败无限重取”。逐项迁移后删除相应分支与过期注释。

函数签名迁移注意：DiskIo 的虚接口和 callback 类型变化属于底层源码级兼容变更，必须同步所有实现；S0 冻结是否增加新详细结果接口并保留 bool 适配，不能把移除旧虚签名当成无影响的内部改动。

## 7. S3 后续函数清单（不在本轮确认范围）

| 类型 | 拟函数 | 说明 |
|---|---|---|
| M | PiecePicker::set_mode、pick_blocks；Torrent::refill；ThreadedDiskIo::write_range/read_range | 顺序/首尾倾向和有界窗口/缓存，仍不修改元数据 piece 长度与协议 block 大小 |
| A | Torrent::set_download_strategy、piece_ranges、set_request_window | 控制策略、聚合分片图、自动/手动窗口上下限 |
| A | RequestWindowPolicy::computeWindow；DiskCacheBudget::acquire/release/evict | RTT/带宽/内存背压共同约束；先压测再开放高级参数 |
| A | BandwidthSchedule::resolve/nextChange；SeedingPolicy::shouldStop | 时间计划、时区/夏令时与做种比例/时长；不默认删除文件 |
| A | DownloadService::setSchedule/setSeedingPolicy；PieceMapWidget::setRanges；TaskDownloadDialog::loadAdvancedPolicy | 高级配置与展示，API/会话需再迁移 |
| D | 无预定删除 | 待单独技术审查再确定 |

## 8. 提交单元与发布检查

建议按 S0-01…S2-04 划分可审查提交；接口变更连同调用方/fake 必须放在同一可编译单元，不能机械按“只改底层”产生中间破坏提交。每个阶段保留测试报告及已知限制；代码提交和外部发布仍遵循用户后续明确指令，不在本计划阶段执行。

实施时更新 [tests/CMakeLists.txt](../tests/CMakeLists.txt) 注册 Qt 测试和 check 依赖，按 librats 测试注册方式加入底层用例；新增 C++ 文件利用现有 CMake CONFIGURE_DEPENDS，但验证资源/翻译显式条目。更新 [API 文档](API.md)、[使用手册](MANUAL.md)、[服务器说明](SERVER.md) 和翻译中涉及的新行为。

更新 [构建脚本](../scripts/build-windows.ps1) 以保存依赖补丁并运行必要的底层测试，不能仅运行父项目 Qt 测试就宣称全部底层测试通过。依赖补丁分组：现有 UTF-8 修复独立保留；新增下载能力使用独立补丁或已发布且锁定的 librats revision，均验证对干净版本的应用/重复应用。原 [UTF-8 补丁](../patches/librats-windows-utf8.patch) 不以整库新 diff 覆盖。

发布包必须在临时解压目录验证：工具链不在 PATH、Qt/MySQL DLL 完整、中文路径、策略持久化；使用本地可控 torrent 下载作最终烟测。正式用户目录与现有数据不参与测试。各阶段保留带版本/阶段标记的包，避免未通过验收时覆盖已交付的稳定 ZIP。

## 9. 待人工确认清单

- [x] 确认本轮执行 S0–S2，S3 只作后续规划。
- [x] 确认上述 M/A/D 函数清单与兼容入口策略，允许在 S0 冻结底层签名并同步所有调用者。
- [x] 确认三项私有函数仅在替代实现通过测试且无调用后删除；本轮不删除公开 API。
- [x] 确认按阶段验收与暂停门槛执行，S0 后重新评估 20–32 人日预算。
- [x] 确认新增 operation.get 是 operationId 契约的配套接口，统一 API 回应恰好一次。
- [x] 确认待实施后再更新依赖补丁、文档与阶段构建包；本阶段仅提交执行计划。

人工确认结论：待填写。确认人：待填写。批准开始阶段：待填写。
