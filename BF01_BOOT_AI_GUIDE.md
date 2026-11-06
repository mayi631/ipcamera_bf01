# BF01：大核优先识别，识别通过才允许落盘

当前方案（2026-11-05）为 Linux ION 40 MiB，小核 AliOS 使用独立的
`ALIOS_RESV` 堆。此前将 13.625 MiB 当成本板小核实际占用并从 Linux ION 中扣除，
这一判断不适用于当前 AliOS 配置，已纠正；56 MiB 不是最终方案。
最终内存依据、改动和实测见 `../bf01_bsp/docs/BF01_ION40_FINAL.md`。
后文 40/48/56 MiB 阶段的记录仅保留为历史证据，不代表当前内存图或精度验收。

## 无屏图片联调与正式模式

新增编译宏 `BF01_BOOT_AI_TEST_INPUT`，默认 **0**，通过同名 Make 变量选择：

| 宏值 | 模型输入 | 其余流程 |
| --- | --- | --- |
| `0` | RTOS 发布的 `VPSS(1,1)` 640×640 RGB planar 帧 | 正式上电拍照、RAM 录像、TPU 门控及 SD 落盘 |
| `1` | `boot_ai.ini` 的 `test_image` 指定的预处理图片 | 仍等待真实 RTOS READY、检查 ION 隔离、真实 TPU 推理，再确认通过或拒绝 |

测试模式不修改模型、置信度、pipeline、编解码、40 MiB ION，也不强制通过。
只替换识别输入，不把测试图片伪装成摄像头 JPEG/录像；落盘的三张照片和 MP4
仍来自真实摄像头。正式流程不需要屏幕，屏幕只影响人工查看构图，不影响直接送帧。

先在 SDK 根目录生成两个输入，主机需要 Python 3 和 Pillow：

```sh
python3 ipcamera_bf01/scripts/prepare-boot-ai-image.py \
    ambquinn-parrot-8368951_1920.png /tmp/boot_ai_test/input_1.rgbp
python3 ipcamera_bf01/scripts/prepare-boot-ai-image.py \
    gruendercoach-parakeet-10074499_1920.jpg /tmp/boot_ai_test/input_2.rgbp
```

工具等比例缩放并居中到 640×640，填充值 114，输出 RGB 平面 uint8，长度严格为
**1,228,800 字节**，同时生成同名 PNG 便于检查。测试图片保留原始宽高比，候选框按
完整 640×640 有效范围校验；正式摄像头模式仍按原有上下各 80 行补边约定校验，
不因测试改变。不要直接把 JPG/PNG
文件改名为 `.rgbp`，也不要提供 RGB packed/BGR 或已归一化浮点文件。

在原有 SDK 构建环境已就绪后：

```sh
make -C ipcamera_bf01 ipcamera -j4 BF01_BOOT_AI_TEST_INPUT=1
```

把 `ipcamera_bf01/out/ipcamera/ipcamera` 部署为 `/mnt/data/ipcamera`，两个 `.rgbp`
部署到 `/mnt/data/boot_ai_test/`。配置保持现有 `model/confidence/class_id/timeout_ms`，
只增加或切换：

```ini
[boot_ai]
model = yolov5s_cv181x_bf16.cvimodel
confidence = 0.25
class_id = -1
timeout_ms = 12000
test_image = boot_ai_test/input_1.rgbp
```

相对路径相对于应用所在目录；`/mnt/cfg/ipcamera/boot_ai.ini` 若存在，仍优先于应用
目录内配置。每个测试输入分别重启一次；RTOS 启动缓存与门控是一次性的，不能在同次
启动完成后直接重跑应用来代表新一轮上电流程。图片缺失、长度不符或读取失败，均拒绝
并停录，不回退到镜头输入。不要同时启动会消费 `/dev/ipcm_anon` 的 `alios_cli`。

检查 `/run/ipcamera-ai/status.ini` 的 `input_source=file_test`、实际 `test_image`、
得分与 `storage_allowed`，以及 `/run/ipcamera-capture/status.ini` 的任务结果；
服务日志位于 `/run/bf01-preview/service.log`。通过应新增三张 JPEG 和十秒 H.265/AAC
MP4；未检出及异常应不新增 SD 媒体。

测试后恢复正式模式，必须重新构建并部署应用，不是只改 INI：

```sh
make -C ipcamera_bf01 ipcamera -j4 BF01_BOOT_AI_TEST_INPUT=0
```

