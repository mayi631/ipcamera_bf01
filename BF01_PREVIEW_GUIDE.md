# BF01 远程网络预览

本功能只实现 live preview：沿用 BF01 已有 VENC/RTSP 码流，通过 JL25 风格控制接口返回预览地址；不实现录像、拍照、回放或配置类功能。默认不自动开启预览，也不改 SDK 的 `S99user`、`auto.sh`、无线驱动或 GPIO。

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

首次使用时，先按下方步骤在板端配置并启动 AP。确认手机连接和预览正常后，可保存自动启动模式；预览程序默认不自动启动，也不会把 AP 密码写入固件。

## 板端使用

预览使用 ATBM 固件提供的 AP 模式；没有使用 `hostapd` 或独立 DHCP 服务。首次使用时，在板端配置热点（SSID、WPA2 密码、信道）：

```sh
/usr/bin/bf01-preview configure-ap BF01-PREVIEW '替换成至少8位密码' 6
/usr/bin/bf01-preview start ap
/usr/bin/bf01-preview status
```

信道参数支持 1–13；请按设备部署地区及环境选择允许使用的信道。以上板端配置方式将凭据保存在 `/mnt/cfg/bf01-preview/`，权限为仅 root 可读写，不会把密码写进 rootfs。若构建时显式给安装脚本传 `--ap-config`，密码会以 `0600` 权限复制进 rootfs 镜像；不要将包含该配置的镜像公开分发。运行时日志在 `/run/bf01-preview/service.log`，启动 AP 和预览就绪状态可通过 `status`、`/run/bf01-preview/ap.ready` 和 `/run/bf01-preview/media.ready` 检查。

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

## 验证边界

已在本机回环接口用当前预览服务源码验证 HTTP 响应：视频未就绪时 `/SetMode?VideoCapture` 拒绝请求，就绪时返回 `VideoCapture`；`/Misc?Info`、`/app/getmediainfo` 和 TCP 6035 首次心跳也通过。该回环测试不经过 ATBM 无线链路、AP 固件或手机 APP，不能证明 AP 地址分配/DHCP、APP 握手兼容性或实际 H.264 解码；这些还需要在板端和手机上验证。
