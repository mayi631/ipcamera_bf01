#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


EVENTS = {
    "host_to_wifi_0x003B": "主控 → Wi-Fi / 0x003B",
    "wifi_to_host_0x003B": "Wi-Fi → 主控 / 0x003B 同次应答",
    "wifi_to_host_0x083B": "Wi-Fi → 主控 / 0x083B 主动下发",
}
HTTP_FIELDS = (
    "baseUrl", "productCode", "deviceSn", "version", "accessToken",
    "httpHmacSignKey", "httpAesKey", "expireTime",
)


def parsed_object(payload):
    try:
        value = json.loads(payload)
    except (json.JSONDecodeError, TypeError):
        return None
    return value if isinstance(value, dict) else None


def describe_field(value, name):
    if name not in value:
        return "字段不存在"
    field = value[name]
    if field is None:
        return "JSON null"
    if field == "":
        return '空字符串 ""'
    return json.dumps(field, ensure_ascii=False, separators=(",", ":"))


def render(trace_path, output_path):
    events = []
    for line_number, line in enumerate(trace_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("{"):
            continue
        event = json.loads(line)
        if not isinstance(event, dict) or "event" not in event:
            raise ValueError(f"{trace_path}:{line_number}: not a probe event")
        if "payload" in event:
            length = len(event["payload"].encode("utf-8"))
            if length != event["length"]:
                raise ValueError(f"{trace_path}:{line_number}: payload length mismatch")
        events.append((line_number, event))
    requests = {}
    exchanges = []
    problems = []
    sections = [
        "# 真实收发逐包记录", "",
        f"源文件：`{trace_path.name}`。仅转换排版，payload 原样保留，不脱敏、不修补。",
        "", "板端 realtime_ms 不等于电脑时间；电脑时间请对照执行窗口和 COM9 记录。", "",
    ]
    for line_number, event in events:
        kind = event["event"]
        if kind not in EVENTS and not kind.startswith("transport_error") and kind != "local_reject":
            continue
        sections.extend([
            f"## {event['sequence']}. {event['case']} / {EVENTS.get(kind, kind)}", "",
            f"原始行 {line_number}；monotonic_ms={event['monotonic_ms']}；"
            f"elapsed_ms={event['elapsed_ms']}；errno={event['errno']}；status={event['status']}。", "",
        ])
        if "payload" not in event:
            sections.extend(["没有有效收包内容；不能写成收到了空字符串。", ""])
        elif event["payload"] == "":
            sections.extend(['实际内容：空字符串 ""（0 字节）。', ""])
        else:
            sections.extend(["```text", event["payload"], "```", ""])
        if event["errno"] or event["status"]:
            problems.append({"case": event["case"], "event": kind, "line": line_number})
        if kind == "host_to_wifi_0x003B":
            if event["case"] in requests:
                problems.append({"case": event["case"], "problem": "unpaired request"})
            requests[event["case"]] = event
        elif kind == "wifi_to_host_0x003B":
            request = requests.pop(event["case"], None)
            if request is None:
                problems.append({"case": event["case"], "problem": "unpaired reply"})
            request_value = parsed_object(request["payload"]) if request else None
            reply_value = parsed_object(event["payload"])
            exchange = {
                "case": event["case"], "source_line": line_number,
                "request": request["payload"] if request else None,
                "reply": event["payload"], "elapsed_ms": event["elapsed_ms"],
            }
            if request_value and reply_value:
                exchange["matching_id"] = (
                    type(request_value.get("id")) is int and
                    type(reply_value.get("id")) is int and
                    request_value["id"] == reply_value["id"]
                )
                if not exchange["matching_id"]:
                    problems.append({"case": event["case"], "problem": "ID mismatch"})
            if request_value and request_value.get("cmd") == "get" and request_value.get("name") == "http":
                sections.append("HTTP 字段定位：")
                sections.append("")
                if not reply_value:
                    exchange["http_fields"] = "没有 JSON 对象应答"
                elif "http" not in reply_value:
                    exchange["http_fields"] = "整个 http 对象未返回，不是空字符串/null/非法 JSON"
                elif not isinstance(reply_value["http"], dict):
                    exchange["http_fields"] = describe_field(reply_value, "http")
                else:
                    exchange["http_fields"] = {
                        name: describe_field(reply_value["http"], name) for name in HTTP_FIELDS
                    }
                sections.extend([
                    "```text", json.dumps(exchange["http_fields"], ensure_ascii=False, indent=2), "```", "",
                    "result=2 是暂未取得凭证；没有 http 对象不等于 expireTime 为空或解析失败。", "",
                ])
            exchanges.append(exchange)
    summary = {
        "source": trace_path.name, "event_count": len(events),
        "exchanges": exchanges,
        "async_messages": sum(event["event"] == "wifi_to_host_0x083B" for _, event in events),
        "transport_errors": sum(event["event"].startswith("transport_error") for _, event in events),
        "unpaired_requests": list(requests), "integrity_problems": problems,
    }
    output_path.mkdir(parents=True, exist_ok=True)
    (output_path / "逐项收发.md").write_text("\n".join(sections), encoding="utf-8")
    (output_path / "报文核对.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return summary


def main():
    parser = argparse.ArgumentParser(description="从实收 probe JSONL 生成不脱敏的逐包证据，不判定业务动作成功")
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    summary = render(arguments.trace, arguments.output)
    print(f"exchanges={len(summary['exchanges'])} async={summary['async_messages']} "
          f"transport_errors={summary['transport_errors']} integrity_problems={len(summary['integrity_problems'])}")
    return 1 if summary["integrity_problems"] or summary["unpaired_requests"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
