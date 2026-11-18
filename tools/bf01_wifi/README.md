# BF01 新协议通信层

2026-11-09 正式接入：通信核心、SDIO适配已移入 `solutions/ipcamera/src/bf01_wifi_link.c`、`solutions/ipcamera/src/bf01_wifi_sdio.c`，由原应用Makefile编译进同一个 `ipcamera`。正式常驻入口为 `ipcamera --wifi-service`，控制入口为 `bf01-wifi`。最新部署和验收边界见 `WIFI模块通讯/主控与Wi-Fi实机联调结论.md`。下面“独立/未链接/尚未接入”的段落是本次正式接入前的隔离阶段记录，不作为当前集成状态。

依据 `WIFI模块通讯/主控与WIFI属性联动协议.md`、`WIFI模块通讯/主控联调测试例子.md` 重新实现。

## 统一参数文件（2026-11-17）

APP可变参数的唯一生效文件已统一为 `/mnt/data/ipcamera-config/app-settings.json`（有真实CFG挂载时为 `/mnt/cfg/ipcamera/app-settings.json`），详见仓库根目录 `APP_SETTINGS.md`。新增 `bf01-wifi settings` / `ipcamera --settings-show` 查看同一份配置；音量、麦克风硬件设置、播放恢复和录音PCM策略也直接读取该文件，不再各存一份。旧 `event-policy.json`、`speaker-volume`、`microphone-enabled` 仅在统一文件不存在时迁移一次，迁移后不再读取或更新。统一文件损坏时明确报错，不回退旧值。

## APP 参数接入（2026-11-17 统一版，待下次启动验收）

- 沿用协议的 15 个主控设置键识别全量 `set`；后续单键、多键增量只合并收到的字段，不清空其余设置。首次全量完成之前的增量仍按握手约定返回 ACK2；未知键忽略，已知键类型、范围或日期格式不合法返回 ACK1。同 ID 重试沿用原有幂等机制。
- 15 个主控设置键（包括未实现的预留设置）经统一管理器合并、原子保存到 `app-settings.json`。重复同值不重写文件。只收到精准率、时间戳等单项时，不用默认 AI 开关或录像时长覆盖本地原有默认配置。
- AI 开关继续使用现有启动门控，不改变抓拍策略、直播优先级或当前正在执行的 AI 任务；新值供下一次抓拍启动使用。`image_filter_level` 的 `low/medium/high` 分别映射真实推理置信度阈值 `0.30/0.60/0.80`，不是识别准确率保证。未收到此键时保留本地 `boot_ai.ini`，启动日志显示实际阈值和来源。
- `pir_record_dur=0/10/20/30` 使用现有录像入口。`-1` 的 AI 动态时长尚无后端：按本次用户要求保存并 ACK0，但在 `host_deferred_fields` 标记，当前明确固定回退为 10 秒，不把它描述为 AI 已决定时长。
- 日期格式映射：`yyyy-MM-dd HH:mm:ss` → 0，`MM/dd/yyyy HH:mm:ss` → 1，`dd-MM-yyyy HH:mm:ss` → 2。Linux 相机启动后及运行期间每秒检查已保存策略，通过原 IPC 通道通知小核；小核确认配置接收后，在现有时间戳更新点切换开关和格式。`bf01-wifi camera-status` 的 `datetime_policy_errno` 为 0 表示策略已接收或没有覆盖配置；非 0 明确表示未确认，不能用 Wi-Fi ACK0 代替画面验收。此项必须配套重新编译、打包小核和应用，旧小核不支持新消息。
- **早期首拍边界**：Linux 启动前已经拍下的 RTOS 照片仍使用小核内置 `[boot_timestamp]` 配置，不能追溯改水印；本轮没有新增小核跨断电配置存储。当前不是“断电重启首张照片也已跟随 APP 设置”的交付结论。
- `pir_sens_level`、`pir_detect_intv`、`brand_wm_switch` 等未实现设置也持久化，并在本次处理的 `host_deferred_fields` 中标记；按用户本轮要求保存成功才暂回 ACK0，不代表真实硬件动作已完成。服务重启后的保存结果以直接读盘的 `host_saved_fields` 为准。`status_led_switch` 沿用未单列灯的原接收逻辑。`memory_format` 不属于设置占位，缺少后端仍 ACK2，绝不假装格式化成功。现有扬声器、麦克风后端保持不变，防闪烁补充启动恢复，真实执行错误仍拒绝成功。
- `host_applied_fields` 的策略类字段表示已经持久化，不表示当前 AI 任务或早期首拍已变更；`event_policy_effect` 给出生效边界。编译/模拟验证与 ADB、COM9 实机验收分别记录，不互相替代。
- 统一参数版验证：应用 ARM 编译、链接、管理器和音频适配器严格告警检查、脚本语法及差异格式检查通过；临时隔离回归覆盖全量/增量合并、同值不重写、独立进程重读、旧文件迁移、忽略过时旧值、并发写入与PCM锁、损坏配置拒绝回退、磁盘/同步/重命名错误，音频回归同时通过UBSan。17:42已通过ADB备份并原子替换板端应用和脚本，未停止进程、未重启、未提前生成统一文件；下次正常启动才迁移。旧版ADB/COM9结果不作为统一版业务验收；本轮未改小核、未验证统一版APP下发及实际媒体效果。

