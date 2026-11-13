# 主控与 Wi-Fi 实机联调结论

**最新真实分片与直播结果见第 13 节：2026-11-10 APP 下发的两片 start 已实收、拼接并执行；真实 ANNOUNCE 的 URL 与模组下发逐字节一致，流媒体服务器约 3 秒后返回 `401 Unauthorized`，正文为 `[network err]:3(connection refused)`。直播尚未通过，不能把该回复直接说成 Wi-Fi 故障或令牌错误。第 11 节及之前的断言现场属于旧轮次，不与本次部署混算。**

测试日期：**2026-11-10**。依据：`主控联调测试例子.md`，字段参考 `主控与WIFI属性联动协议.md`。以下部署轮为 09:17—09:24；09:45 用户断电重启后的因果对照见 2.2.1，**最新同 ID 修复、编译上板及复测见 2.2.2**。均使用真实正式程序，不把不同轮次结果混算。

## 1. 本次部署与结论

正式 ARM 应用编译、install、ADB 部署并启动成功；新增的 `bf01-wifi get NAME` 只是将只读查询交给**同一个生产服务**排队，不是独立测试收发程序，不启动直播。自动接收 set、确认、状态查询均由原生产服务执行。

- 板端最终应用 MD5：`296206ccf922fcfd27baaabd409979d4`；管理脚本 MD5：`b53093af1f798885297ddc0c88220273`；驱动 MD5：`06220a534f736bcaa7363e52ee2c86f5`。应用与本机编译、install、SDK data 待打包文件一致。
- 最终服务 PID **944**；统计主控时间 **09:17:35.152—09:23:59.915**，对应 COM9 电脑取证窗口 **09:17:33—09:24:31**，均为 UTC+8。主控时间约快于电脑 2 秒，不跨时钟计算延迟。此前 PID 500 预检不计入最终统计。
- ADB `0123456789`、COM9 实际可用；只有一个协议服务，另有原网络 watcher PID 318。wlan0 `192.168.10.147/24`，网关/DNS `192.168.10.1`；TCP 过滤前后 flags=7、源/目的端口均为 18883。没有重载驱动、清绑定或修改 AI/抓拍/直播。

09:17 部署轮未重启模组，没有采集启动构建号。主机侧已重新生成 SDK 输出目录 `install/soc_cv1815ja_bf01_spinand/data.spinand`，SHA256 为 `568c5990f413d2c575fcb8664614bd384910a559026f39f0e146ca4778ee820a`；离线解出应用、脚本、驱动和既有配置共 8 个文件，与待打包内容逐字节一致。**板端此次通过 ADB 更新正式应用/脚本，没有 dd 覆盖 SDK 分区。**

**最终版实收 667 条记录、228 次完整交换，无传输 errno/status 错误，无应答错 ID。不能因此说全部用例通过：本轮复现了确认超时，并发现 stream 空闲查询返回旧 ID。**

| 联调例子 | 结论 | 具体原因/责任 |
| --- | --- | --- |
| 第 1 节 ACK 同次返回、第 3 节 set 收发 | **通信通过，设置成功闭环未完成** | 211 条全量 set 均能接收，主控回同 ID ACK2，Wi-Fi 同次返回空串；不能把 ACK2 算设置成功。主控未实际完成设置，详见 2.1 |
| 第 1 节：get/http 期间及时确认异步 set | **失败** | set 11742 发送后约 **11.574 秒**才收到主控 ACK，模组已按超时重新全量同步；主控同步请求占通道，详见 2.2 |
| 第 6.1/6.2：get/status、get/dps | **通过** | 同次返回自身 ACK0，字段完整；分别 8 次、2 次，见第 3 节 |
| 第 6.3：get/http | **ACK2 分支通过；取得凭证未完成** | 1607/1609/1611 均返回 ACK2，没有整个 http 对象；不是空令牌或 JSON 非法。订阅 -1 的内部根因仍待确认，见 2.2 |
| 第 6.4：get/stream | **返回内容与例子空闲初态不一致，Wi-Fi 侧待确认历史状态语义** | 两次均为 idle，但 streamId 是旧值而不是空串。本轮没有发送 stream state/start/stop；查询通信和 ID 正常，差异见 2.3 |
| 第 6.5：未知 name | **通过** | other 查询两次均返回自身 ACK1，82 ms，不把正常拒绝当异常 |

## 2. 未通过/未完成项：完整实际收发与原因

### 2.1 set 能通信，但主控一直 ACK2，握手未完成

**Wi-Fi → 主控，0x083B，ID 11711，411 字节**；COM9 `09:17:33.6653533`，主控 sequence 5：

```json
{"cmd":"set","id":11711,"dps":{"pir_sens_level":"high","pir_detect_intv":0,"pir_record_dur":10,"squirrel_detect_switch":true,"ai_detect_switch":true,"bird_detect_switch":true,"other_detect_switch":false,"brand_wm_switch":true,"datetime_wm_switch":true,"mic_switch":false,"status_led_switch":true,"image_filter_level":"medium","image_anti_flicker":"60Hz","datetime_wm_format":"MM/dd/yyyy HH:mm:ss","spk_vol":80}}
```

**主控 → Wi-Fi，0x003B，同 ID**；COM9 `09:17:33.6683564`，sequence 6：

```json
{"cmd":"ack","id":11711,"result":2}
```

**Wi-Fi 同次返回：空串，length=0，128 ms，errno/status=0**，sequence 7。本轮 211 次 ACK 交换均为空串，耗时 41—179 ms；不是未收到回复。

**具体原因在主控，不是 Wi-Fi 报文有错：**15 键和值符合例子，主控 `wifi_apply()` 执行后仍有以下真实错误：

| 实收设置 | 主控本地执行结果 |
| --- | --- |
| pir_sens_level=high、pir_detect_intv=0、pir_record_dur=10；squirrel_detect_switch=true、ai_detect_switch=true、bird_detect_switch=true、other_detect_switch=false；image_filter_level=medium；brand_wm_switch=true、datetime_wm_switch=true、datetime_wm_format=MM/dd/yyyy HH:mm:ss | 这 11 项均走 `app_ipcam_wifi_link.c` 的 ENOSYS 分支，errno=38；不是模组返回了这些 errno |
| image_anti_flicker=60Hz | 请求原相机控制 socket 返回 ENOENT，errno=2；本轮 camera-status 同样返回 2，相机控制 socket 不存在 |

所以主控回 ACK2，没有 ACK0，也没有自动推进全量 report。**收发协议正常，但设置成功及后续握手不能算通过。**主控需要补真实执行/恢复正常相机控制前置；Wi-Fi 不需要删除这些合法字段。未为测试伪造 ACK0 或属性上报。

### 2.2 get/http 返回 ACK2且等待约 15 秒，挡住 set 确认

以下均为正式服务发 `0x003B`、Wi-Fi 在**同一次交换**返回的完整原文，errno/status 均为 0：

| 主控实际发送 | Wi-Fi 实际返回 | 耗时 | sequence |
| --- | --- | --- | --- |
| `{"cmd":"get","name":"http","id":1607}` | `{"cmd":"ack","id":1607,"result":2}` | 15186 ms | 193/194 |
| `{"cmd":"get","name":"http","id":1609}` | `{"cmd":"ack","id":1609,"result":2}` | 15173 ms | 260/261 |
| `{"cmd":"get","name":"http","id":1611}` | `{"cmd":"ack","id":1611,"result":2}` | 15173 ms | 327/328 |

**凭证没取得的准确含义：**回复只有 cmd/id/result，**整个 http 对象没有返回**。没有实收 accessToken/httpHmacSignKey/httpAesKey/expireTime，不是某个密钥为空，也没有 expireTime 可判非法。例子第 6.3 节允许 ACK2，所以 ACK2 本身合规；本轮没有取得凭证成功分支。

第一轮同时发生以下异步下发，**Wi-Fi → 主控，0x083B**，主控 sequence 198 保存完整实收内容：

```json
{"cmd":"set","id":11742,"dps":{"pir_sens_level":"high","pir_detect_intv":0,"pir_record_dur":10,"squirrel_detect_switch":true,"ai_detect_switch":true,"bird_detect_switch":true,"other_detect_switch":false,"brand_wm_switch":true,"datetime_wm_switch":true,"mic_switch":false,"status_led_switch":true,"image_filter_level":"medium","image_anti_flicker":"60Hz","datetime_wm_format":"MM/dd/yyyy HH:mm:ss","spk_vol":80}}
```

**主控 → Wi-Fi，0x003B**，sequence 199：

```json
{"cmd":"ack","id":11742,"result":2}
```

Wi-Fi 对这个 ACK 的同次返回：**空串，length=0，41 ms，errno/status=0**，sequence 200。报文匹配，但确认晚了：

| 同一 COM9 电脑时钟 | 实际事件 |
| --- | --- |
| 09:19:15.0089690 | 模组记录收到 get/http 1607，开始换凭证 |
| 09:19:15.1479680 | 模组记录凭证主题订阅失败，错误码 -1 |
| 09:19:18.7818670 | 模组发送 set 11742 |
| 09:19:21.8316721 | 同 ID 重发；09:19:24 的后续重发串口有混行，原文保留，不人工补 JSON |
| 09:19:27.7525561 | 模组记录“应答失败，重新从全量 set 开始” |
| 09:19:30.2105258 | 模组记录 get/http 1607 的 ACK2 |
| 09:19:30.3555312 | 模组才记录收到主控对 set 11742 的 ACK2，距下发约 11.574 秒 |

**已定位的确认超时原因：**主控正式服务阻塞在同步 `bf_wifi_sdio_exchange()`，返回后才消费积压 set；等待期间实际 `/proc/944/wchan` 为 `wsm_cmd_send`。驱动 Direct Buffer 从 `wsm_cmd_lock()` 到 `wsm_cmd_unlock()` 串行占用单一 WSM 请求上下文。不能靠再开一个消费者或删锁安全修复。

**双方具体下一步：**主控负责协调请求调度，保证异步确认不被长同步请求挡住；Wi-Fi 开发需检查 get/http 回调使用的任务、MQTT client/锁、SUBACK 及订阅返回 -1 原因，并协调短返回/后台刷新方案。不能断言主控完全无关，也没有证据把订阅失败的内部根因直接判给模组。

第一轮 COM9 原文如下；后两轮也分别“订阅 -1→未换到→稍后更新”，见 serial-current：

```text
09:19:15.1479680 蓝牙添加：订阅失败，主题=v1/sys/P_SBF_F5gT8bN7/D_1A18BB28B91fDq5ems6qpzy/service/httpToken/get_reply，错误码=-1
09:19:30.1165963 主控会话：HTTP 凭证未换到
09:19:35.7536745 主控会话：HTTP 凭证已更新
```

本轮没有 TLS `mbedtls_net_connect returned -68`，不能把订阅 -1 混写成 TLS -68。status ID 1608 曾回 cloud=offline，之后恢复 online；这是实际状态，不是 JSON 格式错误，也不足以单独定位掉线根因。

#### 2.2.1 断电重启对照：具体是哪条主控操作触发订阅失败

**已确认触发操作：为验证例子第 6.3 节，执行正式服务的 `bf01-wifi refresh-http`，由原生产服务发送 `get/http`。不是开机自动发起，也不是主控自己连接 MQTT。**入口为 `solutions/ipcamera/src/app_ipcam_wifi_link.c:546`，实际同步发送为 `solutions/ipcamera/src/app_ipcam_wifi_link.c:226`。本次未修改、编译或替换应用/驱动；哈希与第 1 节一致。

