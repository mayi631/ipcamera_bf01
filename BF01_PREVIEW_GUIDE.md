# BF01 拍照、MP4 录像与远程网络预览

统一相机入口为编译生成的 `ipcamera`。网络预览服务运行在该进程内，默认由它启动 AP 网络辅助进程；辅助进程不再启动相机。不要另起第二个 `ipcamera`，也不需要先启动 `bf01-preview`。单实例锁拒绝重复启动，退出相机时停止所属网络辅助进程并恢复 STA。

网络控制协议只实现 live preview：沿用 BF01 已有 VENC/RTSP 码流，通过 JL25 风格控制接口返回预览地址；不增加远程拍照、录像、回放或配置类接口。应用新增启动后自动拍照、录制 MP4 并保存待识别任务的流程；原有 SIGUSR1 手动拍照入口保留。默认开机启动相机并完成本地采集，但不接管 Wi-Fi；不改 SDK 的 `S99user`、无线驱动或 GPIO。

## 开机自动采集流程

使用本次安装、打包的 `auto.sh` 后，上电由现有 SDK 启动脚本拉起唯一的 `ipcamera`：

1. 初始化既有媒体 pipeline，三路 H.264 视频编码继续运行。
2. 等待可写 SD 卡，拍摄一张 2560×1440 JPEG。
3. 照片保存成功后，从所选视频通道首个关键帧开始录制，默认 10 秒，保存为 MP4。
4. MP4 正常收尾后，将任务标记为 `awaiting_ai`，继续保持相机和预览服务运行。

当前没有本地 AI 和云端上传实现，只保存关联照片、录像的任务信息；不会自动判断识别结果或上传。默认录制通道为 0（1920×1080），通道 1 为 1280×720，通道 2 为 640×360；选择已有编码通道，不重建 pipeline。录制为 H.264 视频封装 MP4，无音频、不转码，与预览使用独立码流读取器。

此流程在每次 **应用进程启动** 时执行一次，不循环拍录；手工停止后重启应用也会生成新任务，不限于整机上电。当前不支持在正在录制的一个 MP4 内切换分辨率；设置在下一次应用启动时生效。未来 APP 可复用配置接口，远程 APP 设置协议尚未接入。

### SD 卡与输出

请插入可用的 FAT32/vfat SD 卡，并保持至少 16 MiB 可用空间。独立模块 `app_ipcam_storage.c` 在相机运行期间持续监测，不再依赖开机采集线程：优先接收内核 uevent，同时每 500 毫秒轮询 sysfs 的 `SD` 类型和挂载状态。将第一分区（无第一分区时尝试整卡）以 vfat 挂载到 `/mnt/sd`；不格式化、不执行自动 fsck、不挂载 eMMC/SDIO、不接管已挂载到其他路径的卡，也不覆盖其他设备的挂载或强制将只读卡改为可写。非 vfat 文件系统需自行正确挂载到 `/mnt/sd`。挂载失败、未插卡、只读或空间不足时，初始任务保持 `waiting_for_storage`，不会向未挂载的目录写照片或视频。

拔卡后模块使旧存储代次失效；自动采集若仍在使用卡，会中止并关闭文件，然后释放存储引用。模块等待旧任务释放引用，再正常卸载失效挂载，重新插卡后自动挂载；不执行强制或 lazy 卸载。若其他进程的打开文件或当前目录占用挂载点，状态为 `unmount_failed`，释放占用后自动重试。已成功完成的拍录不会因为重新插卡再次执行；拔卡中断的任务不会自动补录，仍保持每次相机启动只执行一次的规则。手动 SIGUSR1 是原有异步调试接口，不属于自动任务的存储引用管理，使用前必须确认存储 `ready=1`，且不要在拍照期间拔卡。

查看独立存储状态（不等于本次拍录任务状态）：

```sh
cat /run/ipcamera-storage/status.ini
grep '\[storage\]' /run/bf01-preview/service.log | tail -20
```

`state=ready`、`ready=1` 表示当前卡已挂载、可写且剩余空间达到阈值。其他状态包括 `no_card`（无卡）、`removing`（等旧任务释放）、`unmount_failed`（卸载失败/占用）、`mount_failed`（挂载失败）、`read_only`（只读）、`full`（空间不足）、`io_error`（I/O 错误）、`mounted_elsewhere`（卡被挂在其他路径）、`mount_conflict`（挂载点属于其他设备）和 `stopped`（监控已停止）。状态还包含设备路径、存储代次 `generation`、活动引用数及总容量、剩余容量；容量单位为字节，非 ready 状态不能仅凭旧容量数值判断卡可用。每次进入可用状态前检查临时文件创建、写入及同步；不会每 500 毫秒向 SD 卡写测试文件，运行状态只写 `/run`。