Make 会跟踪宏值变化并自动重编识别对象，无需 clean；发布版本显式指定 `0`。
正式二进制完全排除文件输入分支，忽略 `test_image`，状态显示 `input_source=vpss_1_1`。
本次改动不需要重刷 RTOS、内核或 FIP。两张鸟图仅用于功能联调，不等于完成模型精度、
负样本误检率或所有鸟类覆盖率验收；模型两个类别的具体标签仍需确认。

## 运行顺序

```text
上电，小核 RTOS 启动 VI 1920×1440@30
  ├─ VPSS-GRP0 → 原有三路预览编码
  ├─ 软件 Get/Send → VPSS-GRP1
  │    ├─ CHN0 1920×1440 NV21 → VENC3 JPEG → 三张照片缓存在 RAM
  │    └─ CHN1 640×640 RGB planar → 第三张照片对应帧缓存在 RAM
  └─ bind → VPSS-GRP2 CHN0 1920×1440 → bind → VENC4 → 音视频 RAM 环形缓存
大核 Linux 驱动/IPC 就绪 → 应用优先获取 640×640 帧 → TPU 识别
  ├─ 通过并获小核确认 → 初始化预览/存储业务 → 保存照片、续录并发布 MP4
  └─ 未检出/异常/超时 → 通知小核停录、丢弃缓存 → 不启动 SD 媒体落盘业务
```

**模型推理只在大核 Linux 执行。**小核 `bf01_boot_ai.c` 只负责供帧、结果控制和超时保护，
不链接 `cviruntime`，不调用 TPU 推理接口。“第一件事”指大核应用的首项业务，
不是在内核、TPU 驱动或 IPC 尚未就绪时运行模型。

沿用早期录像的 H.265/30fps/16kHz 单声道音频；按图拆出独立 GRP2 录像通路，
不顺带将既有 H.265 改为 H.264。通过前只在 RAM 中编码缓存，不写 SD。
原预览分支不重新设计。识别未通过时本次启动的应用退出，不持续重试推理。

## 图像交接

- 新增 `VPSS(1,1)`：640×640、RGB planar、8bit、depth=1、独立 VB pool。
- 1920×1440 等比例缩放到 640×480，居中补边，上下各 80 行，RGB 补边值 114。
  不拉伸原图，不从 JPEG 或 SD 文件取图。
- 小核在送帧前及 prime/前两张照片后非阻塞取走已有识别输出，避免队列积压，
  也不要求预热 prime 必须立即产出 RGB 帧；最终帧严格校验 PTS 与本次送帧一致。
  第三张 JPEG 完整接收后，将同次送帧产生的 RGB 输出按 stride 逐行复制到连续 RTOS
  保留堆，做缓存同步，释放 VPSS 帧并关闭识别通道。
- 大核通过 IPC 端口 4 获取物理地址/尺寸/PTS，映射后直接按 `/255` 写入模型的
  FP32 NCHW 输入，不再 JPEG 编码/解码，不二次缩放。
- 严格检查输入 `[1,3,640,640]`、单 FP32 输出 `[1,25200,7,1]` 或等价三维输出。
  RGB、归一化和已解码 YOLOv5 输出仍沿用此前图片测试假设，需模型作者确认。
  VPSS 硬件缩放与 CPU 示例不保证逐像素一致，颜色/画质仍需实图回归。

新增 RGB VB pool 3 块、GRP2 1920×1440 NV21 VB pool 3 块及一帧 RGB 副本，
不计对齐约增加 16.55 MiB 的 RTOS 多媒体/保留堆需求。没有扩大 RTOS 物理区域，
供帧集成阶段没有再次修改 `ION_SIZE`；最终 Linux ION 为 40 MiB，详见上述最终说明。
该内存方案仍需原厂确认及长稳验证。

## 判定规则

工厂配置随应用安装到同目录 `boot_ai.ini`。可用 `/mnt/cfg/ipcamera/boot_ai.ini` 覆盖，
该文件存在但不可读或内容非法时不降级绕过。

```ini
[boot_ai]
model = yolov5s_cv181x_bf16.cvimodel
confidence = 0.25
class_id = -1
timeout_ms = 12000
```