用户 09:45:13 断电重启后，正式服务自动启动，PID **326**；wlan0 为 `192.168.10.147/24`，网关 `192.168.10.1`，无其他协议消费者。触发前 `http_attempts=0`、`cloud=online`、`http=ready`。对照结果：

| 实际阶段 | 实收结果 |
| --- | --- |
| 09:45:13—09:46:55：模组开机连接、主控自动接收 set/回 ACK2、status/dps 查询 | 7 条订阅成功日志，0 条订阅失败；其中凭证回复主题 09:45:36.8511467 订阅成功，09:45:37.1319288 凭证已更新 |
| 主控额外 `get/status` 1731、`get/dps` 1732 | 都返回本 ID ACK0，分别 103/186 ms；未触发订阅失败。status 实收为 `{"cmd":"ack","id":1731,"result":0,"status":{"cloud":"online","wifi_signal_level":13,"battery_level":0,"motor_status":"normal","http":"ready"}}` |
| 09:46:55 仅手动触发一次 `refresh-http`，服务后续自动重试 | 三次 get/http 分别紧接着出现同一凭证主题的订阅 -1；三次均约 15 秒后回 ACK2，之后又通过日常连接流程取得凭证 |

**失败项完整收发**，均走 `0x003B` 同次交换，errno/status=0：

| 主控 → Wi-Fi 实际原文 | Wi-Fi → 主控实际原文 | 同步耗时 |
| --- | --- | --- |
| `{"cmd":"get","name":"http","id":1733}` | `{"cmd":"ack","id":1733,"result":2}` | 15185 ms |
| `{"cmd":"get","name":"http","id":1735}` | `{"cmd":"ack","id":1735,"result":2}` | 15175 ms |
| `{"cmd":"get","name":"http","id":1737}` | `{"cmd":"ack","id":1737,"result":2}` | 15174 ms |

不是返回空字符串，也不是返回了空 token，**是整个 http 对象未返回**。ACK2 分支本身符合例子，但换到凭证未完成。修复前主控忙重试使用新 ID，与例子“重发同一条 get”的要求不一致；该独立问题随后已修复并上板，见 2.2.2。**首次 1733 的订阅失败发生在任何重试之前，不是后续换 ID 才造成的。**本轮因果对照未中途修改生产代码。

首轮同一 COM9 电脑时间线（2026-11-10，UTC+8）：

```text
09:46:55.3903121 主控会话：收到 get，id=1733
09:46:55.3903121 主控会话：正在更换 HTTP 凭证
09:46:55.4823479 蓝牙添加：订阅失败，主题=v1/sys/P_SBF_F5gT8bN7/D_1A18BB28B91fDq5ems6qpzy/service/httpToken/get_reply，错误码=-1
09:47:10.4451861 主控会话：HTTP 凭证未换到
09:47:10.4933985 {"cmd":"get","name":"http","id":1733}
09:47:10.5393980 {"cmd":"ack","id":1733,"result":2}
09:47:16.0086617 蓝牙添加：订阅成功，主题=v1/sys/P_SBF_F5gT8bN7/D_1A18BB28B91fDq5ems6qpzy/service/httpToken/get_reply
09:47:16.6596928 主控会话：HTTP 凭证已更新
```

注意：模组的完整请求 JSON 在处理约 15 秒后才打印，**不能用这行 JSON 的打印时间当作到包时间**；到包另有上面的“收到 get”。主控 wire 记录保存调用前的原始请求及单调时钟耗时。

断电前另一轮 PID 312 的请求 1674/1676/1678 也分别回 ACK2（15211/15175/15174 ms），每次都有同样订阅 -1。两轮合计 **6 次触发、6 次失败**；断电后的三次不是沿用历史结果。两轮均未出现 TLS `-68`。

**责任边界及下一步：**
- **确定的主控行为及影响：**测试主动发送合法 get/http；正式服务随即同步等模组回复，现场 wchan 为 `wsm_cmd_send`，网络 watcher 随后等在 `wsm_get_config`，异步 set 确认会被延迟。这部分不能掩盖，也不能靠再开消费者或删 WSM 锁安全修复。
- **尚不能确定的内部原因：**同一个主题在模组开机/日常路径能订阅成功，在处理 get/http 时失败。已收敛到“主控主动换凭证所走的处理路径”，但没有模组应用源码、订阅内部返回分支或调用栈，**不能断言是 MQTT 锁死、句柄错误或主控驱动破坏网络**。只读 taskShow 只有任务状态，不足以证明哪把锁出了问题。
- **给 Wi-Fi 开发的具体核查：**在收到 1733 这类 get/http 的处理入口记录执行任务、MQTT 连接状态、订阅调用返回分支、是否与日常收包/刷新共用 client，以及 SUBACK 是否收到。对照为何日常路径成功、该入口立即 -1、等待结束后重新连接又成功。优先验证将换凭证交给日常 MQTT 任务、Direct Buffer 未就绪时及时 ACK2 的方案，避免占用同步 WSM 约 15 秒；后台完成后仍需按例子在重试的自身 ACK 中给完整 http，不能把“已更新”日志当成已交给主控。

**重启插曲另列，不混为订阅失败：**09:40:57 单独 ADB reboot 时模组保持供电，重新枚举出现模组 `HIF HW ERR0: Buffer Number mismatch - SDIO!`，主控 `WSM startup timeout`、probe `-110`；该阶段 SDIO 前置不满足，没有完成第二轮 get/http。09:44:57/58 曾尝试重启两端，随后用户 09:45:13 真正断电，才建立本小节的干净基线。断电后 SDIO、网络和生产服务均已恢复，不把软件重启后的重连问题归因成 MQTT 订阅 -1。

本次原始证据统一放在 `实机联调证据261110/订阅失败因果复验/`：`pre-reboot-wire.jsonl`、`cold-boot-wire.jsonl` 是完整生产收发；`com9.jsonl`、`com9-fresh.jsonl` 是带电脑时间的串口原文，含混行不修补；`cold-refresh-trigger.log`、`cold-refresh-wchan.log`、`cold-boot-ready.log` 保留触发、等待及版本/网络现场；`causal-summary.json` 和 `*-exchanges.jsonl` 是离线配对。串口在 09:50:58 自然结束并释放。

#### 2.2.2 最新：主控同 ID 重试已修复，订阅失败仍复现

**主控已修复并实际部署：**HTTP 忙回复 ACK2 后，保存原请求，等待 30 秒原样重发，三次均沿用同一 ID；新请求才分配新 ID。链路重同步时取消失效的忙重试，避免一直等待。改动仅在 `bf01_wifi_link.h`、`bf01_wifi_link.c`、`app_ipcam_wifi_link.c`，不改驱动、过滤表、AI 门控、抓拍或直播，也不假回 ACK0。

正式 ARM 编译/install、通信库 `-Wall -Wextra -Werror` 编译通过。09:57 通过 ADB 原子更新正式应用并正常重启生产服务，PID **619**，应用 MD5 **`c67ae0ff6ff20fd9b726df24dc2b35c0`**；驱动和管理脚本哈希未变，AI 输入模式仍为 0。没有运行板端独立测试收发程序。

触发前 `get/status` 1795 返回 `cloud=online,http=ready`，102 ms；`get/dps` 1796 返回全部 15 个设置键，184 ms，均 ACK0。随后仅调用一次生产 `refresh-http`，实收三轮如下；**每轮请求原文完全相同**，通道 `0x003B`，errno/status=0：

| 主控 → Wi-Fi 实际原文 | Wi-Fi → 主控实际原文 | 耗时 | COM9 订阅 -1 的电脑时间 |
| --- | --- | --- | --- |
| `{"cmd":"get","name":"http","id":1797}` | `{"cmd":"ack","id":1797,"result":2}` | 15191 ms | 09:57:54.9154225 |
| `{"cmd":"get","name":"http","id":1797}` | `{"cmd":"ack","id":1797,"result":2}` | 15174 ms | 09:58:40.0513813 |
| `{"cmd":"get","name":"http","id":1797}` | `{"cmd":"ack","id":1797,"result":2}` | 15293 ms | 09:59:25.2632064 |

三条错误的原文相同：

```text
蓝牙添加：订阅失败，主题=v1/sys/P_SBF_F5gT8bN7/D_1A18BB28B91fDq5ems6qpzy/service/httpToken/get_reply，错误码=-1
```

三轮之后日常路径又分别记录凭证更新：09:58:15.6164786、09:59:00.7511747、09:59:46.8361483。主控始终未收到这三轮 get 的 http 对象；没有令牌、密钥或 expireTime 可供解析。第三轮结束即停止重试，现场 `http_attempts=3,http_rounds=3,http_request_id=1797`，没有继续无限发包；最终 SDIO 已打开、网络及 cloud 恢复 online。

**本轮结论：**同 ID 重试修复通过实机验证；128 次完整交换无传输 errno/status 错误，status/dps 查询正常。**get/http 取得凭证仍未完成，订阅失败没有被主控换 ID 修复消除，也未出现 TLS -68。**已排除“重试换 ID”这一具体原因；仍不能凭 -1 判定 MQTT 内部哪一行出错。交给 Wi-Fi 开发的复现入口应使用上面原样的 1797 三轮，按 2.2.1 所列任务/client/订阅返回分支核查；约 15 秒同步等待造成的主控确认延迟仍需两端协调短返回/后台刷新，不能删除驱动锁来掩盖。

完整证据在 `实机联调证据261110/订阅失败因果复验/同ID修复复验/`：`wire-current.jsonl` 仅含 PID 619，`exchanges.jsonl` 逐次配对，`summary.json` 保留三次同 ID、耗时及全部错误；`com9.jsonl` 与 `com9-tail.jsonl` 是连续两段原始采集，不拼补混行。第三轮“未换到”串口行含混入的 RF 日志，原文保留，以完整 wire/ACK 为凭，不在此重构那一行。

最新 SDK `data.spinand` 已重新打包，SHA256 为 **`b0522964e8832f9158accb6d14ad6d7e461e46f79729efddfe72e416bb6c586c`**；离线解出的应用等 8 个文件与当前安装目录一致，包含本次修复，详见 `image-verification.json`。板端本次仅更新应用，没有 dd 刷 SDK 分区；修复版未再次断电验收，不将此前旧版的断电结果冒充本版结果。

### 2.3 get/stream：idle 携带旧 streamId

**主控 → Wi-Fi，0x003B**；COM9 `09:17:34.4151489`：

```json
{"cmd":"get","name":"stream","id":1603}
```

**Wi-Fi → 主控，同次返回**；COM9 `09:17:34.4611502`，103 ms，errno/status=0：

```json
{"cmd":"ack","id":1603,"result":0,"stream":{"state":"idle","streamId":"ipc_28ac1296ef248ab12c0f746e"}}
```

第二次请求/回复也完整保留，ID 1614，103 ms：

```json
{"cmd":"get","name":"stream","id":1614}
```

```json
{"cmd":"ack","id":1614,"result":0,"stream":{"state":"idle","streamId":"ipc_28ac1296ef248ab12c0f746e"}}
```

**具体差异：**例子第 6.4 节“还没推流”要求 idle 且 streamId 为**空字符串**；实收不是空，也不是缺字段，而是上面的完整旧值。本轮没有发 start/stop/state，不是本轮主控上报把它写进去的。

**责任边界：**字段来自 Wi-Fi 查询应答，需 Wi-Fi 开发核对是否保留历史流状态、何时清空。如果停止后允许 idle 保留最近 streamId，需要明确该状态语义并补充例子；若属于未开始初态，则按例子清空。由于模组上电以来完整直播历史未采集，本项记录为**与例子不一致、历史状态语义待确认**，不捏造内部根因，更不据此断言总线或实际推流故障。