热拔插恢复不等于安全拔卡，不能保证在写入过程中拔卡后文件完整。正常取卡仍应先停止整个相机，等待退出后执行 `sync` 和 `umount /mnt/sd`；不要在监控仍运行时单独 `umount`，否则模块会重新挂载。

源照片和 MP4 按 DC100/DC309 的规则保存到 `DCIM`，使用 `DSCF` 加五位递增编号；不生成缩略图：

```text
/mnt/sd/DCIM/PHOTO/DSCF00001.jpg
/mnt/sd/DCIM/MOVIE/DSCF00001.mp4
/mnt/sd/.capture/task_XXXXXX/task.ini
```

照片和录像分别在各自目录中从已有最大编号继续递增，遗留 `.part` 也会占用编号，避免重用失败任务的路径。不依赖系统日期时间，超过 99999 后继续使用六位编号；照片和录像通过 `task.ini` 中的实际路径关联，不要求编号相同。新任务目录只保存状态清单，不存放媒体副本；项目只保存源照片和源 MP4，不提取缩略图。

任务目录仍保存在 SD 卡，改为 `.capture`，并参考 DC100 的 `custom.c` 设置 FAT `ATTR_HIDDEN | ATTR_SYS`（隐藏＋系统）属性，再同步到卡中。点目录用于 Unix 类系统的默认隐藏，FAT 属性用于 Windows；这不是访问控制，`ls -a` 或关闭系统文件隐藏等操作仍可查看，不能保证所有系统在所有设置下都不可见。非 FAT 文件系统或属性设置失败时输出警告，保留点目录并继续拍录，不为隐藏功能中断采集。

首次运行会将已有 `/mnt/sd/capture` 整体迁入 `/mnt/sd/.capture/legacy_capture`，重名则使用 `legacy_capture_001` 等空闲名称；不覆盖已有目录、不删除文件，也不移动 `DCIM` 中的媒体。旧目录内若有历史照片或录像，会随目录迁移；历史清单文本保持原样，里面的旧绝对路径可能不再有效，当前没有历史任务自动恢复逻辑。需要找旧文件时按迁移后的实际路径查找，不要继续使用旧清单中的路径。

查看本次任务与启动日志：

```sh
cat /run/ipcamera-capture/status.ini
tail -80 /run/bf01-preview/service.log
ls -lh /mnt/sd/.capture/task_*/
ls -lh /mnt/sd/DCIM/PHOTO/ /mnt/sd/DCIM/MOVIE/
```

正常结束时 `state=awaiting_ai`、`error=0`、`ai_state=pending`、`upload_decision=pending`。`task.ini` 保存在 SD 卡，运行状态文件在 `/run`，重启后更新。拍照失败、码流异常或磁盘写入失败标记 `failed`；正常停止应用时未完成任务标记 `interrupted`。录制中的临时文件是目标 MP4 后缀 `.part`（例如 `DSCF00001.mp4.part`），成功写入 MP4 尾部后才更名为最终文件；断电留下的 `.part` 不保证可播放，也不会自动恢复。不要在录制中拔卡或断电；未加入自动覆盖或循环删旧文件策略，请及时导出文件。

状态中的 `directory` 只指向本次应用启动的任务；之前的任务目录不会继续录制或自动补录。旧目录中的 `task.ini` 或 `DCIM` 中的 `.part` 文件不是“还在等录像结束”，需结合该次启动日志排查异常拔卡、掉电或写盘失败，不能通过继续等待修复。

运行状态在录像期间每约 1 秒视频时间刷新一次 `recorded_ms` 和 `video_bytes`；进度只写 `/run`，不每秒同步 SD 卡。`task_started_uptime_ms`、`storage_ready_uptime_ms`、`photo_ready_uptime_ms`、`recording_started_uptime_ms`、`completed_uptime_ms` 为开机后的单调时钟毫秒数；`photo_elapsed_ms` 为本次拍照、保存耗时，`elapsed_ms` 为任务耗时。日志立即输出状态和进度，不等待标准输出缓冲区填满。10 秒录像必须采集完整的 10 秒视频时间；这里优化的是启动、等待和写盘开销，不通过缩短片段、降低分辨率、帧率、码率或 JPEG 质量来加速。

