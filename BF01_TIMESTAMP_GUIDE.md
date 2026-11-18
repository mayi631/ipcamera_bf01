# BF01 照片、录像画面时间戳

## APP 设置优先

APP的`datetime_wm_switch`及`datetime_wm_format`以统一`app-settings.json`为准；缺失键才采用下述INI默认。Linux运行时IPC确认不能修改早于其启动已经编码的照片，因此新增双槽派生缓存：FSBL在释放小核前读取，小核在媒体初始化前采用，避免首拍使用旧格式而录像后半段才切换。

三种APP字符串依次映射为0/1/2：`yyyy-MM-dd HH:mm:ss`、`MM/dd/yyyy HH:mm:ss`、`dd-MM-yyyy HH:mm:ss`。没有把格式硬编码为月日年；开关和格式的单独设置也会保留另一个已存字段。

需要配套BSP的`0046`、`0047`公共补丁、新分区布局、FSBL、小核及应用。首次升级保留已有APP参数时，必须用实存JSON生成缓存种子并保留该JSON，不能指望Linux启动后补发IPC挽回第一次首拍。详细升级限制见`../bf01_bsp/docs/BF01_TIMESTAMP_BOOT_SETTINGS.md`及`APP_SETTINGS.md`。

## 出厂默认开关

编辑应用源码 `resource/parameter/fbm/cv1815ja_bf01_gc4683.ini`：

```ini
[boot_timestamp]
enabled = 1
format = 0
utc_offset_minutes = 480
external_rtc_utc_offset_minutes = 480
```

- `enabled=1`：开启照片和录像的画面时间戳；`enabled=0`：两者均关闭。
- `format`：日期格式，照片和录像共用；缺省使用 `0`，仅允许 `0`、`1`、`2`。
- `utc_offset_minutes`：内部 UTC 秒数转换为画面时间时的显示偏移，范围为 -720～840 分钟。
  默认 `480` 为 UTC+8；需要显示 UTC 时设为 `0`。此参数不修改 RTC 时间。
- `external_rtc_utc_offset_minutes`：外部 BM8563 已存日历的时区，范围同上。
  当前板端实读为北京时间，因此默认 `480`；外部芯片确实保存 UTC 时才改为 `0`。
  读取时先减此偏移换成 UTC，显示时再加显示偏移，两者不能混为同一个参数。
- 左下角白字、黑色描边、透明背景；三种格式均保留完整的时、分、秒。

| `format` | 日期格式 | 显示示例 |
| --- | --- | --- |
| `0`（默认） | 年-月-日 时:分:秒 | `2026-11-06 13:24:56` |
| `1` | 月/日/年 时:分:秒 | `11/06/2026 13:24:56` |
| `2` | 日-月-年 时:分:秒 | `06-11-2026 13:24:56` |

这是APP键缺失或缓存失效时使用的默认值，不是Linux `capture.ini`中的运行时开关。
不复用SDK媒体`param.bin`、A/B的`misc`或RTC寄存器保存APP参数。

BF01 fastboot 的 Makefile 在调用 scons 前运行 BSP 中的
`generate_timestamp_config.py`，从上述应用 INI 生成
`cvi_alios/solutions/fastboot/out/bf01_boot_timestamp_config.h`，随小核镜像内置。
同时生成`timestamp.bin`与`timestamp_bak.bin`。开机由FSBL有界读取两个原始缓存槽，
不让小核访问NAND驱动，不读取Linux文件系统，也不等待大核或SD卡挂载。
配置值非法或应用 INI 缺失会中止构建；内容未变化时不改头文件时间，避免无谓重编译。

