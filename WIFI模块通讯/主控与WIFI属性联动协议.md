# 主控与 Wi-Fi 联动协议

> 主控：CV1815J  
> Wi-Fi：ATBM6461  
> 云端属性以《昆仑物联网平台设备对接方案》3.4、4.4、第 5 章为准。键名与当前固件词表一致。  
> 本文只定义主控和 Wi-Fi 之间的报文。一条报文是 JSON，不带换行。
> Wi-Fi 发给主控的 `set`、`http`、`stream` 走 SDIO 指示 `0x083B`，载荷就是这条 JSON 加结尾 0。主控用 `0x003B` 的 `trans` 回 `ack`，`id` 与该条相同。
> 主控发给 Wi-Fi 的 `get`、`report`、`file`、`stream state` 仍走 Direct Buffer `0x003B`。同一次交换只带回这条请求自己的应答，空串不再取出 Wi-Fi 待发的下一条。

板内链路不加密。加密只存在于 Wi-Fi 和云端之间，以及主控按第 6 节去访问云端 HTTP 时。一条报文是一个 UTF-8 JSON，本身不含换行。布尔只用 `true` / `false`。未知键丢掉，其它键照常执行。日志不打印 token、密钥、推流地址。

---

### 分片传输（2026-11-10 补充）

按本次 Wi-Fi 工程师提供的新格式，较长的主动报文可以经 `0x083B` 拆成多条 `part`；原来的非分片报文继续兼容。例如：

```json
{"cmd":"part","id":9,"seq":0,"cnt":2,"data":"{\"cmd\":\"stream\",\"id\":9,\"action\":\"start\",\"streamId\":\"ipc_1\",\"pushProtocol\":\"RTSP\",\"pushUrl\":\"rtsp://host/live/x?token=AAAA"}
{"cmd":"part","id":9,"seq":1,"cnt":2,"data":"BBBB\",\"channel\":\"ch1\",\"profile\":\"main\",\"expireTime\":1794898006237}"}
```

- `id` 是原始业务报文的 ID，同组所有片一致；`seq` 从 0 开始，`cnt` 为总片数。`data` 是 JSON 字符串，先解开外层 JSON 转义，再按 `seq` 拼接内容，不能直接拼外层报文。
- 主控不逐片发送 ACK，不在缺片时执行直播。收齐并校验内外 ID 一致后，交给原有业务处理，仍从 `0x003B` 回原 ID 的 ACK；结果取决于真实业务执行，不因拼接完成就固定回 ACK0。
- 同组支持乱序和相同片重发；完整报文重发沿用原有幂等处理。需要重试时，应重发同 ID 的完整片组，而不是只重发最后一片。一次只保留一组未完成报文，不混拼不同 ID。
- 当前主控实现边界：每片线上 JSON 不超过 1600 字节，总计最多 32 片，重组后的原始 JSON 不超过 1600 字节，`pushUrl` 原有上限仍为 1400 字节。分片解决模组单包发送限制，不改变这些业务长度限制。
- 首片到达后 15 秒仍未收齐则清理，不回成功确认；片数改变、同序号内容冲突、长度越界会清理本组。完整内容不是合法业务 JSON、内外 ID 不一致或再次嵌套 `part`，按外层原 ID 拒绝 ACK1，不执行。

---

## 1. 一发一确定

每条指令都带 `id`。发送方从 1 开始各自递增，两边的计数互不影响。接收方必须回一条确认，`id` 与收到的指令相同。上一条没有确认之前，不发下一条。

```json
{"cmd":"ack","id":4,"result":0}
```

| result | 含义 |
| --- | --- |
| 0 | 已收下。瞬时动作表示已经做完；耗时动作表示已经开始 |
| 1 | 拒绝。JSON 不合法、取值不在允许范围、推流已过期、协议不支持 |
| 2 | 还不能做。主控未就绪、Wi-Fi 未上线、HTTP 凭证没有 |