## 3. 已通过项的实际发包/收包

以下都是最终生产服务的 `0x003B` 同次交换，errno/status=0；不是独立探针。

| 查询 | 主控完整发送 | Wi-Fi 完整返回 | 耗时 |
| --- | --- | --- | --- |
| status | `{"cmd":"get","name":"status","id":1601}` | `{"cmd":"ack","id":1601,"result":0,"status":{"cloud":"online","wifi_signal_level":15,"battery_level":0,"motor_status":"normal","http":"ready"}}` | 103 ms |
| 未知 name | `{"cmd":"get","name":"other","id":1604}` | `{"cmd":"ack","id":1604,"result":1}` | 82 ms |

**dps 请求与完整返回，185 ms**，sequence 3/4：

```json
{"cmd":"get","name":"dps","id":1602}
```

```json
{"cmd":"ack","id":1602,"result":0,"dps":{"pir_sens_level":"high","pir_detect_intv":0,"pir_record_dur":10,"squirrel_detect_switch":true,"ai_detect_switch":true,"bird_detect_switch":true,"other_detect_switch":false,"brand_wm_switch":true,"datetime_wm_switch":true,"mic_switch":false,"status_led_switch":true,"image_filter_level":"medium","image_anti_flicker":"60Hz","datetime_wm_format":"MM/dd/yyyy HH:mm:ss","spk_vol":80}}
```

ID、结果、字段与例子一致，dps 与实收 set 值一致。status 的 http=ready 只代表模组报告的状态，不代表主控已经取得凭证。

## 4. 其他例子的本轮边界

| 例子 | 分类及实际收发情况 |
| --- | --- |
| 第 4 节全量 report、第 5 节主动 http | **前置未满足而未完成**：主控全量 set 未成功执行，没有发全量 report；没有对应 report 回复/主动 http 可判断。不是“发送后 Wi-Fi 没回” |
| 第 7 节 file 成功三阶段 | **前置未满足而未完成**：未确认 http，未生成并发送真实文件事件；无实际请求/回复。凭证前 ACK2 门禁也未发包验证，不算通过 |
| 第 3 节 APP 增量 set、第 4 节增量/格式化上报 | **未测**：未收到对应 APP 设置变化，未执行格式化；重复全量 set 不代替增量测试。增量 set 不要求先确认 http |
| 第 1 节空 trans、相同主控 ID 的缓存应答；第 9 节坏 JSON、缺/未知 cmd | **未测**：生产程序没有发送对应报文；没有用另一个测试程序注包补结果，也没有凭空判 Wi-Fi 失败 |
| 第 8 节真实直播 start/stop/state、推流 | **用户此前排除，本轮仍未测**；第 6.4 只读查询不等于直播业务通过 |

## 5. 给 Wi-Fi 开发的重点与证据

1. **优先联合处理最新 ID 1797 的三次同 ID 请求：**主控重试换 ID 已修复，get/http 仍每次订阅 -1、约 15 秒后 ACK2，之后日常路径又取得凭证；同步等待还使 set 确认超时。完整包和时间见 2.2.2，不只反馈“Wi-Fi 阻塞”。
2. **核对 ID 1603/1614 的完整返回：**idle 时保留旧 streamId 是否有意；如有意，需要补充停止后状态语义，否则按例子修正。不是笼统反馈“返回非法”。
3. **全量 set 的 ACK2 是主控执行问题：**本轮没有发现这 15 个下发字段不合规，不要求模组改字段来掩盖主控未执行。

全部证据位于 `实机联调证据261110/`：`wire-current.jsonl`、`exchanges.jsonl` 保留最终 PID 的每次完整请求/应答、ID、通道、长度、耗时、errno/status；`serial-current.jsonl` 保留对应电脑时间和原始串口混行；`selected-exchanges.json` 便于定位，重复 ID 的串口时间数组不冒充一对一对应。`summary.json` 是离线统计，`final-credential-wchan.log`、`final-readback.log` 是现场定位依据。编译、部署、镜像内容核对记录一并保存。

令牌、密钥和 URL 如有实收均原样保留；ACK2 中没有 http 对象，没有可补写的凭证。09:45 用户断电后，验证的是修复前版本正常自动启动，见 2.2.1；最新修复版已编译并持久化 ADB 部署、完成正式服务实测，见 2.2.2，但未再次断电验收。未刷 SDK 分区；自动启动或查询成功不等于全量 set、完整握手或直播业务验收通过。

## 6. 直播入口修复后的真实 APP 联调（11:05—11:26）

### 6.1 部署和验收边界

正式应用增加 RTSP/TCP 发布、实际编码帧读取、start/stop 执行及 stream state 上报入口。只有已经发送视频才允许开播 ACK0，准备中 ACK2，执行失败不伪装成功。摄像机不运行时可以按需启动同一个正式应用的独立直播模式；原开机 AI 门控、抓拍和录像路径未改默认策略。RTMP 尚不支持，明确拒绝；APP 本轮实际请求 RTSP。

- 11:05:24—11:05:30 正常停止协议服务及已经完成开机抓拍录像的旧相机进程，再原子更新应用。新协议服务 PID **450**，应用 MD5 **`06f1eabee74d8d20aa335e0b1abdf177`**；驱动仍为 **`06220a534f736bcaa7363e52ee2c86f5`**，未改过滤表、未重载驱动、未清绑定、未 dd。
- 本轮使用正式程序收发，没有用独立协议探针，没有人工构造 stream/start 发给模组，也没有从串口复制地址绕过 SDIO 开播。相机进程 PID **492** 以正式直播模式启动，仅用于验证真实媒体就绪；**收到真实 stream 后自动启动相机这一整条路径尚未获得实机调用证据**。
- 编译/install 成功；SDK DATA 镜像已重新打包，离线解出应用与安装及编译产物 MD5 一致。镜像 SHA256：`c9b283ed697d673c743cb41f854a7a2c79cdd9fca354541e0ca43a3ded52bd8b`。本轮板端只通过 ADB 更新应用，尚未再次断电验收新版。

| 项目 | 本轮结果及限制 |
| --- | --- |
| 正式主控 SDIO 收发 | **通过通信检查**：PID 450 的 11:05:29.389—11:12:36.255 实收 590 条记录、200 次完整交换，无传输 errno/status 错误。190 条主动包全部为 set，stream 为 0 条 |
| 真实相机媒体 | **通过独立媒体检查**：从板端原 RTSP 服务经 ADB 转发取到 H.264、1920×1080、51 帧；不是 APP 直播通过 |
| 新 RTSP 发布器 | **本机互通验证通过，非板端云推流验收**：同一发布器源码通过严格编译和 ASan/UBSan 运行；FFmpeg 收到 H.264 270 帧、H.265 30 帧，ANNOUNCE/SETUP/RECORD 成功，H.264 轮还验证了 OPTIONS 保活 |
| 例子第 3 节全量 set | **设置成功闭环仍未完成，主控原因**：4 项实际应用；其余 11 项本地 ENOSYS=38。防频闪在相机启动后已实际返回 errno=0，不再是本轮剩余项 |
| 例子第 8 节 APP 真实 start/stop、推流及 state | **前置未满足而未完成**：模组串口有真实 IpcStreamStart/IpcStreamStop，但主控 0x083B 没有收到 stream；没有相应 stream ACK 或真实云推流结果，不能算通过，也不能把未调用的发布后端判成执行失败 |

### 6.2 APP 开播时实际收发了什么

11:07:38.7970083、11:07:39.0929026，COM9 均记录 `主控会话：云端推流 start，streamId=ipc_28ac1296ef248ab12c0f746e`。随后 11:08:49.4466142 又记录一次。**这是云端 → Wi-Fi 的证据，不是 Wi-Fi → 主控的 stream 发包证据。**

同一时段主控实际收到的是以下全量 set，**Wi-Fi → 主控，0x083B，ID 87，408 字节**，主控 sequence 230、11:07:38.221：

```json
{"cmd":"set","id":87,"dps":{"pir_sens_level":"high","pir_detect_intv":0,"pir_record_dur":10,"squirrel_detect_switch":true,"ai_detect_switch":true,"bird_detect_switch":true,"other_detect_switch":false,"brand_wm_switch":true,"datetime_wm_switch":true,"mic_switch":false,"status_led_switch":true,"image_filter_level":"medium","image_anti_flicker":"60Hz","datetime_wm_format":"MM/dd/yyyy HH:mm:ss","spk_vol":80}}
```

**主控 → Wi-Fi，0x003B，同 ID，32 字节**，sequence 231、11:07:39.129：

```json
{"cmd":"ack","id":87,"result":2}
```

**Wi-Fi 同次返回：空字符串 `""`，length=0，errno=0、status=0、耗时 41 ms**，sequence 232。不是未收到回复。随后相同 set/87 再来，主控仍回 ACK2，Wi-Fi 回空串、42 ms；COM9 11:07:43.0047582 记录 `主控会话：应答失败，重新从全量 set 开始`，11:07:43.2053512 发全量 set/88。

主控本地的未执行字段为：`pir_sens_level`、`pir_detect_intv`、`pir_record_dur`、`ai_detect_switch`、`bird_detect_switch`、`squirrel_detect_switch`、`other_detect_switch`、`image_filter_level`、`brand_wm_switch`、`datetime_wm_switch`、`datetime_wm_format`，全部 errno=38。实际成功的是 `image_anti_flicker=60Hz`、`mic_switch=false`、`spk_vol=80`、`status_led_switch=true`。**这些 errno 是主控执行结果，不是 Wi-Fi 发来的字段错误。**

为保留地址和令牌，下面是 COM9 **11:08:49.4456290 的完整实收日志行**。末尾本来就是 `rof159}}`，不能补成合法 JSON；这条串口混行也不能当作主控收到了非法 stream 包：

```text
蓝牙添加：下行明文，{"protocol":"5","bizCode":"IpcStreamStart","commandId":"1d7a74c21bfbb5880b99826b1b102076","params":{"streamId":"ipc_28ac1296ef248ab12c0f746e","app":"live","pushProtocol":"RTSP","pushUrl":"rtsp://120.24.118.1:554/live/ipc_28ac1296ef248ab12c0f746e?token=eyJwdXJwb3NlIjoiUFVTSCIsInRlbmFudENvZGUiOiJURU5BTlRfMTc3NjE2MDQ0NDY5NDY0Qjk0MyIsInVzZXJDb2RlIjoiVTE3OTM5NDgwNDUyNTUwMDJGQkYiLCJwcm9kdWN0Q29kZSI6IlBfU0JGX0Y1Z1Q4Yk43IiwiZGV2aWNlU24iOiJEXzFBMThCQjI4QjkxZkRxNWVtczZxcHp5Iiwic3RyZWFtSWQiOiJpcGNfMjhhYzEyOTZlZjI0OGFiMTJjMGY3NDZlIiwiZXhwaXJlVGltZSI6MTc5NDg4NDkyOTE1OX0.1d64bbe222945159856afc5d3a37e79e790a4cf5191237092168b3a99b0952d7&expireTime=1794884929159","channel":"ch1","rof159}}
```

### 6.3 当前责任与下一步

**需要更正此前“直播可以直接独立往下测”的判断：HTTP 凭证本身不是直播地址的前置，但当前固件的全量 set 握手还未完成，不能忽略这一层。**本轮 `get/http` 请求数为 **0**，`http_attempts=0`，不存在用 get/http 同步等待来解释本轮未开播的依据。