- `model`：绝对路径，或相对于正在运行的 `ipcamera` 可执行文件目录。
- `confidence`：`objectness × 最大类别概率`，范围 `(0,1]`。
- `class_id=-1`：默认接受模型任一类别；`0`/`1`：仅接受该最佳类别 ID。
  类别名尚未提供，不能将 ID 擅自解释为某种鸟，正式业务规则需确认后调整。
- 存在一个坐标有效、中心落在有效图像区域、满足类别及得分要求的候选框即通过。
  这里只判断目标存在性，不做数量统计，因此不需要 NMS 再决定存在性。
- 工作进程总时限 `timeout_ms` 为 1000–20000 ms，涵盖模型加载、预处理和推理。
  大核门控另有约 28 秒总期限；小核从媒体启动后最多等待 45 秒的判定。

大核用独立 exec 工作进程运行模型，处理运行库 assertion/崩溃、非零退出、无效结果和超时。
超时先杀掉并回收工作进程，再通知小核回收图像，避免工作进程仍在读取已释放的内存。
小核超时停录但不擅自释放可能仍被读取的 RGB 副本；副本最多一帧，等待拒绝命令释放，
若 Linux 从未运行则保留到重启。只有通过且小核确认后才设置 `storage_allowed=1`，
并开始原有照片接收、SD 等待、保存和 MP4 录制流程。

## VPSS 设备映射修复（2026-11-05）

ADB 读取小核启动日志确认首次配置存在错误：

```text
CVI_VPSS_SetChnAttr(): Grp(1) Chn(1) invalid for VPSS-Dual(0).
[boot-capture] media init failed: ret=0xffffffff
```

图中的 GRP 是逻辑组，不等于硬件 DEV。当前 CV181X 库在 DUAL 模式下，DEV0 每组只有
CHN0，DEV1 每组有 CHN0–CHN2。原先将双输出 GRP1 放在 DEV0，导致初始化 CHN1 时立即失败，
并非本次 TPU 推理失败。已同步修正小核和 Linux 参数文件中的 GRP1 `vpss_dev` 为 1：

| 逻辑组 | 硬件设备 | 输入/输出 |
| --- | --- | --- |
| GRP0 | DEV1 | VI bind；裁剪后 CHN0/1/2 三路预览，15 fps |
| GRP1 | DEV1 | VI Get/Send；CHN0 1920×1440 JPEG，CHN1 640×640 RGB 识别 |
| GRP2 | DEV0 | VI bind；CHN0 1920×1440 录像，30 fps |

GRP0/GRP1 共享 DEV1 的离线处理能力，不改变图中的逻辑组编号、取帧位置及绑定。
保留当前编码格式：三路预览 H.264、拍照 JPEG、早期录像 H.265；本次不改编码实现。
2026-11-05 已更新 RTOS 并检查三张 JPEG、640×640 供帧及早期录像出帧，结果见末节。
Linux 预览与真实 TPU 推理并发的稳定性仍待 ION 前提满足后验收，不能用早期录像测试代替。

本次 ADB 还读到当前固件 ION 总量为 41,943,040 字节（40 MiB），已不同于此前 48 MiB
测试版本。本次修复不修改该内存配置，也不绕过 ION 安全检查；初始化错误先于推理发生。

## ION 安全前提

**旧 40/48 MiB 固件的隔离异常不能绕过；当前 56 MiB 固件已修复实际分配池。**加载模型前检查：

1. 图像地址位于 RTOS 声明的物理区域，尺寸、范围及协议正确。
2. Linux ION `total_mem` 不包含 RTOS ION 预留，`summary` 可分配范围不与预留区重叠。
3. Linux ION 空闲至少达到此前实测峰值 36,745,216 字节。这只是必要下限，
   不是保证并行业务绝对够用的上限。
4. TPU/ION 设备、调试统计、模型、配置和运行库可用。

失败一律停录、不落盘，没有忽略风险开关。修复前驱动把完整 40 MiB 放入分配池，
因此输出 `unsafe ION pool` 并返回 `EACCES`。正确排除 13.625 MiB 的 RTOS 预留后，
40 MiB 总量仅剩 26.375 MiB；此前 48 MiB 方案也仅剩 34.375 MiB，
均低于此前峰值，仍会返回 `ENOMEM`。
当前 56 MiB 总池正确隔离后为 42.375 MiB，实机门控及落盘结果见末节。
编译成功本身仍不是安全性证明，须使用一致内存图与匹配镜像，并让原厂确认容量方案。

## 改动清单

