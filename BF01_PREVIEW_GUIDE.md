# BF01 远程网络预览

统一相机入口为编译生成的 `ipcamera`。网络预览服务运行在该进程内，默认由它启动 AP 网络辅助进程；辅助进程不再启动相机。不要另起第二个 `ipcamera`，也不需要先启动 `bf01-preview`。单实例锁拒绝重复启动，退出相机时停止所属网络辅助进程并恢复 STA。

网络控制协议只实现 live preview：沿用 BF01 已有 VENC/RTSP 码流，通过 JL25 风格控制接口返回预览地址；不增加远程拍照、录像、回放或配置类接口。原有板端 SIGUSR1 单张 JPEG 拍照和录像测试入口仍保留，本次不修改录像实现。默认不自动开启预览，也不改 SDK 的 `S99user`、无线驱动或 GPIO。

## 单应用启动

烧录本次重新编译、安装和打包的固件后，在板端只执行：

```sh
/mnt/data/ipcamera &
```

默认模式为 AP。无已保存配置时，使用应用随包安装的 `ap.conf`：SSID `BF01-PREVIEW`，密码 `12345678`，信道 6。这是联调默认密码，量产前务必修改 `resource/preview/ap.conf` 并重新打包，或用原有 `bf01-preview configure-ap` 保存私有配置；`/mnt/cfg/bf01-preview/ap.conf` 优先于随包默认值。程序需要同目录的 `bf01-preview`（内部网络辅助脚本）、`bf01_atbm_ap` 和参数文件，不能只拷贝一个 ELF 文件。

手机或电脑连接该热点，在支持 RTSP/TCP 的播放器打开 `rtsp://192.168.43.1:8554/live1`；实际地址以 `ifconfig wlan0` 为准。只有收到有效 H.264 参数和关键帧后才生成 `/run/bf01-preview/media.ready` 和 `service.ready`，`pidof ipcamera` 应只有一个 PID。

其它启动方式仍只运行同一个应用，不要同时执行：

```sh
BF01_WIFI_MODE=sta /mnt/data/ipcamera &
BF01_WIFI_MODE=off /mnt/data/ipcamera &
```

`sta` 使用 SDK 已保存的网络连接；首次先用 `wifi connect` 配网。`off` 不接管 Wi-Fi，但仍运行相机和网络预览服务，适用于已有网络或仅验证媒体功能。停止统一应用使用 `kill -TERM "$(cat /run/bf01-preview/ipcamera.pid)"`，等待进程退出后再重启。

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

可直接按“单应用启动”操作，也可以先修改 AP 配置。确认手机连接和预览正常后，可保存自动启动模式；默认不开机自启，但随包包含上述联调用 AP 配置。

## 板端使用

预览使用 ATBM 固件提供的 AP 模式；没有使用 `hostapd` 或独立 DHCP 服务。以下为兼容的管理命令；data 分区部署时将 `/usr/bin/bf01-preview` 换成 `/mnt/data/bf01-preview`。如需修改热点（SSID、WPA2 密码、信道）：

```sh
/usr/bin/bf01-preview configure-ap BF01-PREVIEW '替换成至少8位密码' 6
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

该设置写入持久化的 `/mnt/cfg`；恢复默认不开机启动：

```sh
/usr/bin/bf01-preview autostart off
```

不建议把 `--boot ap` 用于首次启动且未随镜像提供 AP 配置的场景。默认保持 `off`，确认板端实际 AP/手机连接工作正常后再选择自动启动。

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

一次触发保存一张照片，文件名沿用应用的 `Venc3_idx_0.jpg`、`Venc3_idx_1.jpg` 格式；不要高频连续发送信号，标准信号和单次待处理标志可能合并请求。程序重启或计数达到 1000 后会重用编号，应及时转存照片。未挂载 SD 卡时不要测试，以免写入挂载点所在文件系统。应用不会替你挂载 SD 卡或创建保存目录。

本次不增加 APP/HTTP 拍照接口。验收时需检查实际 JPG 尺寸为 2560×1440、画面完整、重复拍照正常，并在拍照过程中持续观察原有预览是否正常；VI 双 Group 分发、内存余量和拍照时的视频实时性仍需板端验证。

## 验证边界

已在本机回环接口用当前预览服务源码验证 HTTP 响应：视频未就绪时 `/SetMode?VideoCapture` 拒绝请求，就绪时返回 `VideoCapture`；`/Misc?Info`、`/app/getmediainfo` 和 TCP 6035 首次心跳也通过。该回环测试不经过 ATBM 无线链路、AP 固件或手机 APP，不能证明 AP 地址分配/DHCP、APP 握手兼容性或实际 H.264 解码；这些还需要在板端和手机上验证。