## 先前生产行为记录

- BF01 的 `make ... install` 同时把正式应用、`bf01-wifi`、`wifi/bf01-sdio-up.sh` 和 `auto.sh` 同步到 SDK `OUTPUT_DIR/data/`，避免应用目录已更新而 `pack_data` 仍打包旧程序。不会覆盖 SDK data 中既有 `param_config.ini`、`capture.ini`、`boot_ai.ini`；已有镜像仍须重新执行 SDK 数据分区打包，安装不等于烧录验收。
- `bf01-wifi start|stop|status|get NAME|refresh-http|filter|camera-status|module-time|speaker-status|microphone-status|sync-clock` 管理真实应用服务；`get NAME` 通过现有生产服务串行排队查询，结果以该服务 wire 日志的对应 ID 应答为准，不另开 SDIO 消费者；`get http` 使用原凭证刷新流程。`filter` 只读模组 TCP 过滤表；`camera-status` 通过原相机进程读取 ISP/录像；`module-time` 读取无时区 native tm，不直接写 UTC；`speaker-status`、`microphone-status` 经同一正式服务只读 DAC/ADC 双声道增益、静音和已应用设置。
- `start` 在驱动控制接口可用并启动网络 watcher 后立即启动协议服务，不再等待 Linux IP 同步；联网仍由模组负责，watcher 同步主控地址、路由和 DNS。后台校时等真实 `watcher.ready` / `network.ready` 后开始，最多等待 30 秒；时间无效时每轮 supervisor 检查会尝试启动同一独占校时任务，网络晚到或校时失败可自动重试，不依赖客户命令行。
- 校时优先走已有正式 `ipcamera --wifi-sync-time` 的 `time.nist.gov:37` TCP 路径（外层限时 8 秒），检查范围并读回 UTC；失败后才有界尝试 NTP 20 秒，已有有效时间不覆盖。启动锁、校时锁防止重复执行，协议服务不等校时任务结束。该无认证时间源不代替凭证或身份验证；网络不可达明确失败，不放开原来的 report/HTTP 时间门禁。`sync-clock` 仍可由研发侧重试同一路径。
- 凭证刷新 ACK2 是一轮完整回复。正式程序等 30 秒后换新 ID 最多再请求两轮，不用原 ID 冒充重新刷新。没有收到回复时才按原 ID 重试。
- 自动刷新单独检查已经确认会话中的凭证到期时间；即使旧凭证已过期，仍按既有退避重新请求，不会因 `http_usable=false` 永久停止刷新。过期凭证仍不向业务开放，缺少全量 `report` 成功确认或尚未取得凭证时不启动该刷新；`status.http_refresh_due` 只读显示此条件。
- 单条请求无回复不会清空其他排队请求或已经取得且未过期的凭证。`completed` 在重试耗尽时收到空回复，调用方须检查。
- APP 设置统一增量合并保存到真实已挂载配置分区的 `/mnt/cfg/ipcamera/app-settings.json`；没有 CFG 分区时使用 `/mnt/data/ipcamera-config/app-settings.json`，不写 `/run` 冒充持久化。全量、单项均只覆盖本包字段，缺失键保留，未收到的键不写默认值。原 INI 保留为未设置字段的默认配置，不以 INI 文件未变化判断 APP 设置丢失。
- 保存使用互斥锁、临时文件、文件 `fsync`、原子替换、目录 `fsync` 和 JSON 读回校验；重复同值不重写。保存失败回 ACK2，不能把只更新内存算成功。`status.host_settings_path`、`host_settings_errno`、`host_saved_fields` 直接读取本地文件；`wifi_saved_dps` 仅为模组状态，`host_applied_fields` 仅为本次服务实际处理状态，不代表所有设置已在硬件生效。
- AI 开关、识别阈值（低30%/中60%/高80%）、通知分类和录像时长由现有消费点读取该 JSON，下次启动不依赖模组再次下发；仅设置阈值不会覆盖原 INI 的 AI 开关或时长。`pir_record_dur=-1` 仅保存为预留自动时长，实际暂按10秒回退。时间戳开关及格式保存，但首拍时间戳生效时序另行处理；本轮不改变直播及其水印。
- `image_anti_flicker` 通过原相机控制桥接到 ISP Get/Set/Get；相机暂不在线时保存后延后执行，下次正常相机初始化时从本地恢复并读回确认，日志明确记录恢复错误，不能以保存成功代替 ISP 生效。`status_led_switch` 按协议对未单列灯的板型接收，不代表实际点灯。
- `spk_vol` 使用已有 `/dev/cvitekadac` 双声道 Set/Get 并读回确认，再持久化音量；不初始化 AO/AI、不操作 ADC 和功放 GPIO。真实全量 `set` 的 `80` 已将增益从 `32/32` 改为 `26/26`；重复同值不写硬件或设置文件。原播放 AO 初始化后恢复已应用音量，没有设置文件时保持原逻辑；寄存器读回不等于已经验证实际播放。
- `mic_switch` 使用 `/dev/cvitekaadc` 只改左右静音位，Get/Set/Get 验证增益未变后持久化；错误不确认成功，尚未安装新策略时尝试恢复原静音位。实际收到 `false` 后读回左/右原生静音值1/2、增益保持9/0。BF01录像的RTOS和Linux两路PCM在原AAC写入点共同执行录音策略：关闭时用等长零样本保持PTS，未收到配置时保持原音频，不改变抓拍触发或录像时长。`app_ipcam_Microphone_GuardFrame()` 返回持有共享锁的文件描述符，调用方须在本次 `app_mp4_write_pcm()` 结束后关闭；设置使用互斥锁，避免确认后还有新读出的未静音PCM写入。损坏策略明确失败，不放开声音。实际MP4静音和恢复录音仍需正常相机流程验收，不能拿ADC位读回代替。
- 按本次 APP 设置预留约定，尚无执行后端的 PIR 灵敏度/间隔、品牌水印等设置也落盘，保存成功后才可回 ACK0，并记入 `host_deferred_fields`；这不代表其业务功能已实现。`set_not_applied.field_errno` 为主控本地逐字段错误码：`38` 是执行后端未实现，`image_anti_flicker:2` 是本地相机套接字不存在，不是协议 ACK2 的另一个名字。格式化等瞬时动作不保存、不重放，不享受占位 ACK0；真实硬件错误仍回 ACK2。音量和麦克风保留真实硬件及恢复/录音策略，但持久化与读取全部改为统一JSON，不再维护音频独立参数文件。
- 初次加载不再默认等45秒：先注册 cfg80211 与 SDIO 驱动，再等待 `mmc1` 的 `007a:6011`；支持先注册、后枚举自动绑定，不因已加载但尚未枚举而直接失败。未枚举时最多每2秒重扫一次，不重复加载、不解绑。Linux/RHEA/SDIO驱动先完成绑定并返回，后台独占worker反复做真实握手：检查总线宽度、必要时恢复SDIO通信、启用function并恢复块大小，再走原生WSM初始化；单轮WSM通知窗口1秒，失败清理线程/工作队列/缓存，清理完成1秒后重试，无总次数限制。总线操作和清理另有耗时，不代表整轮严格2秒。握手成功不再重试，不卸载模块、不解绑、不重置模组CPU。`bf01_startup_state` 显示 `handshaking/retry/ready/stopped` 及轮次/错误码；helper等真实ready后仍检查 `wlan0` 和控制接口，才启动网络watcher、协议和自动校时。枚举/绑定共享60秒限时，握手另有60秒限时，使用 `/proc/uptime`，不随校时变化；helper超时不停止驱动worker，后续supervisor继续检查。`startup-not-before` 保留显式研发下限，默认0；`network-start` 不等IP，`network`仍等真实IP。
- 前两轮启动候选已被冷启动否定：先是短窗口超时，再是60秒等不到WSM；后一轮uptime25.9秒实收CMD52的IOEx/IORx/IF均为0，不能继续将可枚举当软件已就绪。当前后台重握手方案等待整机冷启动验收，软件测试不代替模组实机通知、联网或APP业务通过。仅主控重启/模组保电仍需独立验证。