3 秒内没有确认就按同一个 `id` 再发，最多 3 次。3 次都没有确认，这条链路视为断开。链路恢复后，Wi-Fi 重新发全量 `set` 和 `http`，主控重新发全量 `report`。

同一个 `id` 再到达时，只再回一次确认，不重复执行。

耗时动作做完后另发一条结果，这条结果自己也要确认：

- 改设置、改只读状态：确认表示已经生效。
- `memory_format`、直播开始：确认表示已经开始。做完再用 `report` 或 `stream` 的 `state` 送结果。
- 文件上传：确认表示进度已记下。成败在 `file` 指令的 `result` 里。

---

## 2. 指令

| cmd | 方向 | 作用 |
| --- | --- | --- |
| `set` | Wi-Fi → 主控 | 下发要主控执行的设置 |
| `report` | 主控 → Wi-Fi | 送回只读状态。全量或只带变化的键 |
| `get` | 双向 | 随时读取对方当前数据。确认里带结果 |
| `http` | Wi-Fi → 主控 | 把主控调用云端 HTTP 要用的凭证送过去 |
| `file` | 主控 → Wi-Fi | 通知一次抓拍或日志上传的进度和结果 |
| `stream` | Wi-Fi → 主控为开始和停止；主控 → Wi-Fi 为中途状态 | 直播 |
| `ack` | 回给发送方 | 上一条指令的确认 |

`get` 的确认就是回复，不再另发一条：

```json
{"cmd":"get","id":8,"name":"status"}
```

```json
{"cmd":"ack","id":8,"result":0,"status":{}}
```

`name` 只允许：

| name | 向谁要 | 确认里带的对象 |
| --- | --- | --- |
| `status` | 对方 | `status` |
| `dps` | 对方 | `dps`，只含第 3 节的主控设置 |
| `http` | 只向 Wi-Fi 要 | `http`，字段同第 6 节 |
| `stream` | 对方 | `stream` |

---

## 3. 交给主控执行的设置

Wi-Fi 保存云端下发的值，只把主控要执行的键放进 `set`。链路刚恢复时发全量。之后云端改了哪些键，就只发哪些键。

```json
{"cmd":"set","id":4,"dps":{"pir_sens_level":"low"}}
```

| 键 | 类型 | 允许值 | 云端默认 | 主控动作 |
| --- | --- | --- | --- | --- |
| `pir_sens_level` | 字符串 | `low` `medium` `high` | `high` | 抓拍门限。PIR 引脚在 6461，引脚驱动后期再做 |
| `pir_detect_intv` | 整数 | 0、5、10、15、20、25、30，单位分钟 | 0 | 两次抓拍的最小间隔 |
| `pir_record_dur` | 整数 | -1、0、10、20、30，单位秒 | 10 | 录像时长。-1 由 AI 决定，0 表示不录像 |
| `ai_detect_switch` | 布尔 | `true` `false` | `true` | AI 总开关。关则鸟、松鼠、其他目标都不通知 |
| `bird_detect_switch` | 布尔 | `true` `false` | `true` | 鸟类通知。前提是 AI 总开关打开 |
| `squirrel_detect_switch` | 布尔 | `true` `false` | `true` | 松鼠通知。前提是 AI 总开关打开 |
| `other_detect_switch` | 布尔 | `true` `false` | `false` | 其他目标通知。前提是 AI 总开关打开 |
| `image_filter_level` | 字符串 | `low` `medium` `high` | `medium` | 图像筛选。前提是 AI 总开关打开 |
| `image_anti_flicker` | 字符串 | `off` `50Hz` `60Hz` | `60Hz` | 抗闪烁 |
| `brand_wm_switch` | 布尔 | `true` `false` | `true` | 品牌水印 |
| `datetime_wm_switch` | 布尔 | `true` `false` | `true` | 日期时间水印 |
| `datetime_wm_format` | 字符串 | 见下 | `MM/dd/yyyy HH:mm:ss` | 水印时间格式 |
| `mic_switch` | 布尔 | `true` `false` | `false` | 麦克风录音 |
| `spk_vol` | 整数 | 0 到 100，步长 5 | 80 | 喇叭音量 |
| `status_led_switch` | 布尔 | `true` `false` | `true` | 状态指示灯。原理图未单列此灯时收下即可 |
| `memory_format` | 布尔 | `true` | 无，不保存 | 为 `true` 时开始格式化存储卡 |