以下 BSP 前缀为 `bf01_bsp/cvi_alios/solutions/fastboot/customization/cv1815ja_bf01_spinand/`，
应用源码前缀为 `ipcamera_bf01/solutions/ipcamera/`。

| 文件 | 内容 |
| --- | --- |
| BSP `pipeline/custom_vpssparam.c` | GRP1 CHN1 640×640 RGB；独立 GRP2 录像 |
| BSP `pipeline/custom_sysparam.c` | 两个新增 VB pool，数量 5 → 7 |
| BSP `src/bf01_boot_ai.[ch]` | 供帧协议、缓存同步、结果控制和 45 秒保护，无 TPU 推理 |
| BSP `src/bf01_boot_capture.[ch]` | 取走识别输出；拒绝后丢弃拍照缓存 |
| BSP `src/bf01_boot_video.[ch]` | 录像改为 GRP2 bind；拒绝后停录并释放缓存 |
| 应用 `src/app_ipcam_boot_ai.c` | Linux IPC、安全检查、隔离工作进程、直接输入模型 |
| 应用 `src/app_ipcam_main.c` | 识别放在网络、预览和 SD 业务初始化之前 |
| 应用 `src/app_ipcam_capture.c` | 再检查门控；成功结束为 `completed`、`ai_state=passed` |
| `resource/parameter/fbm/cv1815ja_bf01_gc4683.ini` | 同步新通道及 GRP2，保留已有本地参数修改 |
| `build/dep.mk`、`build/install.mk`、`scripts/install-preview.sh` | 头文件路径；安装模型、配置及匹配 ARM32 运行库 |

## 构建部署

RTOS 和 Linux 应用必须同时更新；只替换 `ipcamera` 不会新增小核 VPSS 通道。
沿用当前工作区配置，不 clean、不重新 defconfig：

```sh
export SCENES=1 CHIP=cv1815ja BOARD=bf01_spinand
source build/envsetup_soc.sh
cvi_setup_env
make -C build alios -j4 \
    FLASH_PARTITION_XML="$PWD/build/boards/cv181x/cv1815ja_bf01_spinand/partition/partition_spinand.xml"
make -C ipcamera_bf01 ipcamera -j4
make -C ipcamera_bf01 ipcamera install
```

安装会从应用目录 `ipcamera_bf01/ai_model/` 复制模型到应用安装根目录，并复制同板型 `tpu_32bit/cvitek_tpu_sdk/lib/` 的两份运行库，
缺库时先按图片示例说明运行 `build_tpu_sdk`。原厂确认内存方案后，按既有流程完整构建
RTOS/固件，再把应用安装目录打入 data 分区；仍可沿用 `make pack_data` 和升级包流程。
源码修改不会自动更新板端；2026-11-05 录像回归的实际更新范围和回退备份见末节。
`install/soc_cv1815ja_bf01_spinand/rawimages/yoc.bin` 是带小写 `cimg` 头的 MTD1 载荷；
其上一级 `yoc.bin` 是大写 `CIMG` 下载封装，不能将两者当成相同格式直接写 MTD1。

## 验收入口

- `/run/ipcamera-ai/status.ini`：等待/识别/通过/拒绝/失败，含错误码、类别、得分、
  Forward 耗时及 `storage_allowed`。
- 通过后的原拍录状态：`/run/ipcamera-capture/status.ini`；最终 `state=completed`、
  `ai_state=passed`。`upload_decision` 仍为 pending，本次不实现上传策略。
- 未通过不启动 SD 挂载监控及媒体保存；日志见 `[boot-ai] gate closed`。
- 必须测试目标/无目标、类别过滤、模型或 TPU 缺失、内存不足、进程崩溃/超时、
  小核等待超时、无卡/插卡、音视频完整性、摄像头并行及长期稳定性。

不得在已知存在隔离风险的旧固件上追加推理；此前两张静态图成功不代表摄像头门控通过。
当前实机已使用末节的隔离修复固件，不能仅用新应用替代该内核与内存图更新。

## 本次验证记录（2026-11-05）

- ARM32/glibc 应用完整编译、链接、安装通过；安装目录已包含程序、配置、原模型和两份匹配运行库。
- 小核新增供帧模块、拍照/录像模块和两份 pipeline 配置的 RISC-V 目标文件均编译通过。
  后续修复小核链接后，完整链接及 `yoc.bin` 生成也通过；该阶段尚未部署到板端。