## 直播唤醒启动优先级（2026-11-16）

- BF01 暂时通过 IR-CUT 模块的板级编译宏 `BF01_IRCUT_GPIO_DISABLED` 屏蔽未配置的机械滤光片及该模块的 IR LED GPIO 操作，启动不再执行 500 毫秒切换等待；手动/自动切换和退出、OTA 清理均不操作这些 GPIO。仅限 `CONFIG_RESOURCE_INSTALL_FBM_CV1815JA_BF01_GC4683=y`，其他板型原行为保留。模块继续编译并保留 `/tmp/day`、`/tmp/night` 和软件日夜图像参数切换，不改 AI、时间戳、GPIO29 关机监测或 Wi-Fi 端红外对管检测。后续接入真实机械 IR-CUT 时，需核实硬件和 GPIO 配置后撤销模块 Makefile 中的屏蔽宏，并重编该模块。
- 首条完整有效 RTSP start 立即交给真实后端；若正在启动，通信层使用最长 2500 毫秒的异步确认窗口，不立即回复 ACK2，也不阻塞接收其他指令。真实首帧已完整发送成功则直接回复原 ID 的 ACK0，不需要模组重发。窗口到期仍未开播则回复 ACK2，内部待处理流程及模组正常重试保留；业务失败按真实结果回复，绝不提前假回 ACK0。窗口使用单调时钟、短于协议 3 秒重发周期，但总线卡顿或网络超时下不承诺首包必成功。
- 同 ID 同内容在等待期间不重建动作或延长截止时间，成功后重发只回缓存确认；同 ID 改内容拒绝。新的有效 stop/start 到达后终止旧 start 的等待确认，不能因稍后轮询旧请求重新开播。传输重试只补发原 ACK，重置链路会清除待确认对象。业务回调只有返回 `EINPROGRESS` 才启用此窗口，原 0/1/2 行为保留；无后端仍返回 ACK2。相机直播 IPC 和状态查询等待上限 100 毫秒，其他配置 IPC 保留原 1500 毫秒上限。
- 首条确认窗口改动只涉及应用 DATA；已经烧录通信优先 ROOTFS 的设备不需再次重编 BSP/驱动/小核，应用 install 后 SDK `pack_data` 并按原流程生成发布包即可。尚未烧录通信优先 ROOTFS 的设备仍需下述 ROOTFS + DATA 配套更新。
- 新固件通过 DATA 中的 `bf01-boot-priority` 与 BF01 板级 ROOTFS 配套启用：挂载 DATA 后由 `S11bf01` 提前启动，只加载 SYS、IPCM、看门狗，再优先启动 cfg80211、SDIO 和正式通信服务；看门狗和原关机保护不后移。旧 DATA 没有标志文件时仍走旧启动顺序。
- 相机启动前优先让通信服务建立 `control.sock`，等待上限 8 秒，不等待云端凭证或全量设置确认。校时进程在通信服务就绪后异步启动，保留原有校时与凭证有效期检查。
- TPU 仅在本次确需 AI 时由应用请求 `bf01-modules ai` 加载；IVE、温控、普通网络初始化、NTP 服务、SSH、日志、USB/ADB 放到相机控制循环就绪后。直播启动时再等本次开机首次真实视频帧完整发送成功；后台延后预算为 `rcP` 进入后 12 秒，超时仍恢复诊断服务，不以此认定直播成功。
- 收到 RTSP 直播 start 后保留本次开机的 `boot-media-cancelled` 标记，取消尚未完成的 AI 子进程，丢弃 RAM 中的启动照片和录像，取消本次拍录落盘。即使随后 stop，本次启动拍录也不补写，下一次重启自动恢复普通开机策略；不会改写 `ai_enabled` 或云端开关。命令到达前已经写入 SD 的文件不属于“尚未落盘”，不会承诺撤销已完成的写入。
- 相机控制套接字使用 `poll` 与 `signalfd` 及时唤醒；直播待处理重试间隔 100 毫秒，已启动状态轮询 500 毫秒，空闲 2 秒。已有相机进程或 IPCM 所有者时不再额外启动一个竞争实例。仍按真实状态回 ACK2/ACK0，不用 ACK2 或 RTSP 握手 200 代替首帧成功。
- 保留时间戳开关、格式及现有水印策略；专用计时打印和测试记录不纳入正式提交。
- 此次涉及 ROOTFS 和 DATA，不能只编译 osdrv。应用执行原有 `make ... install` 后，在已选好 BF01 板型的 SDK 环境执行 `pack_rootfs`、`pack_data`，再按原发布流程 `pack_upgrade`；不要使用会清理 DATA 的应用侧 `build_cvitek.sh pack_data`。无须为本次改动重编 U-Boot、内核或小核。
- 隔离回归和编译检查不能代替新固件冷启动、APP 休眠唤醒首帧、普通 AI 拍录上传及关机的整机验收；不以历史结果确认本次版本通过。