1. **已确定的主控问题：**11 个合法设置未接真实执行，全量 set 始终 ACK2。应补真实执行/读回，成功后才 ACK0，再走 report；不能只保存 JSON 或删除字段假装执行成功。
2. **Wi-Fi 侧需联合核查，但尚不能单判固件错误：**云端开播已到，后续串口“下发”及主控实收仍只有 set。请按上述 commandId、set/87 和 set/88 检查 stream 是否排队等待全量同步、是否在重新同步时被清除，并记录实际 0x083B stream 发送结果。目前无模组应用源码，不能把其内部队列行为说成已经定位到某一行，也不能说主控已经完全排除。
3. **当前不能承诺 APP 已能看图：**发布代码已接，但真实 start 没有到达该入口；ACK0、云端收帧、APP 出图和 stop 闭环均缺实机证据。若两端决定先独立联调直播，需要 Wi-Fi 明确支持在设置忙时保留并下发 stream，而不是主控偷偷绕过握手。

11:16—11:26 ADB 再次枚举不到设备；同一时段 COM9 仍持续收到主控 ACK2。只记录访问现象，不据此判断主控崩溃，不把 ADB 变化当成本轮开播失败的根因。串口采集 11:26 正常结束、进程退出码 0，COM9 已释放；主控完整收发统计截止于最后一次成功拉取的 11:12:36.255，不用其后的串口记录冒充已取得主控 wire。

本轮证据单独保存在 `实机联调证据261110直播/`：`wire-current.jsonl`、`exchanges.jsonl`、`summary.json`、`selected-exchanges.json` 保存完整收发/ID/通道/耗时/状态；`serial-before.jsonl`、`serial-live.jsonl` 保存带电脑时间的串口原文；`deploy.log`、`closing-state.log`、`live-blocker-state.log`、`live-camera.log` 保存版本、驱动、网络、消费者及实际设置错误。媒体与发布器验证另有 probe/log，不混算 APP 验收。用户删除的旧证据目录没有恢复，本节不依赖它；原有暂存区未改变。

## 7. 按用户指示临时确认未实现设置（11:35 起）

### 7.1 修改范围与结果

- **仅对合法、未实现的 set 字段临时回 ACK0**；相机未运行时的防闪烁设置也先接收，不启动/修改 AI 门控或抓拍策略。未执行字段留在 `host_deferred_fields`，不放入 `host_applied_fields` 冒充已执行。JSON/类型/取值校验不放宽，已有后端的真实执行错误仍回 ACK2，格式化请求不假报成功，http 和 stream 不适用此临时规则。
- 首版应用 MD5 `ee919af2cf509a47e23b8f1c72ef872b`，11:35:56 上板，服务 PID 422。此版完成下列 set/report，并复现一次模组断言。11:40:48 COM9 又观察到启动日志，主控 uptime/服务 PID 也重新开始；**本轮没有执行重启、驱动重载或 SDIO 重新枚举命令，重启来源不由本轮命令日志推断**。重启后同版服务 PID 314 完成 set/report/http 闭环。
- 最终应用 MD5 **`98d11a6d4dc3b654bf7ce153842294fa`**，11:41:58—11:42:01 再次 ADB 部署，服务 PID 379。相对首版只增加驱动超时/终止后的 **3 秒重试退避**，防止故障时每约 50 ms 连续发命令；不改变已测试的 set 确认逻辑。此最终进程重新查询 status/dps 成功；它启动后尚未重新收到全量 set，不能把前一进程的握手计作它自己的冷启动验收。退避分支经代码检查，本轮最终版未再出现同样驱动故障，未做故障注入验收。
- 驱动 MD5 `06220a534f736bcaa7363e52ee2c86f5`、管理脚本 MD5 `b53093af1f798885297ddc0c88220273` 均未变；最终应用与 SDK data 镜像离线解出的应用一致。只通过 ADB 更新应用，没有 dd 覆盖 SDK 分区。暂存区未变。

| 联调例子 | 本轮结论 | 具体依据 |
| --- | --- | --- |
| 第 3 节全量 set 确认 | **通信通过；功能未验收** | set/101 → ACK0 → 空串，147 ms；重启后 set/1、3、5、7 也均 ACK0，不能据此认定那些设置已执行 |
| 第 4 节全量 report | **通信通过** | report/2115 → 同 ID ACK0，204 ms；重启后 report/2179—2182 均同 ID ACK0 |
| 第 5 节主动 http 下发及确认 | **先失败一次，重启后闭环通过** | http/102 内容完整，但其 ACK0 后模组断言、同次确认未收到；重启后 http/8 → ACK0 → 空串，44 ms，详见下文 |
| 第 6.1、6.2 节 status/dps | **通信通过** | 最终版 get/status/2243 → ACK0，104 ms；get/dps/2244 → ACK0，186 ms，完整报文见证据 |
| 第 6.3 节主动 get/http 换凭证 | **本轮未测** | 本轮没有发这个请求，不能以主动 http 下发替代此用例，也不能归因于 get/http 阻塞 |
| 增量 set、file、stream 开停与 APP 画面 | **未完成，未计通过** | 本轮现有取证尚无对应完整业务闭环；不编造命令补测。没有执行格式化、解绑或恢复出厂 |

### 7.2 全量 set → report 的真实报文

Wi-Fi → 主控，`0x083B`，sequence 5，主控时间 11:35:57.064，409 字节：

```json
{"cmd":"set","id":101,"dps":{"pir_sens_level":"high","pir_detect_intv":0,"pir_record_dur":10,"squirrel_detect_switch":true,"ai_detect_switch":true,"bird_detect_switch":true,"other_detect_switch":false,"brand_wm_switch":true,"datetime_wm_switch":true,"mic_switch":false,"status_led_switch":true,"image_filter_level":"medium","image_anti_flicker":"60Hz","datetime_wm_format":"MM/dd/yyyy HH:mm:ss","spk_vol":80}}
```

主控 → Wi-Fi，`0x003B`，sequence 6，11:35:57.066，33 字节：

```json
{"cmd":"ack","id":101,"result":0}
```

同次返回：**空字符串 `""`，length=0、errno=0、status=0、147 ms**，不是未收到。随后同一生产服务自动发全量 report，sequence 8，11:35:57.265：

```json
{"cmd":"report","properties":{"memory_capacity":{"total":0,"used":0},"memory_status":"unmount","memory_format_status":"success","firmware_version":"1.0.0"},"id":2115}
```

Wi-Fi 同次返回，sequence 9，204 ms、errno/status=0：

```json
{"cmd":"ack","id":2115,"result":0}
```

### 7.3 http/102 后的一次断言：完整报文与边界

Wi-Fi → 主控，`0x083B`，sequence 10，11:35:57.519，383 字节，原样保留全部令牌/密钥：

```json
{"cmd":"http","id":102,"baseUrl":"http://47.107.170.93:18080","productCode":"P_SBF_F5gT8bN7","deviceSn":"D_1A18BB28B91fDq5ems6qpzy","version":"1.0.0","accessToken":"361afd0d7ae4485e8908396e496a5c0d7ffc3ea8d01343c68ee62eebe3bfeeb6","httpHmacSignKey":"U1JjbUluN2Vsa0FEVVdubHViazNaUjMwa3RGQ1VpSHY=","httpAesKey":"bWU2WVgzc3A1cWlXb1h1ckJlR3oxT2lnT2NNWlZGZHE=","expireTime":1796873398078}
```

主控解析通过，`0x003B`，sequence 11，同一主控毫秒发送：

```json
{"cmd":"ack","id":102,"result":0}
```

**这次应返回的空字符串没有收到**；sequence 12 为 ioctl 失败，30773 ms、errno=1、status=0，无可用协议回复。不能把这个 status=0 当作交换成功，也不能说 Wi-Fi 返回了空串。COM9 电脑时间原文：

```text
11:35:57.9832895 主控会话：收到应答 id=102，result=0
11:35:57.9842517 <ASSERT>:HiMsg_RequestIndBuf 341,IPC=1528da
11:35:57.9842517 Assert HiMsg_RequestIndBuf ErrCode 8 R2 155,R3 0
11:35:57.9852691 IPC = 0x12a26c
11:35:57.9852691 mepc = 0x185d1a mstatus = 0x1888 mcause = 0x8000000b
```

随后主控驱动返回 errno=3。驱动源码 `wsm_cmd_send()` 在底层线程终止时直接 `return -3`；此处不能仅按 `strerror(3)` 的 “No such process” 理解成应用进程不存在。首版故障处理发生密集重试，**但第一条模组断言早于任何这类重试**，不能倒过来将它解释成密集重试造成。该主控重试问题已在最终版增加退避；首版服务在取证后停止，最终版之后重新启动，没有热重载驱动。

**责任边界：**实际确认的命令、ID、result 与例子第 5 节一致，且串口证明 Wi-Fi 已解析到它；没有空令牌、缺 expireTime 或 JSON 解析失败。断言位置在 **Wi-Fi 固件的 `HiMsg_RequestIndBuf`**。但当前没有模组源码/资源分配日志，不能断言其内部哪一个缓存损坏，也不能仅凭应用日志彻底排除底层时序。请 Wi-Fi 工程师检查 **http/102 的 ACK0 处理分支至该断言的调用链、指示缓存申请/释放状态及同次 Direct Buffer 确认是否发送**；以上报文是具体触发场景，不是 get/http 查询。必须与下一节重启后成功的同类交换对照，不写成每次 ACK0 都会崩溃。

### 7.4 重启后的实际恢复，不把历史故障写成当前永久阻塞

COM9 11:40:48.7310220 观察到启动日志。重启后服务 PID 314 已先确认全量 set 并上报 report；主控本地时钟仍未同步时，http/2、4、6 回 ACK2，Wi-Fi 同次均回空串并重新全量同步。这是主控时钟前置未满足，不是凭证字段错误。此时主控 realtime 仍为开机秒数，**不拿它与电脑时间相减计算延迟**。

时钟恢复后，Wi-Fi → 主控，`0x083B`，PID 314、sequence 39；COM9 11:41:49.0321687 记录 http/8：

```json
{"cmd":"http","id":8,"baseUrl":"http://47.107.170.93:18080","productCode":"P_SBF_F5gT8bN7","deviceSn":"D_1A18BB28B91fDq5ems6qpzy","version":"1.0.0","accessToken":"c0d6a8c9d249491f8a89dc68a5ff74afc91f1d2428084b67a02f4abba737b256","httpHmacSignKey":"c1puWExjeVBWeDJvVGU1WUF3R2ZLZ21KT0MwaFdCMlM=","httpAesKey":"d0hUcnRCTkUxNWQ3b3Z1dVZ4MVBwY0J2ZnZxaW56RDI=","expireTime":1796874082030}
```

主控 → Wi-Fi，`0x003B`，sequence 40：

```json
{"cmd":"ack","id":8,"result":0}
```

**Wi-Fi 同次返回空字符串 `""`，length=0、errno=0、status=0、44 ms**，sequence 41；COM9 11:41:49.1715679 记录收到该 ACK0。此次确认闭环通过。它与前面的失败不是同一次交换，不互相覆盖。

本轮证据保存在 `实机联调证据261110直播/临时ACK0/`：`wire-final.jsonl` 保存首版失败轮，`wire-backoff.jsonl` 保存观察到重启后的 PID 314 与最终 PID 379，`serial.jsonl` 保存带电脑时间的完整串口原文；部署、版本、主控状态、编译、主机端边界检查和镜像校验另存日志。主机端检查不是板端独立测试程序，不计入真实通信验收。