应用在临时文件写入后同步数据，重命名后再次同步目标文件及父目录，并同步新建目录的上级目录，再发布完成状态。这一点对 FAT/vfat 尤其重要：只在重命名前同步文件、重命名后仅同步父目录，不能完整覆盖重命名造成的文件 inode 元数据更新。此修复不恢复已经变为 0 字节的旧文件，也不替代安全卸载；SD 卡自身不可靠、拔卡或掉电仍可能造成损坏。

JPEG 保存前检查 SOI/EOI 起止标记，不完整时最多重新取新帧拍摄一次，仍失败则停止本次任务并标记 `failed`，不将坏照片提交为待识别任务；这不是完整 JPEG 解码检查。实测发现同一轮上电内反复停止、重启应用可能拿到缺少 EOI 的 JPEG：上电首次任务原图可解码，部分热重启后的照片不可完整解码。当前已拦截该异常，底层原因尚未确认，不要把“文件非零”当成画质通过；完整冷启动与热重启都需分别验收，本次没有为此修改 SDK。

取卡前先确认任务完成，再执行：

```sh
/mnt/data/bf01-preview stop
cd /
sync
umount /mnt/sd && echo 'SD card unmounted; safe to remove'
```

若卸载失败，不要直接拔卡或强制卸载。原日志已有 FAT 异常时，先备份现有文件，再离线检查文件系统，不要直接格式化。

### 设置录像时长与通道

板端执行以下命令只保存配置，不启动第二个相机、不打断当前任务：

```sh
/mnt/data/ipcamera --capture-config 5 0
```

参数依次为秒数（1–300）和通道（0–2）；例如 `--capture-config 10 1` 表示下次录制 10 秒 720P。配置保存在已挂载且可写的 `/mnt/cfg/ipcamera/capture.ini`，优先于程序同目录的工厂配置。重启板子或正常停止后重新启动应用，才应用新设置；不需要重新烧录。等待当前任务结束后再重启应用，避免中断正在录制的文件。

工厂默认值位于应用源码 `resource/preview/capture.ini`，通过正常 `make ipcamera install` 随包安装：

```ini
[capture]
enabled=1
duration_seconds=10
channel=0
```

要临时关闭自动拍录，可在持久化配置中设置 `enabled=0` 并重启应用，预览与手动拍照仍可使用。再次运行 `--capture-config` 会保存 `enabled=1`。应用配置 API 为 `app_ipcam_Capture_Configure()`，当前与命令行入口一样，只对下一次启动生效。

## 单应用启动

烧录本次重新编译、安装和打包的固件后，应用已由 `auto.sh` 自动启动，不要重复执行二进制。如果需要手动切到热点模式，先等待自动采集完成，再执行：

```sh
/mnt/data/bf01-preview stop
BF01_WIFI_MODE=ap /mnt/data/ipcamera &
```

未指定 `BF01_WIFI_MODE` 时，手动执行二进制仍默认 AP；开机脚本默认 `off`。无已保存 AP 配置时，使用应用随包安装的 `ap.conf`：SSID `BF01-PREVIEW`，密码 `12345678`，信道 6。这是联调默认密码，量产前务必修改 `resource/preview/ap.conf` 并重新打包，或用原有 `bf01-preview configure-ap` 保存私有配置；`/mnt/cfg/bf01-preview/ap.conf` 优先于随包默认值。程序需要同目录的 `bf01-preview`（内部网络辅助脚本）、`bf01_atbm_ap`、`capture.ini` 和参数文件，不能只拷贝一个 ELF 文件。

手机或电脑连接该热点，在支持 RTSP/TCP 的播放器打开 `rtsp://192.168.43.1:8554/live1`；实际地址以 `ifconfig wlan0` 为准。只有收到有效 H.264 参数和关键帧后才生成 `/run/bf01-preview/media.ready` 和 `service.ready`，`pidof ipcamera` 应只有一个 PID。

其它启动方式仍只运行同一个应用，不要同时执行：

```sh
BF01_WIFI_MODE=sta /mnt/data/ipcamera &
BF01_WIFI_MODE=off /mnt/data/ipcamera &
```

`sta` 使用 SDK 已保存的网络连接；首次先用 `wifi connect` 配网。`off` 不接管 Wi-Fi，但仍运行相机并尝试开启预览服务，适用于已有网络或仅验证媒体功能。联网辅助进程异步启动，不阻塞本地拍录；辅助进程失败也不再让相机退出。停止统一应用使用 `kill -TERM "$(cat /run/bf01-preview/ipcamera.pid)"`，等待进程退出后再重启。