## 历史隔离阶段安全边界

这里是可单独构建的库，不参与原 `ipcamera` 的源文件扫描、链接、安装或启动。未修改 AI 门控、抓拍间隔、录像时长、音频、SD 卡管理、水印、现有预览和时间戳代码。原应用与当前 Git 暂存区保持一致。

没有启动脚本、自动配网、驱动热重载、WSM 启动绕过、硬编码能力数据或恢复出厂动作。原始协议文档和历史联调记录保留；历史记录不代表这份独立实现已通过实机验收。

## 实现内容

- `wifi_link.c`：串行会话状态机。通过 `receive` 接收异步指令，经 `exchange` 返回同 ID 确认，不发空字符串，不用 `get/dps` 代替主动全量 `set`。
- 全量 `set` 实际应用成功并完成确认后才生成全量 `report`；该 `report` 获得自己的成功确认后，才接受主动 `http`。HTTP 凭证在确认交换成功且返回空串后对调用方开放。
- 增量设置不重建 HTTP 会话；新全量设置成功应用后重新同步。过滤不属于主控的未知设置，校验已知键。没有应用后端则返回 `2`，绝不把保存 JSON 当成硬件已生效。
- 终态指令缓存同 ID 确认；相同 ID 携带不同内容拒绝。缓存外的已完成旧 ID 不再执行。`2` 表示没有开始动作，可在同 ID 重试时重新检查就绪状态。
- 主控 `get/report/file/stream state` 每次交换只接受自身 ID 的 `ack`。成功的 `get` 必须含对应命名对象。请求间隔 3 秒、最多发送 3 次，超时撤销凭证可用状态，不拆 SDIO。
- 主动 `stream/start/stop` 交给独立回调；校验 RTSP/RTMP、地址、通道、码流和有效期。不构造平台 start，不把本地 RTSP 预览服务器当作 RTSP 发布实现。
- `wifi_sdio.c`：`_IOR(121,90,unsigned int)` 只读取驱动本地的 `0x083B` 队列；`_IOWR(121,53,unsigned int)` 发真实 `0x003B` Direct Buffer。