**11:45:33 收尾状态：**最终 PID 379 仍运行，status 查询累计 4 次、dps 查询 1 次均成功，无传输错误，`last_error=0`；网络地址 `192.168.10.147/24`，网关 `192.168.10.1`，TCP 过滤 flags=7、源/目的端口均为 18883，没有第二个协议消费者。最终进程的 `peer_messages=0`、`http_usable=false`：此前 PID 314 的有效凭证没有跨应用进程重启保存在内存中，**不能说最终进程当前已经持有凭证**；下次正常链路全量同步/凭证重新下发还需确认。此状态不等于 Wi-Fi 没联网，也不直接阻止自带地址的 stream 开播。

COM9 采集窗口为电脑时间 **11:34:37—11:44:37**，自然结束、退出码 0，串口已关闭释放；11:42 起的串口窗口没有新的云端开播/stream 记录，主控截至收尾的 wire 也没有 stream。因此本轮只能确认上述通信闭环，不能确认 APP 出图、stream 状态和 stop 闭环。最终 data 镜像 SHA256 为 `60f1808b15c654c3bdcdb14ada0998e0baa7f764fec0bfb50886f58a607edef5`，未刷入 SDK 分区。

## 8. 用户更新 Wi-Fi 固件后的 get/http 复测（11:52—11:56）

### 8.1 实际结论

**当前取凭证通信通过，旧版“get/http 等约 15 秒后 ACK2”的现象没有复现；真正向云端换新凭证的订阅分支仍未验证，不标为通过，也不标为仍然失败。**

本轮不改主控代码、不重编/覆盖相同应用、不重启或热重载驱动，仅通过现有正式服务排队执行。应用 MD5 `98d11a6d4dc3b654bf7ce153842294fa`、驱动 MD5 `06220a534f736bcaa7363e52ee2c86f5`、管理脚本 MD5 `b53093af1f798885297ddc0c88220273` 与上一轮一致；协议服务始终为 PID **336**，正常相机进程 PID **156**，网络 watcher PID **329**，没有第二个协议消费者。wlan0 `192.168.10.147/24`，网关 `192.168.10.1`，TCP 过滤 flags=7、源/目的端口 18883，前后未变。未改变 AI、抓拍或临时 ACK0 策略。

用户表示已经更新模组固件；本轮串口采集晚于模组启动，**未取得新固件独立构建号/镜像散列**，不把主控应用版本当成 Wi-Fi 固件版本。以下结论针对当前实际运行模组的真实回复。

COM9 电脑取证窗口 **11:52:39.876—11:56:31.659（UTC+8）**，被动读取、无串口写命令，正常关闭、退出码 0。主控本轮统计限定为同一 PID、sequence **28—57**：**30 条记录、15 次完整交换、0 错 ID、0 传输错误、0 未配对请求**。sequence 1—27 是测试前已存在的本次开机日志，单独保留，不冒充本轮带串口同步取证的结果。

| 例子项目 | 本轮判定 | 实际收发结果 |
| --- | --- | --- |
| 6.1 get/status | **通过** | 7 次均返回同 ID ACK0，cloud=online、http=ready，104—105 ms |
| 6.2 get/dps | **通过** | 2 次返回同 ID ACK0 和全量 dps，187 ms；只证明通信，不证明所有设置已真实执行 |
| 6.3 get/http：取得有效凭证 | **通过** | 2317、2319、2322 三次均同 ID ACK0，http 对象完整，145/146/146 ms，无 ACK2 |
| 6.3：过期/401 后真正换新、云端重新订阅 | **前置未触发，未完成** | 三次均为未过期缓存；没有调用云端刷新路径的证据，不能确认该分支修复 |
| 6.4 get/stream 空闲状态 | **通过** | 2315、2321 均 ACK0，state=idle、streamId=""，85 ms；不是旧 streamId |
| 6.5 未知 name | **通过** | other/2316 → 同 ID ACK1，85 ms，符合正常拒绝示例 |
| set/report/http 主动下发、增量 set、file、stream 开停 | **前置未触发，未完成** | 取证窗口无新的 0x083B 指令或真实文件事件；查询空闲不代替开停/APP 画面验收，不注入模拟命令 |

### 8.2 三次 get/http 的实际请求与完整回复

均走主控 → Wi-Fi **0x003B**，分别 sequence 38、42、48：

```json
{"cmd":"get","name":"http","id":2317}
{"cmd":"get","name":"http","id":2319}
{"cmd":"get","name":"http","id":2322}
```

第一次 Wi-Fi 同次返回，sequence 39，**403 字节、145 ms、errno=0、status=0**，原样保留全部令牌/密钥：

```json
{"cmd":"ack","id":2317,"result":0,"http":{"baseUrl":"http://47.107.170.93:18080","productCode":"P_SBF_F5gT8bN7","deviceSn":"D_1A18BB28B91fDq5ems6qpzy","version":"1.0.0","accessToken":"053888d77574462db99c9e87890b3b9542c592a40e534519922566360d3a60c2","httpHmacSignKey":"eFlqMTJUWGJZUXltN2p6SGQybXc0UkpnejV6MmtuUHE=","httpAesKey":"TmhRWWlpcjZxRWl1RXZwY3NhaG5PRTdaYjFPRjlsV0c=","expireTime":1796874628572}}
```

后两次回复分别为 ID 2319/2322、sequence 43/49，**403 字节、146 ms、errno/status=0**，http 内容与第一次完全相同；各自完整原文保存在证据 `exchanges.jsonl` 和 `summary.json`，没有省略或脱敏保存。三次均不是空字符串，不缺 http/expireTime。两把 Base64 密钥各解码为 **32 字节**，version=1.0.0 与主控一致，baseUrl 无尾斜杠，expireTime 为 13 位未来毫秒值，没有 refreshToken；主控实际接受，`http_refresh=received_valid_credentials`、`http_usable=true`。

### 8.3 为什么还不能断言“云端订阅失败的根因已经修复”

三次请求在 COM9 的实际分支日志为：

```text
11:52:58.9909651 主控会话：HTTP 凭证未过期
11:54:02.3296758 主控会话：HTTP 凭证未过期
11:54:50.3830667 主控会话：HTTP 凭证未过期
```

它们返回的 token、两把密钥和 expireTime 也与测试前主动下发的缓存值一致。**这证明目前的 get/http 可以正常取得有效缓存凭证；不是返回空令牌、ACK2 或 JSON 解析失败。**但取证窗口内没有任何订阅日志，没有触发旧故障所在的云端重新订阅路径。

例子第 6.3 节写的是 Wi-Fi 向云端再要一次凭证；本版在未过期时实际直接返回缓存，需与 Wi-Fi 工程师确认这是本次固件的预期策略。不能仅凭 token 没变化判数据非法，也不能把缓存查询成功等同于已经验收“强制换新”。若要单独验收旧订阅故障修复，需要在**真实过期/临近过期、401，或模组提供的正式强制刷新入口**触发真实云端请求，并同步取得订阅和回复日志；本轮不改主控时钟、不伪造凭证/401、不使用未定义字段强行制造这个条件。

本轮没有需要据此修改的主控通信错误。收尾 `last_error=0`、凭证可用、stream=idle、frames=0，无直播待处理命令；不能宣称 APP 直播已经通过。完整证据统一保存在 `实机联调证据261110直播/get-http新固件复测/`，包括原始前后 wire、仅本轮 wire、逐次配对报文、字段校验结果、COM9 原文、版本/网络/消费者状态及暂存区散列。

## 9. 真实直播前置检查（13:31 起，尚未通知开播）

**本次直播测试尚未开始，前置未满足，不记为 stream 指令失败。**13:31 初查 ADB 未枚举到设备；13:32:55 恢复枚举 `0123456789`。恢复后的主控处于刚启动状态，未执行 ADB 重启、驱动重载、烧录或修改配置。应用/驱动/管理脚本 MD5 与第 8 节一致。

- COM9 已于电脑时间 13:31:49 开始被动采集。13:33:47.6500351 实收 `蓝牙添加：Wi-Fi 连接超时，ssid=HOWNICE`，13:33:47.6530350 实收 `蓝牙添加：已绑定但 Wi-Fi 失败`；此前反复扫描，当前窗口没有 `WIFI_CONNECT_GOT_IP` 或 MQTT 已连接记录。不能仅凭此判断路由器、射频或固件的具体根因。
- 主控按既有开机流程自动发起初次 SDIO 初始化：内核 uptime 46.014097 秒已枚举 `mmc1: new high speed SDIO card at address 0001`，随后 `WSM startup poll: ctrl=0x3000`，uptime 59.249965 秒记录 `wsm_startup_done timeout ERROR`，59.334885 秒记录 `probe of mmc1:0001:1 failed with error -110`。这是已枚举卡之后的驱动/固件启动握手超时，不能笼统说 SDIO 引脚没通，也不能未经排查单判某一端缺陷。
- 当前正式服务 PID 361、相机 PID 156，无第二个协议消费者；`sdio_open=false`、`peer_messages=0`、`clock_valid=false`，不存在 wlan0 或默认路由。当前没有主控→Wi-Fi 的业务发包或 Wi-Fi→主控的 JSON 收包可供判定，`wire.jsonl` 在初次检查时尚未建立；这不是对某条 stream 返回空串/ACK1/ACK2。

下一步先确认 HOWNICE 路由器/热点可用，并恢复整机正常启动与 Wi-Fi/SDIO 前置，再通知用户进入 APP 直播页。没有擅自热重载或修改 AI/抓拍策略尝试绕过。当前主控时钟仍为 1970 年，取证时序以 COM9 电脑时间和主控 monotonic/uptime 分别记录，不跨时钟计算延迟。

证据：`实机联调证据261110直播/真实直播1331/` 保存主控检查、驱动原始日志、ADB/Windows USB 枚举及串口快照。串口快照截止 13:33:47.6530350；采集进程仍运行，后续日志不能冒充已经包含在这份快照中。

## 10. APP 真实开播：Wi-Fi 断言，主控未收到 stream（13:38—13:48）

### 10.1 本轮结论与环境

本轮只验收例子第 8 节的真实云端开播及其前置，不冒充重跑全部用例。13:38 预检时 ADB、COM9、wlan0/默认路由正常，主控已经确认凭证，`http_usable=true`；相机启动抓拍/录像完成，未改 AI 或抓拍策略。使用板端正式应用 MD5 `98d11a6d4dc3b654bf7ce153842294fa`，驱动 MD5 `06220a534f736bcaa7363e52ee2c86f5`，没有重新编译、推送或热重载驱动，也没有清绑定。只有一个生产协议服务及原网络 watcher；电脑监视仅读取日志和生产服务的本地状态，不是第二个 SDIO 消费者。

| 项目 | 本轮判定 | 实际结果 |
| --- | --- | --- |
| 第 6.1 节 get/status，开播前 | 通过 | ID 2443 同次 ACK0，105 ms，cloud=online、http=ready，见下文原包 |
| 第 8 节：真实 start 从 Wi-Fi 下发主控 | 失败 | 云端 start 已到模组；模组断言，两个主控启动轮次的正式 wire 均无任何 stream，不能提供不存在的 SDIO stream ID |
| 第 8 节：主控实际推流、ACK0、pushing 状态闭环 | 前置未满足而未完成 | 没有收到 start，`generation=0, frames=0, state=idle`，未进入 RTSP 发布；不是推流后失败，也没有假回开播 ACK0 |
| 第 8 节：真实 stop、断流 fail 上报及查询一致性 | 前置未满足而未完成 | UART 后续收到云端 start/stop，但 SDIO 已失效，没有主控收包；未产生真实推流可供停止/断流验收 |
| 第 5 节 http 主动下发的 ACK0 空回复，重启复核 | 失败 | 主控收到完整有效 http 6 并 ACK0，模组同一时刻断言，未收到预期空回复；30,878 ms 后 ioctl 失败，errno=1 |