`datetime_wm_format` 只允许 `yyyy-MM-dd HH:mm:ss`、`MM/dd/yyyy HH:mm:ss`、`dd-MM-yyyy HH:mm:ss`。

喂食相关键不在 `set` 里。它们由 Wi-Fi 自己处理。

---

## 4. 主控送回的只读状态

```json
{"cmd":"report","id":5,"properties":{"memory_status":"normal"}}
```

链路刚恢复时发全量。之后只发变过的键。Wi-Fi 确认后，向云端上报：每次上线报完整 `dps` 和 `properties`，同一次连接里随后只报变过的键。

| 键 | 类型 | 允许值 | 说明 |
| --- | --- | --- | --- |
| `memory_capacity` | 对象 | `total`、`used`，单位 GB | `total` 为整数，`used` 最多一位小数 |
| `memory_status` | 字符串 | `normal` `unmount` `error` | 存储卡状态 |
| `memory_format_status` | 字符串 | `formatting` `success` `fail` | 格式化过程 |
| `firmware_version` | 字符串 | 主控版本号 | Wi-Fi 用它作为云端 `firmware_version` |

格式化先报 `formatting`，结束后再报 `success` 或 `fail`。失败时把结束后的容量和卡状态一并带上。Wi-Fi 还没收到过 `report` 时，云端用默认值：容量 0、卡状态 `unmount`、格式状态 `success`、版本 `1.0.0`。

---

## 5. 随时可读取的状态

`get` 且 `name` 为 `status` 时，对方把当前状态放进确认。任何时候都可以读，不必等对方主动上报。

主控回答：

```json
{"cmd":"ack","id":8,"result":0,"status":{"memory_capacity":{"total":32,"used":1.2},"memory_status":"normal","memory_format_status":"success","firmware_version":"1.0.0","stream":"idle","record":"idle"}}
```

| 字段 | 允许值 | 说明 |
| --- | --- | --- |
| `memory_capacity` `memory_status` `memory_format_status` `firmware_version` | 同第 4 节 | 与最近一次 `report` 一致 |
| `stream` | `idle` `pushing` `fail` | 直播 |
| `record` | `idle` `recording` | 是否正在录像 |

Wi-Fi 回答：

```json
{"cmd":"ack","id":9,"result":0,"status":{"cloud":"online","wifi_signal_level":40,"battery_level":0,"motor_status":"normal","http":"ready"}}
```

| 字段 | 允许值 | 说明 |
| --- | --- | --- |
| `cloud` | `online` `offline` | 当前这条 MQTT 是否已上线 |
| `wifi_signal_level` | 0 到 100 | Wi-Fi 自己读取 |
| `battery_level` | 0 到 100 | 电量计未测时为 0 |
| `motor_status` | `normal` `block` `fault` | 马达未测时为 `normal` |
| `http` | `ready` `none` | `ready` 表示第 6 节的凭证可用 |

`name` 为 `stream` 时，确认是 `{"state":"idle","streamId":""}`。`state` 为 `idle`、`pushing` 或 `fail`。

`name` 为 `dps` 时，确认里是第 3 节当前正在生效的设置，不带 `memory_format`。

---

## 6. 主控使用的 HTTP

图片、短视频、日志由主控调用云端 HTTP。Wi-Fi 只提供凭证，不代替主控上传。影子 `shadow/get` 和 `device/state` 仍由 Wi-Fi 调用，主控不重复发。

凭证有变化，或主控发来 `get` 且 `name` 为 `http` 时，Wi-Fi 下发：

```json
{"cmd":"http","id":3,"baseUrl":"https://gateway","productCode":"P_SBF_F5gT8bN7","deviceSn":"","version":"1.0.0","accessToken":"","httpHmacSignKey":"","httpAesKey":"","expireTime":0}
```