## 后端契约

使用 `wifi_link.h` 的 `bf_wifi_ops` 注册真实后端。一个会话只由一个线程调用，不在回调中重入同一个会话。

| 回调 | 要求 |
| --- | --- |
| `receive` | 无异步报文返回 `EAGAIN`，不能通过发送空字符串向模块取指令 |
| `exchange` | 成功返回 0，回复写入带结尾 NUL 的缓冲区；传输错误返回正 errno |
| `apply` | 只在真实应用完成后返回 0；尚未执行返回 2；拒绝返回 1。成功的动作不会因 ACK 传输失败重复调用 |
| `stream` | 不改动现有相机初始化；尚未就绪返回 2，拒绝返回 1；不允许未推流就返回 0 |
| `snapshot` | `report` 返回完整只读属性；`status/stream` 返回对应对象。对象所有权交给通信层，不用默认值冒充硬件读数 |
| `completed` | 只接收正确匹配的终态确认；回调内不得保留临时 JSON 指针 |

缺少 `apply/stream/snapshot` 时不会启动媒体操作，也不会假确认成功。现在尚未把这些回调接回原应用；这是保护已完成功能的隔离边界，不是自动联网/直播交付版。HTTP 对象是会话持有的只读借用对象，不跨 `step/reset/destroy` 保留指针。正式代码不输出凭证和推流地址。

## 构建

### GPIO 低电平关机

