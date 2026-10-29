#!/usr/bin/env python
# 验证分机列表解析: 标准格式 / 无 extlist 包裹 / 只有 number 键
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
RESP = f"device/{TOKEN}/api/v1.0/response"

sys.path.insert(0, os.path.join(ROOT, "testlibs"))
import paho.mqtt.client as mqtt

u = ctypes.windll.user32
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]

IDC_CALLER, IDC_LOG = 1009, 1014
CB_GETCOUNT, CB_GETLBTEXT = 0x0146, 0x0148

# 测试前先清掉可能残留的实例 (单实例保护会让新进程直接退出)
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


def combo_items(hwnd):
    h = u.GetDlgItem(hwnd, IDC_CALLER)
    n = u.SendMessageW(h, CB_GETCOUNT, 0, 0)
    items = []
    for i in range(n):
        if i > 3000:
            break
        b = ctypes.create_unicode_buffer(128)
        u.SendMessageW(h, CB_GETLBTEXT, i, ctypes.addressof(b))
        items.append(b.value)
    return items


def wait_combo(hwnd, mincount=1, timeout=8.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        items = combo_items(hwnd)
        if len(items) >= mincount:
            return items
        time.sleep(0.25)
    return combo_items(hwnd)


with open(INI, "w", encoding="utf-8", newline="\n") as f:
    # caller=3149 不在列表里: 每次解析都要走"保留手输主叫"的分支
    # (这条分支曾经用 malloc(字符数) 装 wchar_t 文本, 在几百个分机时踩坏堆)
    f.write(f"host=127.0.0.1\nport=1883\ntoken={TOKEN}\ncaller=3149\n")

proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE),
                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def _win_pid(h):
    _pd = wintypes.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(_pd))
    return _pd.value


hwnd = 0
t0 = time.time()
while time.time() - t0 < 20:
    _h = u.FindWindowW(None, TITLE)
    if _h and u.GetDlgItem(_h, 1014) and _win_pid(_h) == proc.pid:
        hwnd = _h
        break
    time.sleep(0.2)
print("hwnd=0x%x" % hwnd, flush=True)

items = wait_combo(hwnd, 1, 20)
check("标准格式 extlist", len(items) > 0, "共%d个: %s" % (len(items), items[:4]))

cl = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="ext-test")
cl.connect("127.0.0.1", 1883, 30)
cl.loop_start()
time.sleep(0.4)

# 变体 B: 没有 "extlist" 包裹, 数组藏在别的字段里
payload_b = {
    "status": "Success", "message": "success", "request_id": "b1",
    "extensions": [
        {"number": "3301", "username": "A", "type": "SIP", "status": "Idle"},
        {"number": "3302", "username": "B", "type": "SIP", "status": "Idle"},
    ],
}
cl.publish(RESP, json.dumps(payload_b, ensure_ascii=False, indent=2))
time.sleep(2)
items = combo_items(hwnd)
check("无 extlist 包裹时兜底解析", len(items) >= 2,
      "共%d个: %s" % (len(items), items[:4]))

# 变体 C: extlist 里混入不带 number 的项
payload_c = {
    "status": "Success", "message": "success", "request_id": "c1",
    "extlist": [
        {"type": "SIP", "status": "Idle"},
        {"number": "3401", "username": "C"},
        {"number": "3401", "username": "C"},   # 重复项
        {"number": "3402", "username": ""},
    ],
}
cl.publish(RESP, json.dumps(payload_c, ensure_ascii=False, indent=2))
time.sleep(2)
items = combo_items(hwnd)
check("跳过无效项并去重", len(items) == 2 and items[0].startswith("3401"),
      "共%d个: %s" % (len(items), items))

# 变体 D: 真实 PBX 的大列表 (450 个, 字段名 fullname/ipaddr, 多行格式化 JSON)
# 用于回归 "只显示 200 个" 的问题
big = []
for i in range(450):
    n = str(1000 + i)
    e = {"fullname": "", "type": "SIP", "status": "Unavailable", "number": n}
    if i == 0:
        e["status"] = "Idle"
        e["ipaddr"] = "sip:%s@192.168.8.171:5060" % n
    big.append(e)
payload_d = {"status": "Success", "message": "success", "request_id": "d1", "extlist": big}
_body = json.dumps(payload_d, ensure_ascii=False, indent=2)
print("变体 D 报文长度: %d 字节" % len(_body.encode("utf-8")), flush=True)
cl.publish(RESP, _body)
time.sleep(4)
items = combo_items(hwnd)
check("大列表不被 200 截断", len(items) >= 450,
      "共%d个, 末位=%s" % (len(items), items[-1] if items else "-"))
h_log = u.GetDlgItem(hwnd, IDC_LOG)
_n = u.SendMessageW(h_log, 0x000E, 0, 0)
_b = ctypes.create_unicode_buffer(_n + 1)
u.SendMessageW(h_log, 0x000D, _n + 1, ctypes.addressof(_b))
check("日志报出分机总数", "共 450 个分机" in _b.value)
check("大列表后进程仍存活 (堆未损坏)", proc.poll() is None, "rc=%s" % proc.poll())
_ph = u.GetDlgItem(hwnd, IDC_CALLER)
_pn = u.SendMessageW(_ph, 0x000E, 0, 0)
_pb = ctypes.create_unicode_buffer(_pn + 1)
u.SendMessageW(_ph, 0x000D, _pn + 1, ctypes.addressof(_pb))
caller_txt = _pb
check("大列表后主叫仍是手输的 3149", caller_txt.value.strip() == "3149",
      "主叫框=[%s]" % caller_txt.value)

cl.loop_stop()
h = u.GetDlgItem(hwnd, IDC_LOG)
n = u.SendMessageW(h, 0x000E, 0, 0)
b = ctypes.create_unicode_buffer(n + 1)
u.SendMessageW(h, 0x000D, n + 1, ctypes.addressof(b))
print("\n".join(b.value.splitlines()[-8:]), flush=True)

u.PostMessageW(hwnd, 0x0010, 0, 0)
time.sleep(1)
try:
    proc.terminate()
except Exception:
    pass
bad = [r for r in RESULTS if not r[1]]
print("\n== 结果: %d/%d ==" % (len(RESULTS) - len(bad), len(RESULTS)), flush=True)
sys.exit(0 if not bad else 1)