| 字段 | 说明 |
| --- | --- |
| `baseUrl` | 网关地址，不含路径末尾的斜杠。Wi-Fi 填当前在用的地址 |
| `productCode` `deviceSn` | 请求头 `X-Device-ProductCode`、`X-Device-SN` |
| `version` | 请求头 `X-Device-Version`，用主控版本 |
| `accessToken` | 请求头 `Authorization: Bearer ` 后面的值 |
| `httpHmacSignKey` | 签名密钥，Base64 的 32 字节 |
| `httpAesKey` | 正文加密密钥，Base64 的 32 字节 |
| `expireTime` | `accessToken` 失效的 13 位毫秒时间戳 |

主控确认 `result` 为 0 之后才能发 HTTP。距离 `expireTime` 不足 10 分钟，或云端返回 401，主控发 `get`，`name` 为 `http`。Wi-Fi 用 MQTT `httpToken/get` 换新凭证，再放进确认的 `http` 对象。换不到则确认 `result` 为 2。

`refreshToken` 只留在 Wi-Fi，不下发给主控。

主控调用云端时：

1. 明文 JSON 记为 `plainJson`。`nonce` 为 32 位十六进制。`timestamp` 为 13 位毫秒。
2. `X-Device-Sign` 为小写十六进制 `HMAC-SHA256(httpHmacSignKey, plainJson + nonce + timestamp)`。`timestamp` 按十进制字符串拼接。
3. 请求体是 `{"encryptedData": Base64(AES-GCM(plainJson, httpAesKey))}`。
4. 请求头再带 `Content-Type: application/json`、`X-Device-ProductCode`、`X-Device-SN`、`X-Device-Version`、`X-Device-Timestamp`、`X-Device-Nonce`、`Authorization`。

主控按这个顺序做一次文件：

1. `POST {baseUrl}/api/device-api/files/uploads/prepare`  
   明文：`purpose` 为 `image`、`cloud_video` 或 `log`。`files` 最多 10 个，每项只有 `fileType`：`jpg`、`mp4`、`zip` 或 `log`。
2. 响应解密后取出每个文件的 `fileId`、`uploadUrl`、`uploadMethod`、`expireTime`、`headers`、`enc`、`encType`、`fileEncryptKey`。`uploadMethod` 为 `PUT`。`enc` 为 `true` 时，先按 `encType` 用 `fileEncryptKey` 加密文件再上传。加密字节布局以云端 4.4 为准。
3. 按 `uploadMethod` 把文件 PUT 到 `uploadUrl`，带上响应里的 `headers`。这一步不带设备的 `Authorization`。
4. `POST {baseUrl}/api/device-api/files/uploads/report`  
   明文含 `purpose`、`files`、`params`。`files` 每项含 `fileId`、`fileType`、`keyId`、`fileSize`，图片和视频再加 `detectType`。`params` 含 `eventId`、`alarm`、`eventTime`。日志的 `alarm` 为 `false`，并且不带 `detectType`。

| 字段 | 由谁产生 | 允许值 |
| --- | --- | --- |
| `detectType` | 主控按 AI 结果填写 | `bird` `squirrel` `other` |
| `eventId` | 主控生成，16 位十六进制 | 同一次抓拍的图片和视频用同一个值 |
| `eventTime` | 主控 | 13 位毫秒 |
| `alarm` | 主控 | 图片、视频需要推送时为 `true`；日志为 `false` |
| `fileSize` | 主控 | 上传字节数 |

2026-11-12 实测兼容补充：当前云端图片/视频report会将 `files[].sha256` 校验为必填，视频还会将 `files[].duration` 校验为必填；这与参考旧文档的“可选”标注及本节原字段清单存在差异。主控已补传实际上传文件包体的64位小写十六进制SHA-256，以及视频实测时长（数字，秒）；两者属于上述HTTP report字段，不增加SDIO `file`通知字段。加密包摘要包含KLF1头、随机IV、密文及Tag，不是源媒体摘要。缺少原密文摘要的历史已上传对象不盲目重传，需平台核对。原始失败响应、修复及真实云验证见 `out/bf01-report-fix-20261112-1433/RESULT.md`；该实测补充不代表所有平台协议变更已经双方验收。