GPIOA29 拉低后，应用调用随固件安装的 `bf01-poweroff`：先设置本次启动的关机标志，禁止相机/Wi-Fi 自动重启、SDIO 重新加载和校时启动；向 SDIO 的 `bf01_startup_state` 写入 `stop`，先拒绝新的传输、取消握手并唤醒未完成的 WSM 命令，再等待 SDIO 启动控制锁释放，停止持有 `/dev/atbm_ioctl` 的消费者，正常卸载 `atbm6461_wifi_sdio`，最后执行 `/sbin/poweroff`。停止传输不向模组发送命令；该状态在本次加载中不可恢复。旧驱动没有此入口时仍尝试原卸载流程，不能视为本修复已生效。不强制卸载，不卸载可能被其他设备共用的 `cfg80211`。`/run` 关机标志仅重启后消失，不修改 AI、直播或云端参数。

消费者先等待 5 秒，再 SIGKILL 并等待 2 秒；卸载最多观察 10 秒。卸载失败或内核阻塞会明确记录，不把超时或 `poweroff` 返回成功视为物理关机成功，也不使用 `poweroff -f` 绕过文件系统清理。配套 0042 驱动修复让取消状态优先于握手成功状态，并在 IRQ/线程清理前停止收发；BF01 关机 notifier 不再发 WSM 关机/休眠命令等待已失联的模组，模块卸载则先注销 notifier。必须结合真实串口验证卸载和最终 `Power down`，编译或模拟测试不能替代整机验收。

关机步骤日志保存在 `/mnt/data/diagnostics/poweroff/latest.log`，上次记录为 `previous.log`；DATA 不可写时退回 `/run/bf01-poweroff/shutdown.log`。这不是完整启动/内核末段日志：只读重挂载后的关机串口需要电脑侧持续采集。脚本和应用必须一起安装，之后重新打包 DATA 才会进入客户固件。

```sh
make -C tools/bf01_wifi
make -C tools/bf01_wifi OUTPUT=/tmp/bf01-wifi-arm CC=/path/to/arm-linux-gnueabihf-gcc AR=/path/to/arm-linux-gnueabihf-ar
```

产物只有 `libbf01_wifi_link.a`，不会替换原应用。链接调用程序时需 `-lm`。更换编译器时使用不同的 `OUTPUT`，避免混用本机和 ARM 对象。

驱动候选单独构建：

```sh
bash tools/bf01_wifi/build-driver.sh /path/to/SDK /tmp/bf01-wifi-driver /path/to/arm-linux-gnueabihf-
```

驱动源码改动统一由 BSP 的 `patches/0042-osdrv--feat-wifi-add-bidirectional-host-integration.patch` 管理；SDK 必须先包含 2026-11-12 更新后的完整 0042。应用不再携带或自动应用驱动补丁。该补丁提供 Direct Buffer 有界确认、`0x083B` 接收队列，以及等待启动通知 1000ms、失败清理后间隔 1000ms 重试的后台 WSM 握手。WSM 未就绪时总线读写失败交回外层 worker；握手后的原生重试策略不变。

脚本每次从 SDK 复制源码到新的隔离目录，检查上述必需功能后编译；缺少功能会明确报错，提示先在 SDK 应用新版 0042，不会兜底打补丁或沿用旧 ko。脚本不改 SDK 源码，也不部署板端。BF01 正式 `make install` 仍调用该脚本，将本次新编译 ko 同时安装到项目 install 和 SDK DATA 暂存；非 BF01 不走该构建。之后仍须正式 pack_data/pack_upgrade，客户无需命令行。接收支持 1600 字节 JSON；当前 SDK 发送帧空间限制主控 JSON 为 1595 字节，超限明确返回 EMSGSIZE，不截断。双向 1600 字节上限仍须与模块确认帧容量。

## 本轮验证与下一步

2026-11-09：原应用基线重新交叉编译、安装通过；独立库本机/ARM 严格告警编译通过；独立驱动候选编译通过。外部临时测试程序通过 ASan/UBSan 的双向握手、重复包、busy、重同步、凭证门禁、get、file、stream 路由、匹配 ID、重试和长度边界用例。LeakSanitizer 受当前 ptrace 环境限制，未运行成功；未将其记为通过。

这些是使用假后端的本机协议测试，不是真实 SDIO 属性生效、文件上传或 APP 直播验收。下一步先在相机原功能回归通过后，仅验证驱动异步收发，再逐项接入真实后端；不得再次为通信联调修改 AI/录像/抓拍策略。
