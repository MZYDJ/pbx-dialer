#!/usr/bin/env python
# -*- coding: utf-8 -*-
# E2E: 事件报文处理
#   1. 同一条报文 1.5 秒内重复到达 -> 只记录一次（现场 PBX 会连发两三遍）
#   2. callstatus 事件字段名对不上 -> 打印原始报文，而不是 "分机 ? ?"
#   3. 不同 callid 的 callstatus -> 每条都记录
import ctypes
import json
import os
import subprocess
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "pbxdialer.exe")
INI = os.path.join(os.path.dirname(EXE), "pbxdialer.ini")
TITLE = "PBX 拨号器"
TOKEN = "testtoken001"
EV = "device/%s/api/v1.0/event" % TOKEN

sys.path.insert(0, os.path.join(HERE, "..", "testlibs"))
import paho.mqtt.client as mqtt  # noqa: E402

u = ctypes.windll.user32
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]

IDC_LOG = 1014

for _n in ("pbxdialer.exe", "\u62e8\u53f7.exe"):
    try:
        subprocess.run(["taskkill", "/F", "/IM", _n],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok))
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def log_text(hwnd):
    h = u.GetDlgItem(hwnd, IDC_LOG)
    n = u.SendMessageW(h, 0x000E, 0, 0)
    b = ctypes.create_unicode_buffer(n + 1)
    u.SendMessageW(h, 0x000D, n + 1, ctypes.addressof(b))
    return b.value


print("== E2E 事件报文处理 ==", flush=True)

with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=%s\n" % TOKEN)

proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE),
                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
time.sleep(6)
hwnd = u.FindWindowW(None, TITLE)
check("窗口出现", hwnd != 0)
check("已连接", "MQTT 已连接" in log_text(hwnd))

try:
    cl = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id="e2e-events")
except Exception:
    cl = mqtt.Client(client_id="e2e-events")
cl.connect("127.0.0.1", 1883, 30)
cl.loop_start()
time.sleep(0.5)

# 1) 同一条报文连发 3 次，只应记录一次
p = json.dumps({"event": "callstatus", "extension": "3143",
                "status": "Talking", "callid": "1794.1"})
for _ in range(3):
    cl.publish(EV, p)
    time.sleep(0.1)
time.sleep(1.5)
n = log_text(hwnd).count("分机 3143 Talking")
check("重复报文只记一次", n == 1, "记录 %d 条 (期望 1)" % n)

# 2) 字段名对不上 -> 打印原始报文，不再出现 "分机 ? ?"
q = json.dumps({"event": "callstatus", "callid": "1794.2",
                "channelid": "PJSIP/3143-0001"})
cl.publish(EV, q)
time.sleep(1.2)
t = log_text(hwnd)
check("字段未识别时给出提示", "字段未识别" in t)
tail = t.split("字段未识别")[-1][:200] if "字段未识别" in t else ""
check("提示里带原始报文", "channelid" in tail, tail.strip()[:80])
check("不再输出 '分机 ? ?'", "分机 ? ?" not in t)

# 3) 不同 callid 的同一事件都要记录
for i in range(2):
    cl.publish(EV, json.dumps({"event": "callstatus", "extension": "3150",
                               "status": "Ringing", "callid": "1794.%d" % (10 + i)}))
    time.sleep(0.4)
time.sleep(1.2)
n = log_text(hwnd).count("分机 3150 Ringing")
check("不同 callid 都记录", n == 2, "记录 %d 条 (期望 2)" % n)

cl.loop_stop()
u.PostMessageW(hwnd, 0x0010, 0, 0)
time.sleep(1)
try:
    proc.terminate()
except Exception:
    pass

print("\n== 结果 ==", flush=True)
bad = [n for n, ok in RESULTS if not ok]
print("通过 %d / %d" % (len(RESULTS) - len(bad), len(RESULTS)), flush=True)
if bad:
    print("失败: %s" % ", ".join(bad), flush=True)
sys.exit(1 if bad else 0)