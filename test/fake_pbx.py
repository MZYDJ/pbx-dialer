#!/usr/bin/env python
# 模拟 Smart PBX: 作为 MQTT 客户端连接 Broker,
# 订阅 device/<token>/api/v1.0/command/# 并回复 response / 推送 event。
# 用于在无真机环境下验证拨号器 (pbxdialer) 的完整流程。
import json
import os
import sys
import time
import random

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "testlibs"))

import paho.mqtt.client as mqtt  # noqa: E402

TOKEN = os.environ.get("PBX_TOKEN", "testtoken001")
HOST = os.environ.get("PBX_MQTT_HOST", "127.0.0.1")
PORT = int(os.environ.get("PBX_MQTT_PORT", "1883"))

BASE = f"device/{TOKEN}/api/v1.0"

# 模拟分机列表 (与文档示例字段一致)
EXTLIST = [
    {"status": "Idle", "type": "SIP", "number": "8001", "username": "前台"},
    {"status": "Idle", "type": "SIP", "number": "8002", "username": "技术部"},
    {"status": "Ringing", "type": "SIP", "number": "8003", "username": "销售部"},
    {"status": "Idle", "type": "FXS", "number": "2001", "port": "1", "username": ""},
]

_last_callid = {}


def on_connect(client, userdata, flags, rc, properties=None):
    print(f"[fake-pbx] connected rc={rc}", flush=True)
    if rc == 0:
        client.subscribe(f"{BASE}/command/#", qos=0)
        print(f"[fake-pbx] subscribed {BASE}/command/#", flush=True)


def publish_response(client, payload):
    # 真实 PBX 的报文是多行格式化 JSON, 这里保持一致以验证解析
    client.publish(f"{BASE}/response", json.dumps(payload, ensure_ascii=False, indent=2))


def publish_event(client, payload):
    client.publish(f"{BASE}/event", json.dumps(payload, ensure_ascii=False, indent=2))


def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        print(f"[fake-pbx] bad payload on {msg.topic}", flush=True)
        return
    cmd = data.get("cmd")
    rid = data.get("request_id", "")
    print(f"[fake-pbx] recv {msg.topic} cmd={cmd} rid={rid}", flush=True)

    if msg.topic.endswith("/command/system"):
        if cmd == "extension_list":
            publish_response(client, {
                "status": "Success", "message": "success", "request_id": rid,
                "extlist": EXTLIST,
            })
        elif cmd == "deviceinfo":
            publish_response(client, {
                "status": "Success", "message": "success", "request_id": rid,
                "deviceinfo": {"devicename": "FakePBX", "sn": "SN12345678",
                               "version": "V1.0.2"},
            })
        else:
            publish_response(client, {
                "status": "Success", "message": "success", "request_id": rid,
            })

    elif msg.topic.endswith("/command/call"):
        if cmd == "dial":
            caller = data.get("caller", "?")
            callee = data.get("callee", "?")
            callid = f"1673{int(time.time()) % 100000000}.{random.randint(1000, 9999)}"
            _last_callid["cid"] = callid
            print(f"[fake-pbx] DIAL {caller} -> {callee} callid={callid} "
                  f"autoanswer={data.get('autoanswer')}", flush=True)
            # 先推送一个 invite 事件, 再返回 dial 响应
            publish_event(client, {
                "event": "invite", "from": caller, "to": callee, "callid": callid,
            })
            time.sleep(0.05)
            publish_response(client, {
                "status": "Success", "message": "success", "request_id": rid,
                "callid": callid,
            })
            time.sleep(0.05)
            # 真实 PBX 会推送 callstatus / extension_status 事件
            publish_event(client, {
                "event": "callstatus", "extension": caller,
                "status": "InUse", "callid": callid,
            })
        elif cmd == "hangup":
            cid = data.get("callid", _last_callid.get("cid", ""))
            print(f"[fake-pbx] HANGUP callid={cid}", flush=True)
            publish_response(client, {
                "status": "Success", "message": "Channel Hungup", "request_id": rid,
            })
            time.sleep(0.05)
            publish_event(client, {"event": "hangup", "callid": cid})
        else:
            publish_response(client, {
                "status": "Success", "message": "success", "request_id": rid,
            })


def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="fake-pbx")
    client.on_connect = on_connect
    client.on_message = on_message
    print(f"[fake-pbx] connecting {HOST}:{PORT} token={TOKEN}", flush=True)
    client.connect(HOST, PORT, keepalive=60)
    client.loop_forever()


if __name__ == "__main__":
    main()
