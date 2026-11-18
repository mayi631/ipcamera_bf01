# BF01 APP 统一参数管理

## 唯一生效文件

当前 BF01 没有 CFG 分区，APP 设置统一保存在：

```text
/mnt/data/ipcamera-config/app-settings.json
```

如果真实挂载了 CFG 分区，则统一改用 `/mnt/cfg/ipcamera/app-settings.json`。不使用未挂载目录或 `/run` 冒充持久化存储。

该 JSON 是 APP 可变设置的唯一数据源，不是状态镜像。Wi-Fi 下发、启动策略、录音逐帧静音、扬声器恢复、防闪烁恢复和本地录像配置均经 `app_ipcam_settings.c` 访问它。锁文件 `app-settings.lock` 只负责并发，不保存另一份参数。

查看当前文件及实存值，不需要开启第二个 Wi-Fi 服务：

```sh
/mnt/data/bf01-wifi settings
/mnt/data/ipcamera --settings-show
```

`bf01-wifi status` 中的 `host_settings_path`、`host_settings_errno`、`host_saved_fields` 也是同一文件的读取结果。`wifi_saved_dps` 是模组查询值，不代替本地参数；`host_applied_fields` 是当前服务处理快照，不是另一个参数文件，也不证明画面或业务已经完成。启动日志统一输出 `[settings] active=... fields=... errno=...`。

## 字段与消费者

| 字段 | 真实读取位置 | 生效边界 |
| --- | --- | --- |
| `ai_detect_switch` | 启动 AI 门控 | 下一次启动任务，不强制改变正在运行的任务 |
| `image_filter_level` | 启动 AI 推理配置 | low/medium/high 对应阈值0.30/0.60/0.80；不是准确率保证 |
| `bird_detect_switch` / `squirrel_detect_switch` / `other_detect_switch` | 事件及分类通知策略 | 控制通知，不分别启停模型；AI关闭时不生成分类识别通知 |
| `pir_record_dur` | 启动拍录任务 | APP允许-1/0/10/20/30；-1暂回退10秒，0保留现有不录像语义 |
| `image_anti_flicker` | 相机 ISP 设置和启动恢复 | 当前相机在线时设置及读回，不在线时保留供启动恢复 |
| `spk_vol` | DAC设置、状态查询、播放初始化后恢复 | 改音量，不主动开启功放或播放声音 |
| `mic_switch` | ADC设置、状态查询、RTOS/Linux两路PCM写AAC前的共同录音策略 | 设置和逐帧录音使用同一文件、同一把锁，关录音填等长零样本 |
| `datetime_wm_switch` / `datetime_wm_format` | 首拍前启动缓存 + 现有时间戳IPC策略 | JSON与双槽缓存均保存、读回成功才接受；下次启动在小核首拍前读取；不新增直播水印 |
| `pir_sens_level` / `pir_detect_intv` / `brand_wm_switch` | 设置预留 | 仅保存，不声称业务后端已实现 |
| `status_led_switch` | 当前板型接收逻辑 | 保存，不声称已执行独立灯控制 |

本地 `ipcamera --capture-config SECONDS CHANNEL` 也写该 JSON，不再写 `capture.ini`。时长仍兼容本地1–300秒；额外使用 `capture_enabled`、`capture_channel` 两个本地键。Wi-Fi协议仍只允许其原有时长枚举，不因本地能力扩展而放宽。后续APP下发的 `pir_record_dur` 会覆盖同一字段，不存在两份时长互相遮盖。

## 旧数据迁移

只有统一文件不存在时，首次访问在独占锁内执行一次迁移：

1. 读取原 `event-policy.json` 中保存的字段。
2. 导入原 `/mnt/data/ipcamera-wifi/speaker-volume` 和 `microphone-enabled`。如与旧JSON镜像冲突，以音频后端之前实际读取的各自旧文件为准。
3. 原子保存 `app-settings.json`，同步文件与目录，读回校验成功才继续。

旧文件保留，便于回溯；新版不再更新它们。**统一文件一旦存在，不再读取或合并旧文件，即使旧文件内容后来变化也不会覆盖新值。** 统一文件损坏会明确报错，不自动回退旧文件、重建默认值或放开麦克风。