需要同时具备 BSP 定制目录、`patches/0039-cvi_alios--feat-generate-bf01-timestamp-configuration.patch`
中的 SDK Makefile 修改，以及 `patches/0040-cvi_alios--fix-pthread-getschedparam-declaration.patch`
中的 POSIX 公共头文件声明补齐，使用正常 `build_alios`/`build_all` 流程构建。
修改这里的INI出厂默认或时区时，仍需重新编译并更新小核`yoc.bin`。
完整升级到启动缓存方案之后，APP设置开关、格式不再需要重编小核，保存成功供下次启动使用。
只改旧板端INI或只更新`data.spinand`不能代替首次完整固件升级。

## 实现范围

- 三张 JPEG：在 `VPSS(1,0)` 输出处、JPEG 编码之前叠加。
- H.265 录像：在 `VPSS(2,0)` 输出处、录像编码之前叠加，随 RTC 秒数更新。
- 不在 `VPSS(1,1)` 的 640×640 AI 输入中叠加，避免文字干扰识别。
- Linux 保持原来的 JPEG 保存和 H.265/AAC MP4 封装流程，不解码重编码，
  不改变编码格式、录像分辨率、AI 放行规则或 ION 内存图。
- RGN 句柄 `96` 专用于此水印，拍照完成、录像停止或失败时释放；不能分配给其他 OSD。
  画布为 384×32 ARGB1555 双缓冲，像素缓冲约 48 KiB，另有驱动管理开销。
  拍照与录像顺序复用该句柄，不同时保留两套画布。
- 水印准备使用低于抓拍线程的优先级，与等待首个 VI 帧并行。首拍不等待水印线程；
  如果准备尚未完成，记录 `prepare pending; photo does not wait for watermark`，
  本帧不保证有水印，优先保持拍照时序。准备线程只在连拍结束后回收。
- 不修改 ISP bypass、sensor 参数、预热丢帧、连拍间隔和原有 pipeline。
  关闭水印时准备线程直接返回，不创建RGN。

## 时间来源与异常

小核完成引脚和控制台初始化后，在媒体初始化前通过 IIC1 读取外部 BM8563，将有效日历转换为 UTC，
以此次真实读取的时间为基准，加小核 64 位单调计时器经过的秒数，按编码时刻叠加；
不在每帧访问 IIC1，不使用大核落盘时刻，也不写入编译时间或伪造日期。
外部 RTC 的接线为 `PAD_MIPI_TXP2/B5 → IIC1_SCL`、`PAD_MIPI_TXM2/A5 → IIC1_SDA`，
七位设备地址为 `0x51`（规格书写地址 `0xA2`、读地址 `0xA3`）。开机连续读取
`0x00–0x08`，包含控制状态和 `0x02–0x08` 秒到年；检查 STOP、VL、BCD、日历日期和世纪位。
只在读事务前发送寄存器地址指针，不写外部芯片的时间、控制、闹钟或定时器寄存器。
读取失败、STOP/VL 置位或日期无效会打印原始状态，保留原有片内 RTC/Linux 校时路径；
不等待外部 RTC 恢复，不阻止拍照录像，传输等待使用共用 20ms 超时预算。
连续读采用先排入九个读命令、再接收的方式，避免控制器因等待后续命令保持 SCL 低；
异常仅复位并释放 IIC1 控制器，不改写 BM8563，也不影响摄像头 IIC3。

内部时间仍使用 UTC，外部芯片日历先按 `external_rtc_utc_offset_minutes` 转换，
显示时仍使用 `utc_offset_minutes`。BM8563 本身没有时区字段；当前板端保存北京时间，
此前直接当作 UTC 导致实拍水印多加八小时，此处将存储时区与显示时区明确区分。
当前改动只读外部 RTC，**不会把 Linux/云端校时时间写回 BM8563**，首次设时及掉电保持需另行验证。
成功日志含原始寄存器、`stored_utc_offset_minutes`、`utc_seconds`、读取耗时及 `IIC1 released`，
随后打印 `clock source=BM8563+monotonic`。外部时间有效时不再先写片内 RTC 再依赖回读：
实机曾出现写入后回读为 0、旧值及计数速率异常，导致冷启动水印缺失或走时不准。
外部 RTC 无效时保留原片内 RTC/Linux 校时路径，不绕过 VL、STOP 或日期检查。