- 主机逻辑测试通过：帧边界、ION 隔离/容量、得分和类别过滤、补边区域/NaN 拒绝、
  管道 SIGPIPE、工作进程成功/拒绝/崩溃/超时/协议异常及子进程回收。
- 小核协议模拟测试通过：RGB 平面/stride 复制、队列取走、帧释放和通道关闭、
  地址/状态/期限/PTS 校验、重复通过、通过后的撤销、超时保留图像至读者结束、提前拒绝。
- 大核完整门控测试使用真实 exec 工作进程及模拟 IPC/runtime，通过了 RGB `/255` NCHW 检查、
  通过/无目标/类别过滤、非法配置、驱动或模型加载失败、ION 风险/容量不足、tensor 错误、
  推理失败/崩溃/超时，以及通过应答丢失后主动撤销的路径；只有获确认的成功才开放落盘。
- 主机模拟测试不代表模型精度、真实 TPU 耗时或摄像头并行稳定性。测试工件和日志位于
  `/tmp/bf01_ai_validation/`，未引入新的项目测试框架。

## 小核链接修复（2026-11-05）

首次验证仅编译目标文件，未发现共用头文件声明的 `CVI_SYS_MmapCache`、`CVI_SYS_Munmap`
在当前 RTOS 库中没有实现。`APP_BootAiCapture` 已改为沿用小核 SDK 示例的物理地址直接访问，
不再依赖这两个 Linux 映射接口；仍在读取 VPSS 帧前执行 `CVI_SYS_IonInvalidateCache`，
并在发布 RGB 副本前执行 `CVI_SYS_IonFlushCache`。

重新执行完整 SCons 构建，已完成 `libfastboot.a` 更新、`yoc.elf` 链接及 `yoc.bin` 生成。
小核原有主机测试移除了映射接口模拟，并新增缓存同步及其失败清理检查，ASan/UBSan 检查通过。
该链接修复阶段未烧录，也未解除上述 ION 安全限制；后续录像实机验证记录如下，
不代表已完成整机识别验收。

## 录像回归修复及实机验证（2026-11-05）

### Git 基线与根因

BSP 基线为 `e20c560bde89c7dfb35b88548ce7f80e9c7bc688`，提交时间为
2026-11-04 16:58:33 +08:00。该版使用 VI Get → GRP1 Send 和 VPSS Get → VENC Send
双线程软件供帧，录像格式为 H.265 1920×1440@30。AI 集成改为 GRP2 绑定供帧后，
仍沿用了先 `CVI_VENC_StartRecvFrame` 再 `CVI_SYS_Bind` 的顺序；绑定链路应先绑定再启动接收，
SDK 自带 `components/cvi_test/test/venc/test_venc_func.c` 的实现也是此顺序。

另外，绑定路径的 `CVI_VENC_GetStream` 在 FIFO 暂无数据时可返回普通 `CVI_FAILURE(-1)`，
旧错误处理把第一次短等待当作致命错误。修复前实机日志为 `ret=0xffffffff`、
`frames=0`、`error=5`，早于 Linux AI 拒绝发生，是录像回归，不是正常的 AI 停录。

本次修复：

| 文件 | 改动 |
| --- | --- |
| BSP `src/bf01_boot_video.c` | 创建 VENC4 → 启动音频 → bind GRP2/0 → StartRecvFrame；取流对 FAILURE/BUSY/BUF_EMPTY 在总计 1000 ms 内重试；保留取消和硬错误退出 |
| 应用 `src/app_ipcam_boot_video.c` | 每次交换先清空应答；STOP 将合法 READY/FAILED/RELEASED 视为已停止，历史录像错误不再误报成 IPC 未应答 |
| BSP `pipeline/custom_vpssparam.c` 与 Linux BF01 INI | 保留前一轮已修复的 GRP1 DEV1 双输出、GRP2 DEV0 单输出配置 |

没有改变当前 H.264 预览、JPEG 拍照、H.265 录像、AAC 封装格式，也没有修改当前 40 MiB 内存图。

### 实际更新与回退

设备 `0123456789` 的 MTD1 和原 `/mnt/data/ipcamera` 已备份到
`/mnt/data/bf01_rollback_video_20261105_1438/`，包含原配置和启动脚本。
电脑端固件、程序备份另存于 `/tmp/bf01_runtime_20261105/rollback/`。
NAND 备份读取结果为 0 个坏块、0 个 ECC 纠正/失败；电脑与板端备份 SHA-256 一致。

