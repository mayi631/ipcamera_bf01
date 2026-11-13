# 真实主控–Wi-Fi 诊断

仅供已连接真实板卡的诊断使用，不链接进生产应用，不自动安装，不模拟Wi-Fi。主验收文档是 `WIFI模块通讯/主控联调测试例子.md`；先按仓库根 `AGENTS.md` 的联调约定执行。

`wire_probe.c` 逐条执行三列TSV：测试名、操作、参数。操作 `exchange` 原样发送第三列JSON，包括测试空串；`listen` 只取驱动异步队列；`busy-set` / `reject-set` 等到下一条真实set后分别回同ID的ACK2/ACK1。这两个分支都不会回ACK0，也不应用设置，不应被记录为全量设置成功。

各请求与回复分开记录为JSONL，包含序号、测试名、单调/实时时间、完整报文、传输状态和交换耗时。没有报文是未收到，有空payload是确实收到空字符串，不能混淆。只在驱动ioctl传输失败时中止；ACK1/2保留为业务结果，不能把成功分支未通过伪装为程序崩溃。

交叉构建示例，从仓库根执行：

```sh
ARM_CC=/path/to/arm-linux-gnueabihf-gcc
"$ARM_CC" -O2 -std=gnu11 -Wall -Wextra -Werror -static \
  -Imodules/common/include tools/bf01_wifi/diagnostics/wire_probe.c \
  modules/common/cjson/src/cJSON.c -lm -o /tmp/bf01-wire-probe
```

使用前确认没有生产协议进程或其它诊断进程消费 `0x083B`。本工具锁只约束同工具实例，不能代替检查其它消费者。用电脑时间记录ADB运行窗口，并同时被动记录Wi-Fi串口；板端时间可能无效，不可伪称已同步。诊断程序在 `/tmp` 运行，不替换相机或启动入口。

举例，三列以真实TAB分隔，以下表示语义而不是可以直接粘贴的TSV：

```text
observe-retry <TAB> listen <TAB> 13000
status <TAB> exchange <TAB> {"cmd":"get","id":9500001,"name":"status"}
busy <TAB> busy-set <TAB> 8000
reject <TAB> reject-set <TAB> 8000
```

`reject-set` 是明确拒绝当前未实现的设置指令，可能结束模块当前待确认任务；不是被动监听，只有在本轮确实需要测试文档拒绝分支时使用。不要因为工具能构造任意报文而捏造成功report、推流start或文件上传结果。用户排除直播时，第8节直播动作列为未测，不启动摄像头。

原始日志归档后可以生成不脱敏的逐包记录和字段核对：

```sh
python3 tools/bf01_wifi/diagnostics/render_trace.py /path/to/SDIO.jsonl /path/to/evidence
```

输出 `逐项收发.md` 和 `报文核对.json`。工具检查字节长度、收发配对和数字ID，区分空串、缺字段和传输错误；不把ACK0自动判作动作已生效，也不从串口混行补造凭证。HTTP查询只返回ACK2时明确写整个http对象未返回，不将不存在的expireTime写成空或非法。该工具不连接板卡、不发包、不修改协议会话。