云端 `report` 返回 `code` 为 200 后，主控通知 Wi-Fi。失败也通知：

```json
{"cmd":"file","id":15,"phase":"done","purpose":"image","eventId":"0123456789abcdef","detectType":"bird","fileId":"","result":0}
```

`phase` 为 `prepare`、`upload` 或 `done`。`result` 为 0 表示这一阶段成功，1 表示失败。Wi-Fi 只确认并记下，不再向云端重复调用这三个接口。

---

## 7. 直播

云端从 `service/invoke` 下发 `IpcStreamStart` 或 `IpcStreamStop`。Wi-Fi 解密后转给主控，主控推流。

开始：

```json
{"cmd":"stream","id":20,"action":"start","streamId":"","pushProtocol":"RTMP","pushUrl":"","channel":"ch1","profile":"main","expireTime":0}
```

| 字段 | 说明 |
| --- | --- |
| `streamId` | 云端给出的流 ID |
| `pushProtocol` | `RTMP`、`RTSP` 或 `GB28181`。不认识则确认 `result` 为 1 |
| `pushUrl` | 推流地址。过期或为空则确认 `result` 为 1，不推流 |
| `channel` | 默认 `ch1` |
| `profile` | `main` 或子码流。云端示例为 `main` |
| `expireTime` | 13 位毫秒。当前时间已经超过它，确认 `result` 为 1 |

主控确认 `result` 为 0 表示已经在推。摄像机未就绪则确认 `result` 为 2，不推流。

停止：

```json
{"cmd":"stream","id":21,"action":"stop","streamId":""}
```

确认 `result` 为 0 表示已经停止。云端停止指令没有 `streamId` 时，Wi-Fi 填最近一次开始的 `streamId`。

推流中途断开时，主控另发：

```json
{"cmd":"stream","id":22,"action":"state","streamId":"","state":"fail"}
```

`state` 为 `pushing`、`idle` 或 `fail`。Wi-Fi 确认。对云端的应答由 Wi-Fi 在收到主控确认或 `state` 之后处理。

---

## 8. 工作周期

1. 链路恢复。双方都可以开始发 `get`。
2. Wi-Fi 发全量 `set`，等到确认。
3. 主控发全量 `report`，等到确认。Wi-Fi 再向云端报一次完整属性。
4. Wi-Fi 发 `http`，等到确认。此后主控才能上传文件。
5. 云端改了主控设置：Wi-Fi 发只含变化键的 `set`，确认后向云端只报这些键。
6. 主控状态变化：发只含变化键的 `report`，确认后 Wi-Fi 向云端只报这些键。
7. 抓拍或录像：主控生成 `eventId`，按第 6 节 prepare、PUT、report，每个阶段发 `file` 并等待确认。
8. 直播：Wi-Fi 发 `stream` 的 `start` 或 `stop`，以主控确认为准。中途变化由主控发 `state`。
9. 任一方需要当前情况时发 `get`，在确认里读到结果。
10. 断线并再次上线：从第 1 步再做。云端完整上报只跟在新的一次上线后面。

同一条云端设置里如果夹有主控字段和 Wi-Fi 字段，Wi-Fi 字段自己处理，不放进 `set`。

---

## 9. 不在本协议里

- 喂食：`feed_plan_switch`、`feed_plan`、`feed_manual`。马达在 6461，外设未测。
- `battery_level`、`motor_status`、`wifi_signal_level` 的采集。电量和电机未测，信号由 Wi-Fi 自己读。本协议只在 `get` 的状态里提供当前值。
- 升级：`NewVersion`、ota 主题、HTTP `upgrade-info`。后期再做。
- PIR 触发、红外对管、按键、RTC、太阳能、NTC。外设未测。抓拍触发后期另加一条报文。