兼容保留 `bf01-preview start ap|sta`、`stop`、`status` 等旧命令：`start` 现在直接启动同一个 `ipcamera`，不再运行第二个相机主管进程。已启动且网络模式相同的情况下会提示已运行，不重复启动。若旧版本程序还在运行，应先正常停止旧程序，再部署新程序，不能混用新旧脚本和二进制。

## 编译

在 SDK 根目录打开同一个 Bash 终端，先加载板级环境，再编译 IPCamera：

```bash
cd /home/mayi/SDK_CV181X_BF01
export SCENES=1
source build/envsetup_soc.sh
defconfig cv1815ja_bf01_spinand
cd ipcamera_bf01
make cv1815ja_bf01_defconfig
make ipcamera
make ipcamera install
```

构建产物位于 `ipcamera_bf01/install/`，包含 `ipcamera`、BF01 参数、ATBM AP 控制工具和运行脚本。环境初始化、配置、编译和安装要在同一个终端完成。

## 合入固件

和 DC100/DC309 一样，先安装 IPCamera，再将安装目录打进 SDK 的 data 分区，最后打 rootfs 和升级包。以下命令在同一个 Bash 终端执行：

```bash
cd /home/mayi/SDK_CV181X_BF01
source build/envsetup_soc.sh
defconfig cv1815ja_bf01_spinand
cd ipcamera_bf01
make install
make pack_data
cd ..
pack_rootfs
pack_upgrade
```

`make pack_data` 会把 `ipcamera_bf01/install/` 安装到 `install/soc_cv1815ja_bf01_spinand/data/` 并调用 SDK 的 `pack_data`；板端由现有 `S99user` 执行 `/mnt/data/auto.sh`，无需手工修改生成后的 rootfs。SDK 生成的 `upgrade.zip` 按原有烧录/升级流程写入设备。

默认上电启动相机完成拍照和 MP4 录像，网络辅助模式为 `off`，随包包含上述联调用 AP 配置。确认手机连接和预览正常后，可保存 `ap` 或 `sta` 开机网络模式；相机自动采集不依赖网络连接。本次只改应用；如果此前已验证 4M JPEG pipeline，不需要为本次 MP4 流程重编 RTOS。

## 板端使用

预览使用 ATBM 固件提供的 AP 模式；没有使用 `hostapd` 或独立 DHCP 服务。以下为兼容的管理命令；data 分区部署时将 `/usr/bin/bf01-preview` 换成 `/mnt/data/bf01-preview`。如需修改热点（SSID、WPA2 密码、信道）：

```sh
/usr/bin/bf01-preview configure-ap BF01-PREVIEW '替换成至少8位密码' 6
/usr/bin/bf01-preview stop
/usr/bin/bf01-preview start ap
/usr/bin/bf01-preview status
```

信道参数支持 1–13；请按设备部署地区及环境选择允许使用的信道。板端保存的凭据位于 `/mnt/cfg/bf01-preview/`，权限为仅 root 可读写，覆盖镜像内的工厂配置。工厂配置以 `0600` 权限打包；不要公开分发含私有密码的镜像。管理脚本或开机启动时日志在 `/run/bf01-preview/service.log`，直接执行二进制时日志默认输出到终端。启动 AP 和预览就绪状态可通过 `status`、`/run/bf01-preview/ap.ready` 和 `/run/bf01-preview/media.ready` 检查。

手机连接热点后，可用支持 RTSP/TCP 的播放器测试：

```text
rtsp://<板端 wlan0 地址>:8554/live1
```

JL25 客户端控制服务使用 TCP 80，设备状态长连接使用 TCP 6035，事件连接使用 TCP 8222；设备发现使用 UDP 49142。预览接口只响应视频预览相关请求，不支持其它相机功能。停止预览：

```sh
/usr/bin/bf01-preview stop
```

停止会请求 ATBM 固件切回 STA，并尝试恢复 SDK Wi-Fi 服务。若需要开机自动进入 AP，先完成一次 AP 配置，再执行：

```sh
/usr/bin/bf01-preview autostart ap
```

该设置写入持久化的 `/mnt/cfg`，下一次上电生效；恢复默认不接管 Wi-Fi（本地自动拍录仍启动）：

```sh
/usr/bin/bf01-preview autostart off
```

不建议把 `--boot ap` 用于首次启动且未随镜像提供 AP 配置的场景。默认保持 `off`，确认板端实际 AP/手机连接工作正常后再选择网络模式。`bf01-preview boot` 是旧兼容入口，`off` 时不会启动应用；正常固件启动由 `auto.sh` 完成本地采集，不使用这个旧入口。

## 4M 单张 JPEG 拍照