未实现设置仍沿用用户此前指定的临时 ACK0 策略，本轮不将其算成硬件功能已实现。两次主控启动分别为 PID 336、boot ID `04a166a9-b4ae-4cc8-823f-ae09f46f8698` 和 PID 335、boot ID `bfa1d46b-02fd-42b3-9753-f76ee34a8d92`；不能按相同 sequence/ID 跨重启合并。以下 UART 时间均为电脑 UTC+8；交换耗时使用主控 monotonic，不跨两端时钟相减。

### 10.2 APP 开播时，主控实际做了什么

故障前最后一组主控业务报文是正常的定时状态查询（PID 336，sequence 36—37）：

主控 → Wi-Fi，`0x003B`，39 字节；COM9 电脑时间 `13:39:08.8315053`：

```json
{"cmd":"get","name":"status","id":2443}
```

Wi-Fi → 主控，同次返回，142 字节，**105 ms，errno=0/status=0**；COM9 `13:39:08.9405281`：

```json
{"cmd":"ack","id":2443,"result":0,"status":{"cloud":"online","wifi_signal_level":11,"battery_level":0,"motor_status":"normal","http":"ready"}}
```

随后 `13:39:38.6265948`，模组 UART 记录云端 `IpcStreamStart` 下行，宣告明文长度 716；`13:39:39.1111087` 实际捕获的下一行如下，**原样保留令牌，不补字节**：

```text
{"protocol":"5","bizCode":"IpcStreamStart","commandId":"936c2fdf6e0a7a79aa4923f440907463","params":{"streamId":"ipc_28ac1296ef248ab12c0f746e","app":"live","pushProtocol":"RTSP","pushUrl":"rtsp://120.24.118.1:554/live/ipc_28ac1296ef248ab12c0f746e?token=eyJwdXJwb3NlIjoiUFVTSCIsInRlbmFudENvZGUiOiJURU5BTlRfMTc3NjE2MDQ0NDY5NDY0Qjk0MyIsInVzZXJDb2RlIjoiVTE3OTM5NDgwNDUyNTUwMDJGQkYiLCJwcm9kdWN0Q29kZSI6IlBfU0JGX0Y1Z1Q4Yk43IiwiZGV2aWNlU24iOiJEXzFBMThCQjI4QjkxZkRxNWVtczZxcHp5Iiwic3RyZWFtSWQiOiJpcGNfMjhhYzEyOTZlZjI0OGFiTJjMGY3NDOH0.6b3385c0dbae12574614a176c3e07fb780ffd40200e0fa96f2d358fc81d41ffd&expireTime=1794893978688","channel":"ch1","profile":"main","expireTime":1794893978688}}
```

注意：该行实际只有 **677 UTF-8 字节**，与 UART 宣告的 716 不同；后续其他 start 还有混行/未闭合字符串。它是串口实收内容，不等于完整云端原包，不能据此判 token 或 URL 非法，更不能把不同片段拼成完整报文。

紧接着模组自身输出：

```text
13:39:39.1120925 <ASSERT>:HiMsg_RequestIndBuf 341,IPC=1528e4
13:39:39.1130587 Assert HiMsg_RequestIndBuf ErrCode 8 R2 155,R3 0
13:39:39.1130587 IPC = 0x12a26c
13:39:39.1140589 mepc = 0x13a6f0 mstatus = 0x1888 mcause = 0x8000000b
13:40:05.8740670 altobeam 6461 wifi boot(4M   ver v2.3 [COMPRESS-OTA IVB EFUSE FW_CHKSUM:1 NO_DELAY:0]) (no-enc CACHE_ITCM_64K)check rom
```

**两端收发的具体缺口：**正式服务在故障前后都没有 `wifi_to_host_0x083B` 的 `cmd=stream` 记录，所以主控没有 stream 请求 ID，也没有对应 ACK0/ACK1/ACK2；这是**未收到**，不是收到空字符串、缺少 expireTime 或 JSON 解析失败。主控 `live.generation=0/frames=0`，没有发起 RTSP ANNOUNCE/SETUP/RECORD，也没有运行视频上传造成网络负载。本轮没有新发 `get/http`；保留的 watcher 仍按原逻辑读取网络状态，不能据此声称主控/驱动完全零活动。

模组重启后，主控下一次定时发送 `{"cmd":"get","name":"status","id":2444}`（sequence 38）；此次**没有收到业务回复**，1,630 ms 后是 `transport_error_0x003B, errno=3, status=0`（sequence 39），后续重试 8 ms 即失败。这里的 status=0 是本地结构初值，不是模组 ACK0。代码 `wsm_cmd_send()` 在 `atbm_bh_is_term()` 时直接返回 `-3`，用户态显示为 errno 3；不是生产程序不存在。驱动同时记录 SDIO 控制寄存器读取 `-110`。模组随后恢复联网/MQTT 订阅不代表旧主控 SDIO 会话自动恢复。

### 10.3 历史故障会话重启主控：确认 http 6 时再次断言

保全首轮全部证据后，于电脑 `13:44:27.317` 执行一次 `adb reboot`，没有写串口命令或重载驱动。**这是已故障会话后的主控重启，不是主控和模组同时断电的干净对照，不能据此单独证明某条 ACK 导致断言。**主控按正常启动流程重新枚举 SDIO、接收全量 set、发送 report。启动前几秒主控时钟尚未校准，http 暂回 ACK2；同 ID 重发后时钟有效，主控正常改回 ACK0。下列是第二轮 **PID 335** 的完整实际报文，不采用 UART 中模组自行输出的 `***` 代替 wire 原包：

Wi-Fi → 主控，`0x083B`，sequence 31，**381 字节**：

```json
{"cmd":"http","id":6,"baseUrl":"http://47.107.170.93:18080","productCode":"P_SBF_F5gT8bN7","deviceSn":"D_1A18BB28B91fDq5ems6qpzy","version":"1.0.0","accessToken":"1a4c6369809c4058921b915969a37bc3dcae5517472b4e46b8c69d1fe3ce80a9","httpHmacSignKey":"aUJyTk9vUExkdXhIMlh5VkdUekVubEdPZFVkWnEyek8=","httpAesKey":"ekVOTWNKVFBQRFZGVmNsZFluMVA4YVhpU3FPRXJhcno=","expireTime":1796881229234}
```

主控 → Wi-Fi，`0x003B`，sequence 32，**31 字节，monotonic=60,509 ms**：

```json
{"cmd":"ack","id":6,"result":0}
```

Wi-Fi 实际行为：电脑 `13:45:30.1855976` 先打印 `主控会话：收到应答 id=6，result=0`，同一电脑时间打印 `HiMsg_RequestIndBuf 341,IPC=1528e4` 断言，随后 `13:45:58.1983733` 又出现模组启动标识。主控这次交换实际结束记录（sequence 33）：

```json
{"event":"transport_error_0x003B","errno":1,"status":0,"elapsed_ms":30878}
```

**与例子的具体差异：**ACK 同 ID、result=0 符合协议；例子要求同次返回空字符串，但本次**未收到回复**，不是实收 `""`。http 原包字段齐全、expireTime 是 13 位未来毫秒，两把 Base64 密钥均可解出 32 字节；没有复现 expireTime 空值，也不能归因为主控拒收凭证。主控只在空回复确认成功后才提交凭证，因此最终 `http_usable=false`，不是没有收到原始 http 数据。此次主控没有开始推流，断言与收到凭证 ACK0 共现；由于并非双方干净启动，不能称 ACK0 单独必现或认定底层会话无关。第 11 节的新启动轮中，http 8 的 ACK0 实际成功。

### 10.4 责任边界与给 Wi-Fi 工程师的下一步

- **已确定的故障位置：Wi-Fi 固件内部 `HiMsg_RequestIndBuf` 断言及随后重启。**第一轮在真实云端 start 到达后、主控尚无 stream 收包时出现；第二轮在有效 http 的标准 ACK0 后出现。不是仅凭 TLS 错误或网络不可达就归因 Wi-Fi。
- **尚未确定的内部根因：**没有模组源码/符号和收发 credit/缓冲统计，不能把 ErrCode 8 直接解释成“缓冲耗尽”“越界”或证明与主控 SDIO 底层行为完全无关。本轮核对主控 1,600 字节接收上限、异步收包优先轮询、标准 ACK 内容与唯一协议消费者，未发现能据此修改的主控业务错误；不盲改现有直播或禁用必要握手掩盖断言。
- **Wi-Fi 侧需检查：**按上述两种已记录现场，解析 `HiMsg_RequestIndBuf:341`、`IPC=0x1528e4/0x12a26c` 及调用栈，增加 indication 分配/释放、空闲数、SDIO credit、消息长度/ID 的诊断。确认转发 start 的 `0x083B` 是否真正提交，及确认 http 后是否正确返回 `0x043B` 空内容；若暂时无缓冲，应排队/重试而不是断言。先核对这些现场，再验真实推流。
- **主控侧当前限制：**模组自行重启后，驱动 BH 已终止，原会话没有自动重建；`sdio_open=true` 或偶尔 `last_error=0` 只表示文件仍打开/本地循环状态，不能当通信恢复。是否需要协同的复位通知与 SDIO 重枚举恢复，需要双方约定，不用未经验证的热重载绕过。独立 RTSP 发布路径仍未得到本轮真实 start，不能宣布主控直播后端已经通过或已经失败。

本轮没有新增代码修复、刷写或暂存操作；暂存区 SHA256 保持 `8b4da76ab28b12c557883a5094db830cf532371f48f78d218d8b0796643b5866`。ADB 重连后已恢复可用，证据已重新拉取，当前限制不是“拿不到 ADB”。COM9 被动取证在 `13:48:37.3423618` 正常结束，无采集错误。完整原始 wire、逐次请求/回复或明确未回复、COM9 电脑时间、版本/进程/网络及原始内核日志统一放在 `实机联调证据261110直播/真实直播1337/`；`收发配对.jsonl` 不会将 transport_error 伪造成空字符串回复。

## 11. 逐步操作复核：这次主控做了什么（14:12—14:33）

### 11.1 测试范围与操作

依据例子第 5、6、8 节，用已烧录的正式程序；没有另发模拟 start，没有重新编译、推送、板端重启、串口写命令、热重载或清绑定。应用 MD5 `98d11a6d4dc3b654bf7ce153842294fa`，驱动 MD5 `06220a534f736bcaa7363e52ee2c86f5`；主控 boot ID `704518c7-5c19-4470-9d05-2a70e4896806`，协议服务 PID 338，相机 PID 156，原网络 watcher PID 331。预检 wlan0 为 `192.168.10.147/24`，路由为 `192.168.10.1`，只有一个 JSON 协议消费者。

电脑 COM9 只读取证两段：`14:12:41.324—14:17:41.548`、`14:18:27.583—14:33:27.790`，中间有约 46 秒空档；断言发生在第二段内部，无采集错误。`14:13:55.327` 观察到模组启动，非本轮工具主动重启。ADB 曾掉线，只执行过电脑端 reconnect/server 重启和只读保活，没有把它当板端重启；操作全记在 `operations.log`。以下操作时间为电脑时间，交换耗时来自主控 monotonic，不跨时钟相减。

