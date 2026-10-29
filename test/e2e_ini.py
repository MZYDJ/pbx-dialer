#!/usr/bin/env python
# E2E: 通过 ini 让软件自己连接, 验证 MQTT 连接/订阅/分机列表/拨号校验
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "pbxdialer.exe")
INI = os.path.join(os.path.dirname(EXE), "pbxdialer.ini")
SHOT = os.path.join(HERE, "screenshot.png")
TITLE = "PBX 拨号器"

u = ctypes.windll.user32
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
u.PostMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]

IDC_CONNECT, IDC_DIAL, IDC_HANGUP, IDC_LOOG = 1006, 1012, 1013, 1014
IDC_LOG = 1014
WM_GETTEXT, WM_GETTEXTLENGTH, BM_CLICK = 0x000D, 0x000E, 0x00F5

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


def get_log(hwnd):
    h = u.GetDlgItem(hwnd, IDC_LOG)
    n = u.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    buf = ctypes.create_unicode_buffer(n + 1)
    u.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.addressof(buf))
    return buf.value


def wait_log(hwnd, needle, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in get_log(hwnd):
            return True
        time.sleep(0.25)
    return False


print("== E2E (ini 自动连接) ==", flush=True)

# 写入配置, 让软件自己读取 (模拟用户填好后保存)
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\n")
print("已写入 ini:", INI, flush=True)

proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE),
                        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)

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
if not hwnd:
    print("FAIL: 窗口未出现", flush=True)
    sys.exit(1)
print("窗口就绪 hwnd=0x%x" % hwnd, flush=True)

check("自动连接 MQTT", wait_log(hwnd, "MQTT 已连接", 25))
check("订阅 response/event", wait_log(hwnd, "已订阅", 5))
check("分机列表刷新", wait_log(hwnd, "分机列表已刷新", 15))

# 拨号按钮路径 (被叫为空 -> 应给出明确提示, 证明按钮与校验链路正常)
u.PostMessageW(u.GetDlgItem(hwnd, IDC_DIAL), BM_CLICK, 0, 0)
time.sleep(1.5)
check("拨号按钮响应", wait_log(hwnd, "拨号失败", 8))

u.PostMessageW(u.GetDlgItem(hwnd, IDC_HANGUP), BM_CLICK, 0, 0)
time.sleep(1.5)
lg = get_log(hwnd)
check("挂机按钮响应", ("挂机" in lg), "")

try:
    from PIL import ImageGrab
    ImageGrab.grab().save(SHOT)
    print("截图:", SHOT, flush=True)
except Exception as e:
    print("截图失败:", e, flush=True)

print("\n== 日志 ==", flush=True)
print(get_log(hwnd), flush=True)

u.PostMessageW(hwnd, 0x0010, 0, 0)
time.sleep(1)
try:
    proc.terminate()
except Exception:
    pass

bad = [r for r in RESULTS if not r[1]]
print("\n== 结果: %d/%d ==" % (len(RESULTS) - len(bad), len(RESULTS)), flush=True)
sys.exit(0 if not bad else 1)