大核启动后以及运行中每 60 秒检查 Linux 时间，只有日期有效才异步将 UTC 秒数
通过独立 IPC 端口 5 发送给小核，由已有 RTC HAL 写入硬件。小核在连拍完成后才
注册校时处理，不阻塞首拍；不使用编译时间、文件时间或固定日期作为校时来源。
校时时按 SDK RTC 驱动的方式开启秒计数，但不清除原有闹钟使能；RTC 读写的
短寄存器序列使用中断保护，避免水印刷新与校时相互打断。
Linux 时钟仍为 1970 年时只记录等待外部校时，不向 RTC 写入无效时间。
Linux IPC 校时仍须片内硬件真实确认才返回成功；确认后同时刷新水印的时间基准，
不拿软件基准冒充片内 RTC 的硬件确认，也不修改原 IPC 端口或消息格式。

正常运行时，通过上位机、客户协议或网络把 Linux 系统时钟设置为正确时间，
由主应用在下一次检查时自动同步，不要额外启动校时进程。此同步仅更新片内 RTC，
不会设置 BM8563；下次冷启动仍首先读取外部芯片中已有的时间。

需要立即校时并确认时，先停止主应用，确认没有其他进程使用匿名 IPC，再执行：

```sh
/mnt/data/bf01-preview stop
LD_LIBRARY_PATH=/mnt/data/lib /mnt/data/ipcamera --sync-rtc
```

此命令只做 RTC 校时，不进入普通拍照/录像/识别入口。主应用与校时命令在初始化
IPC 前互斥持有 `/run/bf01-preview/ipcm.lock`；主应用仍在运行时命令直接拒绝，
不创建第二个匿名消息读取线程，不抢占拍录消息。不要与服务启动操作并行执行。
端口 6 接收小核读取硬件后的确认，确认等待最多约 1 秒，IPC 清理还可能耗时约
3 秒；成功打印 `RTC sync confirmed`，失败返回非零。
这是首次配置/维护操作，不在首拍路径上。自动同步的 `RTC sync queued` 只代表
已发送，须结合小核日志中的 `confirmed` 非零、`error=0` 判断硬件是否确认。
当前年份不能凭空获得：首次出厂 RTC 未校时的那次启动，已拍的照片不会有正确日期。
需要先校准并确认 RTC 保持走时，之后再上电验证首拍；若彻底断电导致 RTC 丢时，
仍需重新校时，不能用等待大核来掩盖这个硬件/产品前提。

RTC 无效（早于 2020 年或达到 2100 年）时，隐藏时间戳并记录：

```text
[boot-timestamp] RTC not synchronized; watermark hidden until valid
```

拍照录像继续原流程；后续 RTC 有效后，仍在录制的画面会开始显示时间戳，
已经缓存的 JPEG/H.265 不会被补写时间戳。RGN 创建、更新失败也会记录明确错误，
继续拍录而不阻塞首拍或取消正常媒体保存。关闭时不创建 RGN 画布。

RGN 初始化不在抓拍线程内执行，但线程创建、CPU 调度及每帧画布更新仍有少量开销；
没有新增等待不等于绝对零开销，未做实机测量，不能保证首拍毫秒数完全不变。
日志分别记录 `prepare done` 的耗时，以及原有 `first-photo timing` 的
`boot_to_frame_ms`、`boot_to_photo_ms`，不能将 `media_ready_ms` 当作首拍完成时间。
首次烧录后应分别验收开启、关闭、RTC 未校时三种情况，检查三张照片、录像头尾、
跨秒变化，以及 AI 输入无水印。外部 RTC 读取需另外核对开机寄存器日志和实际芯片时间，
不能用编译通过代替实机 IIC 通讯、首拍延时和断电保持验收。