只更新 MTD1 和 `/mnt/data/ipcamera`，写后读回核验通过，未更新 FIP、BOOT、参数或分区表。
已部署版本的 SHA-256：

- MTD1 有效载荷 1,389,548 字节：`75d5bd7e5b409fd14aa0a6bac78f9baceb26a6892360400112a91a05a4efdea6`。
- `ipcamera`：`e62e10f6c544634bcb202772b3a470ece7cdbdd44c3673b34704f50d613c75b0`。

### 验证结果与边界

- 主机回归测试通过：绑定启动顺序、短暂空流/忙/超时重试、总期限、硬错误、启动失败清理，
  以及 STOP 终态确认、历史错误、应答丢失、旧应答清除；ASan/UBSan 检查通过。
- 第一次重启：三张照片成功，640×640 RGB 帧就绪；录像从启动后约 2.032 秒开始，
  连续 259 帧，PTS 计算实际帧率约 29.99998 fps；音频 435 帧、139,200 个样本，
  16 kHz 单声道、8.7 秒，非全零。录像最终 `error=0`，无取流失败或停止未确认。
- 录像停止后只读快照其仍保留的内存内容，两次物理读取与电脑副本 SHA-256 相同。
  259 帧 H.265 完整解码通过。此快照是一次性诊断，不依赖已释放缓冲区作为生产接口。
- 在板端 `/run` 内存盘使用未修改的 `app_ipcam_mp4.c`，从该真实快照提取 8 秒录像，
  得到 240 帧 H.265 1920×1440@30 和 16 kHz 单声道 AAC；两条轨道均为 8.000 秒，
  容器显示 8.064 秒包含 AAC 编码延迟。拉回电脑后音视频完整解码通过。
  文件为 `/tmp/bf01_runtime_20261105/decoded/recording_verified.mp4`；
  这是原封装代码的独立实机测试，不是 AI 通过后自动保存到 SD 的业务验收。
- 第二次重启：三张照片、640×640 供帧再次成功，录像 123 帧后正常停录，`error=0`；
  本次 Linux 更早到达 AI 检查，因此录制时间更短。共享日志尾部会残留前一次启动内容，
  不应把上一轮 259 帧记录当成本轮结果。
- 两次 AI 均为 `state=failed,error=13,forward_us=0,storage_allowed=0`，
  因 ION 隔离检查失败而正常停录，没有运行 TPU，不能解释为模型判定无目标。

该录像回归阶段板端 ION carveout 总量 41,943,040 字节，当前分配 0、启动峰值 28,672 字节，
空闲区仍为完整 `[0x85800000,0x88000000)`；这不是模型推理峰值。
录像采集/编码及原 MP4 封装已实测恢复，但“真实 TPU 识别通过 → 继续录制 → SD 落盘”
仍受 ION 隔离和容量阻塞。没有为了让录像落盘而绕过识别，也没有宣称整条业务已验收正常。

## ION 隔离修复与完整实机流程（2026-11-05）

### 隔离失败的真正原因

`linux_5.10/drivers/staging/android/ion/ion_carveout_heap.c` 已读到设备树的
`fast_image/ion-size=0xda0000`，即 13.625 MiB。旧实现仅调整清零使用的 `page/size`，
`gen_pool_add()` 和 `heap.total_size` 仍使用原始整个 ION 区域，因此 Linux 分配器
仍可能分配 RTOS 保留区。应用报隔离失败是正确拦截，不能删除检查来放行。

现在建池、基址及容量统计统一使用扣除预留后的范围，同时检查非法预留及建池失败。
修复在内核源码和 BSP 补丁 `0032-linux_5.10--fix-ion-exclude-rtos-reservation.patch`
中各有一份，不更改 TPU 模型、门控规则、内核导出接口或模块结构体布局。

仅修复隔离仍不够：40 MiB 总池扣除预留只剩 26.375 MiB，48 MiB 也只有
34.375 MiB，均小于实测约 35.043 MiB 的模型分配峰值。因此本次一致更新
FIP、RTOS 和 Linux 设备树，将 `memmap.py` 的总 ION 改为 56 MiB。