当前 GC4683 的 4M 模式输出为 2560×1440（368.64 万像素）。照片保留当前 ISP/HDR 处理后的完整输出尺寸，不裁剪、不缩放；输出为有损 JPEG，不是 Sensor RAW，也不从 H.264 预览流截图。

RTOS pipeline 保留原有 VPSS GRP0 / VENC0–2 三路视频，新增 VPSS GRP1（Device0）CHN0，使用独立 VB Pool4（三块全尺寸 NV21 缓存、取帧深度 1），与原有 GRP0 共同接收 VI0。VENC3 为 2560×1440 JPEG，质量 85，码流缓存 4 MiB，关闭 ES buffer queue，采用不绑定、手动送帧的方式。应用收到请求时先丢弃队列内旧帧，再取新帧编码、保存并释放资源。

此变更必须同时更新 RTOS 和应用；只替换 `param_config.ini` 或只执行 `pack_rootfs` 不会创建 RTOS 的拍照通道。在已同步 BF01 BSP 的 SDK 根目录，同一个终端依次执行：

```bash
export SCENES=1
source build/envsetup_soc.sh
defconfig cv1815ja_bf01_spinand
build_alios
cd ipcamera_bf01
make ipcamera && make ipcamera install && make ipcamera pack_data
cd ..
pack_rootfs && pack_upgrade
```

每步成功后再执行下一步，烧录更新后的完整升级包。应用必须使用本次打包的参数文件。正常日志应包含 `Use fastboot venc[3]: codec=26, size=2560x1440`（PT_JPEG），原有三个 RTSP session 仍只使用 VENC0–2。

板端先启动预览或应用，确认 SD 卡已挂载到 `/mnt/sd` 且可写，再触发拍照：

```sh
mount | grep ' /mnt/sd '
kill -USR1 "$(pidof ipcamera)"
sleep 2
ls -lh /mnt/sd/Venc3_idx_*.jpg
```

一次手动触发保存一张照片，文件名仍沿用应用的 `Venc3_idx_0.jpg`、`Venc3_idx_1.jpg` 格式；自动任务照片使用 `DCIM/PHOTO/DSCF%05d.jpg`。不要高频连续发送信号，标准信号和单次待处理标志可能合并请求。程序重启或计数达到 1000 后会重用手动拍照编号，应及时转存照片。手动触发前仍需确认 SD 卡已挂载到 `/mnt/sd`；自动流程会尝试挂载卡，但手动接口本身不负责挂载。自动拍照与手动拍照共用 JPEG 编码互斥锁，避免同时送帧。

本次不增加 APP/HTTP 拍照接口。验收时需检查实际 JPG 尺寸为 2560×1440、画面完整、重复拍照正常，并在拍照和 MP4 录像过程中持续观察原有预览是否正常。旧 `/tmp/rec` 调试入口仍是裸流测试，不属于本次 MP4 流程，不要用它验证 MP4。

## 验证边界

自动采集流程已进行主机模拟测试：三种分辨率各 5 秒/10 秒 MP4 均检查时长并完整解码；验证先拍照后录像、任务目录唯一、无卡等待、配置非法、拍照失败、码流异常及正常退出中断的状态。交叉编译和链接使用项目已有的 FFmpeg 静态库，不新增板端 FFmpeg 共享库依赖。这些测试不等于板端通过，仍需烧录后验证 SD 自动挂载、真实硬件时间戳、MP4 可播放、采集同时预览以及 5 秒/10 秒设置生效。

已在本机回环接口用当前预览服务源码验证 HTTP 响应：视频未就绪时 `/SetMode?VideoCapture` 拒绝请求，就绪时返回 `VideoCapture`；`/Misc?Info`、`/app/getmediainfo` 和 TCP 6035 首次心跳也通过。该回环测试不经过 ATBM 无线链路、AP 固件或手机 APP，不能证明 AP 地址分配/DHCP、APP 握手兼容性或实际 H.264 解码；这些还需要在板端和手机上验证。

2026-10-26 临时程序实机验证：任务 `task_caActe` 的 1080P MP4 为 150 帧、10.000 秒，可完整解码；正常卸载、重挂后照片/视频校验值和任务记录一致。该轮照片保存耗时 95 ms，从采集线程启动到 MP4 完成同步为 10666 ms；这不是整机上电总耗时，也不代表该轮 JPEG 质量通过（该轮热重启照片缺 EOI）。新增完整性检查后已在板端正确拒绝不完整 JPEG，旧正式 ELF 未替换；修复后完整冷启动拍录还需重新编译烧录验证。