| 顺序 | 本轮实际操作/ID | 通信结论 | 实际结果 |
| --- | --- | --- | --- |
| 自动启动握手 | 主动 http 8，主控同 ID ACK0 | 通过 | sequence 42→43→44；同次实收空串，82 ms，errno/status=0。启动早期时钟无效时先 ACK2，随后同 ID ACK0 成功 |
| 1，14:20:23 | `bf01-wifi status` | 本地查询通过，不是 SDIO 测试 | sequence 54 前后不变，没有向模组发新报文 |
| 2，14:21:22 | get/status 2581 | 通过 | 同次 ACK0，106 ms，cloud=online、http=ready |
| 3，14:25:06 | get/dps 2586 | 通过 | 同次 ACK0，187 ms；421 字节，15 键齐全 |
| 4，14:25:16 | get/stream 2587 | 通过 | 同次 ACK0，85 ms；`state="idle", streamId=""` |
| 5，14:26:37 | 事先告知后执行一次 refresh-http，即 get/http 2589 | 缓存凭证查询通过；云端换新未完成验证 | 同次 ACK0，147 ms，403 字节完整凭证；模组明确打印“HTTP 凭证未过期”，不是重新订阅/换新。不能将之后整个 15 秒观察窗称作无断言 |
| 自动轮询，14:26:44 | get/status 2590 | 通过 | 同次 ACK0，106 ms；这是断言前最后一条主控 JSON 请求 |
| APP 真实开播，14:26:47 | 云端 IpcStreamStart 到模组 | **失败：未完成 Wi-Fi→主控下发** | 模组串口记录 start 后同一读取块出现断言；主控 wire 没有任何 stream，不能给出不存在的 SDIO stream ID |
| RTSP 发布、pushing、stop、断流状态 | 需要主控先收到真实 start | **前置未满足而未完成** | 主控 `generation=0, frames=0, state=idle`，没有启动发布，也没有假回开播 ACK0 |

本轮不是全部用例重跑；增量 set、file、未知 name 等没有执行，不能引用历史结果充作本轮通过。没有新增用户排除项。全量设置仍有延后执行字段，本轮没有把临时 ACK0 算作硬件设置已完成。

### 11.2 断言前后实际发包与收包

**最后一条手工 SDIO 查询：get/http 2589，主控→Wi-Fi，0x003B，sequence 73：**

```json
{"cmd":"get","name":"http","id":2589}
```

**同次 Wi-Fi→主控返回，sequence 74，147 ms，errno/status=0，完整实收内容：**

```json
{"cmd":"ack","id":2589,"result":0,"http":{"baseUrl":"http://47.107.170.93:18080","productCode":"P_SBF_F5gT8bN7","deviceSn":"D_1A18BB28B91fDq5ems6qpzy","version":"1.0.0","accessToken":"9f1181259837495abffb369ac1acb28ce2e9eea6ec88446ca607066dc41f1f42","httpHmacSignKey":"T3FJckJoM2VqZFQ0cm80dDdXRHBGdmJETnRheHVWeUE=","httpAesKey":"SFVHdlNYbzhQaXBlcFo3aTN2S0NEUmRjdEFGM1dTTWo=","expireTime":1796883268075}}
```

**断言前最后一条 JSON：自动 get/status 2590，主控→Wi-Fi，0x003B，sequence 75；模组串口电脑时间 14:26:44.6569584：**

```json
{"cmd":"get","name":"status","id":2590}
```

**同次 Wi-Fi→主控返回，sequence 76，106 ms；模组串口电脑时间 14:26:44.7184515：**

```json
{"cmd":"ack","id":2590,"result":0,"status":{"cloud":"online","wifi_signal_level":40,"battery_level":0,"motor_status":"normal","http":"ready"}}
```

**14:26:47.4417705，COM9 实收云端 start 与断言位于同一原始 3,968 字节读取块：**云端 commandId 为 `eb0404fa2c12d0052d209459fcebeacd`，streamId 为 `ipc_28ac1296ef248ab12c0f746e`。此处是云端→模组 UART 日志，不是主控收到的 SDIO JSON；URL/token 原始文本保存在 `关键串口原文.jsonl` 和原始字节块中，不补齐或修复 UART 损坏的字符。断言原文：

```text
<ASSERT>:HiMsg_RequestIndBuf 341,IPC=1528e4
Assert HiMsg_RequestIndBuf ErrCode 8 R2 155,R3 0
IPC = 0x12a26c
mepc = 0x710bc mstatus = 0x1888 mcause = 0x8000000b
```

**例子第 8 节要求：**模组经 `0x083B` 向主控发送 `cmd=stream, action=start` 的完整 JSON，主控收到后才执行推流并确认。**本轮实际：主控未收到该报文，不是收到空串、不是缺某字段、也不是主控已解析失败。**断言前最后一次成功查询之后，主控没有新的 JSON 请求或直播 ACK；原网络 watcher 仍有每秒 WSM GET_STATUS，JSON trace 不覆盖全部底层 WSM/IP 交互，不能据此说主控完全没做任何事。

**后续顺序：**`14:27:14.176` 模组再次打印启动标识；主控下一次正常定时查询为：

```json
{"cmd":"get","name":"status","id":2591}
```

该请求 sequence 77→78 **未收到业务回复**，8 ms 后记录 `transport_error_0x003B, errno=3, status=0`；同 ID 两次重试分别 8/9 ms 失败。status=0 是本地结构值，不是模组回复 ACK0。驱动 `wsm_cmd_send()` 在 BH 已终止时直接返回 `-3`。

`14:28:17.114` 又收到云端 start，commandId 为 `92677441f132e3ea1b6b53b0db276d6c`，模组打印“云端推流 start”。**这条晚到的 start 周围未再记录新的断言，但主控此时 SDIO 会话已经失效；不能拿它的正常 UART 行否认 14:26:47 的前一次断言，也不能说它本身又触发断言。**截至最后快照，正式服务仍只收到 12 条启动 set/http 指示，stream 为 0。

### 11.3 责任结论与下一步

- **已确定：**失败发生在“云端 start 已到模组，但主控尚未收到 0x083B stream”阶段，且存在模组 `HiMsg_RequestIndBuf:341` 断言及自动重启的原始证据。不是主控已经开始 RTSP 推流后把模组压垮，也没有证据说明 expireTime/URL 被主控解析拒绝。
- **尚未确定主控/Wi-Fi 谁造成底层异常：**标准 get/status、get/http 当次已成功，不等于完全排除前期累计影响、WSM/SDIO credit 或其他 IP 交互；ErrCode 8 不能无源码地解释为某一种缓冲错误。此前“ACK0 能触发断言”的因果措辞已更正为共现场景，本轮 http 8 ACK0 成功也否定“收到 ACK0 就必然断言”。
- **主控当前可确认的限制：**模组重启后，旧驱动会话没有自动恢复；本地状态仍可能显示 `sdio_open=true, last_error=0`、缓存 cloud=online，这不表示通信恢复。它们是恢复/可观测性问题，不可冒称已经找到最初断言的主控根因。本轮没有为此盲改协议或热重载。
- **下一轮主控验证：**双方干净上电后只保留正式自动握手，去掉本轮全部手工 get，再仅由 APP 触发一次真实 start；同步记录现有驱动 WSM 收发，确认后台 GET_STATUS/以太网收发/0x083B 是否先异常。主控单独重启且沿用故障模组状态，不是这个对照。当前旧 SDIO 会话已失效，继续点直播不能完成此对照。
- **Wi-Fi 工程师需提供的具体诊断：**用本轮断言地址和实际 commandId 还原调用栈；在 `HiMsg_RequestIndBuf` 入口记录调用者、申请长度、indication 分配/释放计数、空闲数、SDIO 收发序号与 credit，并确认 stream 的 0x083B 是尚未构造、未提交，还是已提交未被主控读取。只有拿到这些及主控 WSM 对照，才能决定修哪一端，不能仅凭函数名直接判模组业务代码有错。

本轮证据统一位于 `实机联调证据261110直播/真实直播1413/`：完整 wire、COM9 原始字节与电脑时间、所有操作、版本/进程/网络、内核/服务日志、`收发配对.jsonl`。密钥/令牌保留完整 wire 实收值；模组 UART 自行显示的 `***` 不反推为原包值。`analyse_causal.py` 只离线整理证据，不访问设备、不发协议包。暂存区 SHA256 未变。

## 12. part 分片适配与正式应用部署（15:35—15:52）

按用户提供的新格式，正式 `bf01_wifi_link.c` 在现有业务分发之前重组 `part`；保持原非分片接收、stream 执行、ACK 缓存不变。不是另起板端测试程序。每片实际 `0x083B` 原文仍由生产 wire 日志记录，重组成功另记 `reassembled id=... parts=... bytes=...`，不伪装成收到过一个完整 SDIO 单包。

- **确认规则：**缺片不执行、不逐片 ACK；按 `seq` 拼齐、核对内外 ID 后交原直播后端，以真实执行结果回原 ID 的 ACK0/1/2。支持乱序、相同片重发、完整片组重发幂等；15 秒缺片清理。当前上限为 32 片、总 JSON 1600 字节、URL 1400 字节，没有通过放大驱动 ABI 绕过限制。
- **本机验证通过，不是实机业务验收：**直接链接正式通信源码，13 组验证覆盖用户示例、转义、乱序/重发、缺片超时、错误 ID/片数、长度边界、ACK 失败重试和原单包 start/stop；另有 128 次打乱顺序测试覆盖 1—32 片。严格编译及 AddressSanitizer/UndefinedBehaviorSanitizer 检查无报错。未向板端发送示例里的占位地址或模拟 start。
- **编译和部署完成：**正式 ARM 应用、应用 install、SDK data 待打包目录和板端 MD5 均为 `08bee9b2b17a2b18391eda23540c0210`。只经 ADB 替换应用并重启协议服务，未重启板子、重载驱动或改配置；驱动仍为 `06220a534f736bcaa7363e52ee2c86f5`。SDK data 文件已同步，**未重新生成 data.spinand 镜像**，不要烧旧镜像当作已包含此适配。
- **板端普通通信通过：**新生产服务 PID 429；自动 get/status 2965 同次 ACK0，106 ms；get/dps 2966 同次 ACK0，187 ms，15 键完整。主控 boot ID 仍为 `c22d0c90-58e2-4a2b-8502-1de666e4884e`，原网络 watcher PID 305 保留，没有第二个 JSON 消费者。
- **真实分片和直播前置未满足而未完成：**截至 15:52 快照，新服务尚未收到任何 `0x083B` 指示，不能宣布分片真机验证、真实推流或模组不再断言已经通过。串口现由 Wi-Fi 工程师接管，本轮没有本机 COM9 证据，也未再打开串口争抢；需由工程师同步保存带电脑时间的模组日志。
- **热更新后的会话状态说明：**旧服务原来已有凭证，新服务重启后不沿用旧进程内存；Wi-Fi 尚未重新下发全量 set/http，主控 `http_usable=false`，但查询返回的模组状态为 `http=ready`。这是本次仅重启应用后的握手未重建，不是收到空凭证或返回 JSON 错误。完整联调前应由 Wi-Fi 重新发全量 set，按 report/http 顺序恢复；没有伪造凭证或假回成功来掩盖。

实际新服务首轮收发（`0x003B`，其余完整包见本轮 wire）：

```json
{"cmd":"get","name":"status","id":2965}
{"cmd":"ack","id":2965,"result":0,"status":{"cloud":"online","wifi_signal_level":13,"battery_level":0,"motor_status":"normal","http":"ready"}}
```