升级时不要在旧版服务仍运行期间提前生成统一文件，以免旧版继续写旧文件造成迁移快照过早。将新版程序留给下一次正常启动，由新版服务和相机在同一统一锁下完成迁移即可。不为迁移重启当前业务，也不删除旧值。

## INI 文件的剩余职责

`capture.ini`、`boot_ai.ini`、`param_config.ini` 保留为出厂默认、模型路径及底层媒体硬件配置，**APP设置不会再分头写它们**。统一JSON中某个APP字段未设置时，继续兼容原默认值；收到APP字段后，对应消费者优先读取统一JSON，不用INI覆盖它。

这不等于把模型、编码器、GPIO、云端凭证等所有不同生命周期的数据混入一个JSON。早期小核不能读取Linux文件系统，因此只将JSON中的时间戳开关、格式派生为带CRC和代数的启动缓存；它不是第二个用户配置文件，不能反向覆盖JSON。

## 时间戳首拍前生效

- 普通设置仍只保存统一JSON。包含任一时间戳字段的全量/增量设置，在JSON持久化后同步`BF01_TS0`、`BF01_TS1`两份启动缓存，成功才允许ACK0；缺分区、布局不符、擦写或读回失败返回ACK2。同值重试也会检查并修复缓存，不因JSON已经同值而跳过。
- NAND原始分区不是普通文件：本内核MTD字符设备不提供`fsync`，擦除/写页为同步操作，关闭可写描述符调用`mtd_sync`；随后重新打开设备读回、校验CRC和完整记录。JSON仍使用原有文件与目录`fsync`。
- 两槽轮换写入，先保留最新有效副本再更新另一槽，均校验后才确认。若JSON写入后、缓存完成前掉电，这次设置未获成功确认，可能保留旧或新时间戳；下次Linux启动尽力从JSON修复，模组也可重发。不能宣称跨JSON与NAND的掉电原子事务。
- FSBL在释放小核前只读预加载；小核在媒体初始化前验证缓存并选择较新代数。CRC/版本错误、两个同代副本冲突时使用原内置默认，不阻止拍录。缓存读取与RTC读取独立，RTC不可用不丢弃设置。
- 启动时尽力修复缓存，失败只明确告警，不中断原拍摄业务。可用`/mnt/data/ipcamera --settings-sync-boot`单独重试；它只同步设置，不开启第二个Wi-Fi服务、相机或匿名IPC消费者。
- `--settings-show`增加`boot_timestamp`：`errno=0`、`matches_saved=true`、`redundant=true`表示两份缓存有效且与JSON一致；`enabled`和`format`为缓存字段，格式编号0/1/2对应年/月/日优先。该状态不证明板端FSBL/小核已升级或画面已验收。

本功能依赖BSP新分区布局、FSBL、小核和应用配套升级，**不能只替换应用**。SYSTEM尾部缩小512KiB，新增两个256KiB原始缓存槽，DATA物理地址/大小不变但MTD编号由8变10。旧启动脚本不能与新布局混用，否则可能把缓存槽误当DATA；不能在旧UBI卷上直接截尾或在线擦写。迁移、两套A/B系统的同步更新及首次开机种子见`../bf01_bsp/docs/BF01_TIMESTAMP_BOOT_SETTINGS.md`。未执行板端分区变更或烧录。

## 保存与验证

- 全量/增量均按键合并，不清空缺失键，不凭空写入未收到的APP设置。
- 临时文件、文件同步、原子替换、目录同步、读回校验；同值不重写，重试仍检查持久化同步错误。无法保存就不能以ACK0声称设置已保存。
- 所有写入和音频读取共用统一锁，避免并发丢字段及确认后新PCM仍使用旧录音开关。各硬件动作和统一文件不是一个跨设备原子事务：若全量中部分真实后端失败，可以保留已成功处理的字段，但整包返回ACK2并给出逐项错误；不伪装为全部成功。
- 预留设置可按既有用户约定在保存成功后接受，格式化等瞬时动作不保存、不重放，不享受占位成功。
- `memory_format=true` 现已接入真实异步 SD 格式化后端；取得存储独占权并启动线程后才确认开始，过程和终态按协议上报。插拔、容量和保护规则见 `SD_CARD.md`。
- 本机迁移/并发/PCM/故障注入和ARM编译不能替代新版启动后的APP下发、硬件及媒体验收；不使用旧版本测试冒充新版通过。