| 项目 | 当前值 |
| --- | --- |
| DDR / RTOS 区域 | 256 / 128 MiB，保持不变 |
| 总 ION | 56 MiB，`[0x84800000,0x88000000)` |
| RTOS 的 ION 预留 | 13.625 MiB，`[0x84800000,0x855a0000)` |
| Linux 实际分配池 | 42.375 MiB，`[0x855a0000,0x88000000)` |
| 本轮 Linux ION 实测峰值 | 36,745,216 字节，35.043 MiB |
| 峰值时容量余量 | 7,688,192 字节，7.332 MiB |
| 工作进程退出后 ION | 0 字节 |
| Linux `MemTotal` | 63,744 KiB；不是剩余可分配 ION |

上述峰值来自内核 ION 累计统计，覆盖该次真实推理；不是 RTOS 全部堆峰值，
也不代表更复杂场景或长期运行的最坏值。

### 三次启动的实测结果

1. 原配置 `confidence=0.25`：真实摄像头 VPSS(1,1) 640×640 RGB 供帧，
   TPU `forward_us=407419`，`state=passed,error=0,score=0.383698`。
   自动保存 `DSCF00019.jpg`～`DSCF00021.jpg` 及 `DSCF00007.mp4`，
   最终 `state=completed,recorded_ms=10000,video_bytes=775658`。
2. 仅用于拒绝测试，临时将板端门限改为 0.99 后重启：真实 TPU
   `forward_us=408731`，`state=rejected,error=61,storage_allowed=0`。
   应用未自动挂载 SD、未启动拍录保存业务；只读诊断挂载后核对 DCIM 文件列表，
   与测试前完全一致。小核本轮录像 298 帧后正常停止，`error=0`。
   此为受控拒绝测试，不是自然无目标精度测试；共享日志尾部的 352 帧属于前轮残留。
3. 从备份恢复原始 0.25 配置，SHA-256 与测试前一致，再次重启：
   `state=passed,error=0,class_id=0,score=0.366745,forward_us=406406`。
   三张照片约在启动后 792/1324/1860 ms 获取；小核录像约 2060 ms 开始，
   共 396 帧后正常停止。自动保存 `DSCF00022.jpg`～`DSCF00024.jpg`
   和 `DSCF00008.mp4`，最终 `state=completed,error=0,recorded_ms=10000`。

两轮自动保存的 MP4 拉回电脑后均通过哈希核对及音视频完整解码：H.265
1920×1440、30 fps、300 帧、10.000 秒；AAC 16 kHz 单声道、10.000 秒，
容器 10.064 秒包含编码延迟。没有更换编码格式。六张 JPEG 均可解码和查看，
FFmpeg 对其原有 APP 元数据有警告，不影响此次像素解码；未在本次修改 JPEG 元数据。

实际文件和状态保存在 SDK 根目录 `bf01-e2e-results/20261105-ion56/`；
`final-pass/` 为恢复原配置后的最终结果，`reject/` 为拒绝分支的实机证据。
该历史测试结束时恢复原门限 0.25，`storage_allowed=1`，拍录为 `completed`。

### 更新范围与验收边界

本次实际更新 MTD0/FIP、MTD1/RTOS、MTD2/BOOT，均写后读回哈希核验；
没有更新 ROOTFS、分区表、算法文件或应用二进制。完整原分区、程序、配置及模块备份在
`/mnt/data/bf01_rollback_e2e_20261105_1458/`，电脑副本和构建日志在
`/tmp/bf01_e2e_20261105/`。NAND FIP 必须保留正确的块头、双副本及芯片参数，
不能将原始 `fip_spl.bin` 或大写 `CIMG` 封装直接写入 MTD。

本次保留原 ROOTFS 模块，内核兼容构建的 `UTS_RELEASE` 为
`5.10.4-tag--g7b50980d408d`，实际内核构建时间为 2026-11-05 15:25:19 CST。
普通完整固件构建应重新构建并同时更新内核及所有模块，不能混用 `-dirty` 和非
`-dirty` 的模块版本。具体说明见 BSP 的 56 MiB 文档。

办公室实拍画面未见明显目标，但模型在原门限下仍返回 class 0，得分约 0.37～0.38。
因此目前证明的是“实际推理结果驱动通过/拒绝和落盘”的流程，不是识别精度正确；
类别含义、误检率、门限和预处理约定还需模型方确认及正负样本测试。
没有为通过验收降低门限，也没有绕过隔离或强行设置识别成功。
原厂内存方案合规性、长稳、热启动/断电、多卡及故障注入等仍未完成验收。