本轮构建、本机检查、部署操作与前后完整收发保存在 `实机联调证据261110分片/`；旧应用只备份在电脑 `/tmp/bf01-part-adapt-261110/ipcamera-before-part.bin`，没有回刷旧镜像。暂存区 SHA256 保持 `8b4da76ab28b12c557883a5094db830cf532371f48f78d218d8b0796643b5866`。

### 12.1 真实直播待 APP 触发（15:58—16:00）

按用户要求预启动现有正式应用的 `--live-camera` 入口，PID 524，未注入模拟 start 或占位 URL，未修改 AI/抓拍配置、驱动或 Wi-Fi 绑定。真实相机控制接口已就绪，启动日志确认沿用 RTOS 的 1920×1080、1280×720、640×360 编码通道；生产服务查询得到 `camera_errno=0`、`live_control_errno=0`，当前 `stream={"streamId":"","state":"idle","generation":0,"errno":0,"frames":0}`。这是后端就绪，不是已有推流画面。

完整真实 `part` 拼齐后直接交原 `stream` 执行流程，无需用户额外执行推流命令；现等待 APP 下发实际地址，才能验证分片收齐、业务 ACK、RTSP 实际发送帧和 APP 画面。主控生产 wire、服务和相机日志已开始按电脑时间跟踪；串口仍留给 Wi-Fi 工程师，本机未占用。准备过程见本轮证据内 `live-readiness-current.log`、`live-camera-start.log`、`live-ready.log`。

## 13. APP 真实分片直播：通讯通过，服务器拒绝发布（16:02—16:18）

依据例子第 8 节和 8.1，只使用正式应用和 APP 的真实 start，没有注入占位地址或模拟业务。以下具体收发为本轮 **id=22**；此前 id=7、10、13、16、19 的完整收发也保留在 wire 中，不把各轮地址拼在一起。表中时间取主控同一时钟；电脑接收时间另保存在 `live-pc-live-follow.log`。COM9 留给 Wi-Fi 工程师，本机没有占用，故不能据此宣布模组断言问题已经消失。

| 验证项 | 本轮结论 | 具体依据 |
| --- | --- | --- |
| 真实两片接收、转义解码、拼接、执行 | 通过 | `0x083B` 两片 507/264 字节，seq=0/1、cnt=2、id=22；拼接为 643 字节并进入真实 RTSP 发布器 |
| 地址是否被主控截断或改写 | 通过 | 抓包中 ANNOUNCE 请求行的完整 URL 与该组 part 解码后的 `pushUrl` 逐字节相同；实际 SDP 长度 760，与 Content-Length 相同 |
| 执行中确认、执行失败反馈 | 通过通讯检查，不是开播通过 | 先 ACK2；服务器拒绝后，同 ID 重发得到 ACK1；主控上报 fail，模组同 ID ACK0，104 ms |
| 真实推流、APP 画面 | **失败** | 服务器收到 ANNOUNCE 后返回 401；正式后端 `state=fail, errno=13, frames=0`，未进入 SETUP/RECORD 和实际发帧 |
| pushing 后 stop、断流恢复、APP 出画面 | 前置未满足而未完成 | 发布未获服务器允许，不能当作已测通过；本轮不代替整份例子的全项目复测 |

### 13.1 实际两端收发

Wi-Fi → 主控，`0x083B`，16:11:53.657 / 16:11:53.707，原文如下，包含完整实收令牌：

```json
{"cmd":"part","id":22,"seq":0,"cnt":2,"data":"{\"cmd\":\"stream\",\"id\":22,\"action\":\"start\",\"streamId\":\"ipc_28ac1296ef248ab12c0f746e\",\"pushProtocol\":\"RTSP\",\"pushUrl\":\"rtsp://120.24.118.1:554/live/ipc_28ac1296ef248ab12c0f746e?token=eyJwdXJwb3NlIjoiUFVTSCIsInRlbmFudENvZGUiOiJURU5BTlRfMTc3NjE2MDQ0NDY5NDY0Qjk0MyIsInVzZXJDb2RlIjoiVTE3OTM5NDgwNDUyNTUwMDJGQkYiLCJwcm9kdWN0Q29kZSI6IlBfU0JGX0Y1Z1Q4Yk43IiwiZGV2aWNlU24iOiJEXzFBMThCQjI4QjkxZkRxNWVtczZxcHp5Iiwic3RyZWFtSWQiOiJpcGNfMjhhYzEyOTZlZjI0OG"}
{"cmd":"part","id":22,"seq":1,"cnt":2,"data":"FiMTJjMGY3NDZlIiwiZXhwaXJlVGltZSI6MTc5NDkwMzExMjk1NX0.cf47b43fc6cb19eb00f43a96969c0e7bb862eb98164892fedef5136e97617578&expireTime=1794903112955\",\"channel\":\"ch1\",\"profile\":\"main\",\"expireTime\":1794903112955}"}
```

主控 → Wi-Fi，`0x003B`，16:11:54.255，拼齐后 548 ms 发送执行中确认：

```json
{"cmd":"ack","id":22,"result":2}
```

该 ACK 的 SDIO 调用返回长度为 0 的空字符串，耗时 40 ms、传输 errno/status 均为 0；不是“未收到 start”，也不是模组返回了开播 ACK0。16:11:58.256 / 58.307 模组完整重发同一两片；主控 16:11:59.257 按实际执行失败返回：

```json
{"cmd":"ack","id":22,"result":1}
```

16:12:00.257 主控 → Wi-Fi 上报失败，16:12:00.361 模组确认：

```json
{"cmd":"stream","action":"state","streamId":"ipc_28ac1296ef248ab12c0f746e","state":"fail","id":2995}
{"cmd":"ack","id":2995,"result":0}
```

### 13.2 服务器具体返回什么

正式发布器实际 TCP 抓包：主控 `192.168.10.147:44838` → 服务器 `120.24.118.1:554`。16:11:54.317 发出 ANNOUNCE 共 1335 字节；16:11:54.336 服务器 TCP ACK 已累计确认这 1335 字节。16:11:57.342 收到以下完整 265 字节回复，距请求 **3025 ms**：

```text
RTSP/1.0 401 Unauthorized
Content-Length: 35
Content-Type: text/plain
CSeq: 1
Date: Tue, Nov 10 2026 08:11:56 GMT
Server: ZLMediaKit(git hash:ac64e33/2026-07-24T11:51:08+08:00,branch:master,build time:2026-07-24T03:52:21)

[network err]:3(connection refused)
```

**责任边界：**这一次请求已经从主控完整到达流媒体服务器，并收到该服务器的明确拒绝；不是 SDIO 未拼齐，也不是主控凭证 JSON 解析失败。401 是服务器返回的状态，正文说的是 connection refused，**不能仅凭 401 说令牌错误，也不能把正文中的 network err 说成主控 TCP 建连失败**。服务器内部具体哪个连接被拒，需要平台/流媒体工程师核查同一时间的发布鉴权、上游连接和服务日志；现有板端证据不能再细分为哪个上游端口。

**Wi-Fi 方本轮无需为这条失败修改 part 字段。**请协助向平台方转交本组完整 URL、服务器时间 `2026-11-10 08:11:56 GMT`、streamId、ANNOUNCE 原文和 401 正文，查清并恢复被拒绝的连接，再下发新的真实 start 验证 SETUP/RECORD、pushing 和 APP 画面。没有绕过鉴权、换用假 URL 或假回开播成功。

### 13.3 主控已修复与部署边界

- **ANNOUNCE 原等待仅 2 秒：**本次实际回复需要约 3 秒，旧期限会先报 errno=110，掩盖服务器的具体回复。已改为有界 10 秒并记录建连、发送、等回复阶段和耗时，真机现能收到 401，返回 errno=13 / ACK1，而不是只报告超时。
- **失败缓存误挡同 streamId 的新地址：**原判断只看 streamId 和 fail；已同时区分 URL，避免新令牌地址被旧失败直接挡住。同一失败地址仍保留失败结果，不无限重推。
- 正式应用编译、本机发布器严格编译、ADB 部署完成；本机 out、应用 install、SDK data 待打包目录和板端 MD5 均为 `03ce786ef09f0ffcaa8b2e7266f2e842`。最终协议服务 PID 799、相机 PID 707；应用路径不再指向已删除的旧文件，驱动仍为 `06220a534f736bcaa7363e52ee2c86f5`，未重启板子、重载驱动或改 AI/抓拍/绑定配置。
- 最后切换协议服务后，正式 get/http id=3032 收到 ACK0 和完整有效凭证，146 ms；但该新进程尚未重收全量 set/report 握手，故 `http_usable=false`。这是本轮服务热更新后的会话前置未重建，不是空凭证、JSON 错误或上述 401 的原因；不能把它算作完整握手通过，也没有伪造 set 绕过。继续完整验收时需模组重发全量 set，按原流程完成 report/http。
- 仅更新 SDK 待打包文件，未重新生成 data.spinand。本轮完整原始 wire、电脑时间跟踪、实际 TCP pcap、ANNOUNCE 原始请求/回复和部署日志统一保存在 `实机联调证据261110分片/`，文件前缀为 `live-`。只读抓包工具不发送业务、不占用 SDIO JSON 接收通道，已结束。

### 13.4 推流参数逐项核对（16:38，只读检查）

**有实际 TCP 抓包可逐字节核对的是第 13 节 id=22，不能与重启后的 id=16 混用。**重新从原 pcap 提取 ANNOUNCE，与该组真实两片比较，结果如下：

| 参数 | Wi-Fi 下发 | 主控实际使用 | 比对结论 |
| --- | --- | --- | --- |
| pushUrl | 完整 465 字节 | ANNOUNCE 请求行 465 字节 | 逐字节相同，未截断、未额外编码；完整 token 保存在原始证据和参数比对 JSON 中 |
| pushProtocol | RTSP | RTSP/1.0 ANNOUNCE | 一致 |
| streamId | ipc_28ac1296ef248ab12c0f746e | URL 的 /live/ 后同名路径；token 载荷中也相同 | 三者一致 |
| expireTime | 1794903112955 | URL 查询参数和 token 载荷也为 1794903112955 | 三者一致，2026-11-17 16:11:52.955（UTC+8），本次请求时未过期 |
| channel / profile | ch1 / main | 校验 ch1；main → session0 → VENC0 | 与板端配置一致；实际 SDP 为 H264、1920×1080 |
| id | 22 | 业务 ACK 使用 22 | 一致；此 ID 不作为 RTSP 参数发送 |

token 载荷中的 productCode、deviceSn 分别为 `P_SBF_F5gT8bN7`、`D_1A18BB28B91fDq5ems6qpzy`，与模组 HTTP 凭证中的设备身份一致。这里只验证载荷内容及传输保真，**没有平台签名密钥，不能声称 token 签名已验证有效**。

当前板端在 16:31 左右发生重启，boot ID 为 `4106c78c-e192-4bb9-b271-c08ea4751bf7`；16:38 实读正式服务 PID 346、相机 PID 364，应用与驱动版本未变，只有一个 JSON 消费者。该轮最新实收 id=16 的 expireTime 为 `1794904449592`，其 URL/token 载荷/JSON 的有效期及流 ID 也一致；本轮已自动恢复真实全量握手，`http_usable=true`。但该轮没有保存出站 TCP 抓包，不能宣称其出站 URL 也已重新抓包比对；其正式发布器日志仍为 ANNOUNCE 401、frames=0。本次只读核对，没有改代码、重启服务或注入直播命令，串口留给 Wi-Fi 工程师。

比对结果和完整未脱敏数据见本轮证据内 `parameter-audit-comparison-report.json`；最新收发、版本、板端参数及日志见同前缀文件。
