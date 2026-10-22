# CV1815JA BF01 编译和运行

BF01 JL25 远程网络预览的编译、固件打包和板端操作步骤见 [BF01_PREVIEW_GUIDE.md](BF01_PREVIEW_GUIDE.md)。

使用 `configs/cv1815ja_bf01_defconfig`，目标程序为 `ipcamera`。
将本仓库放在 SDK 根目录下，与 `build`、`cvi_mpi`、`host-tools` 等目录同级；SDK 的头文件和库需已编译就绪。

## 1. 编译和安装

从 SDK 下的应用目录开始，在 Bash 中按以下顺序执行。示例应用目录名为 `ipcamera`；若本仓库目录名为 `ipcamera_bf01`，将下面的 `cd ipcamera` 改为 `cd ipcamera_bf01`。

```bash
export SCENES=1
source build/envsetup_soc.sh
defconfig cv1815ja_bf01_spinand
cd ipcamera
ls configs/
make cv1815ja_bf01_defconfig
make ipcamera
make ipcamera install
```

`source build/envsetup_soc.sh` 加载 SDK 编译环境，`defconfig cv1815ja_bf01_spinand` 选择 SDK 板级配置，`make cv1815ja_bf01_defconfig` 选择应用配置。
环境初始化、配置选择、编译和安装需在同一个终端中完成。

## 2. 产物和上板运行

程序输出为 `out/ipcamera/ipcamera`，模块静态库输出到 `out/lib/`。
`make ipcamera install` 将程序复制到 `install/ipcamera`，将 BF01 参数文件复制到 `install/param_config.ini`，并按 SDK 分区配置打包参数镜像 `param.bin`。

将 `install/ipcamera` 和 `install/param_config.ini` 一起复制到板子的 `/mnt/data/`，然后在板上执行：

```bash
chmod +x /mnt/data/ipcamera
/mnt/data/ipcamera -i /mnt/data/param_config.ini &
```

BF01 使用快起，SYS、VI、VPSS、VENC 由小核 AliOS 初始化，参数文件中的四个 `alios_*_mode` 均为 `1`。
对应小核 pipeline 为 SDK 中的 `cvi_alios/solutions/fastboot/customization/cv1815ja_bf01_spinand/pipeline/`。
当前配置提供三路 H.264：

| RTSP 地址 | VENC 通道 | 分辨率 | 帧率 |
| --- | --- | --- | --- |
| `rtsp://<板子IP>:8554/live0` | 0 | 1920 × 1080 | 15 fps |
| `rtsp://<板子IP>:8554/live1` | 1 | 1280 × 720 | 15 fps |
| `rtsp://<板子IP>:8554/live2` | 2 | 640 × 360 | 15 fps |

在 SDK 根目录执行：

```bash
export SCENES=1
source build/envsetup_soc.sh
defconfig cv1815ja_bf01_spinand
build_alios
```

打包后的分区镜像为 `install/soc_cv1815ja_bf01_spinand/yoc.bin`，裸固件为该目录下的 `rawimages/yoc.bin`。使用 SDK 的烧录流程更新小核固件后重启，再连接 RTSP 验证帧率。

## 目录说明

| 目录/文件 | 说明 |
| --- | --- |
| `configs/` | 编译配置 |
| `solutions/ipcamera/src/` | 程序入口 |
| `modules/` | 媒体、RTSP、网络和外设模块 |
| `components/` | RTSP、OSAL 和环形缓冲组件 |
| `prebuilt/` | 第三方预编译库 |
| `resource/` | 参数文件、模型和其他资源 |
| `out/` | 编译输出 |
| `install/` | 上板程序和参数文件 |
