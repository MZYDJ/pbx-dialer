#!/usr/bin/env python
# 测试 v1.2 的 [诊断] / [复制日志] 按钮
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
TITLE = "PBX 拨号器"

u = ctypes.windll.user32
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
u.PostMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]

IDC_DIAG, IDC_COPYLOG, IDC_REFRESH, IDC_LOG = 1015, 1016, 1017, 1014
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
    b = ctypes.create_unicode_buffer(n + 1)
    u.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.addressof(b))
    return b.value


def wait_log(hwnd, needle, timeout=15.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in get_log(hwnd):
            return True
        time.sleep(0.2)
    return False


print("== v1.2 诊断按钮测试 ==", flush=True)
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\nshow_diag=1\n"
            "test_topic=device/%s/api/v1.0/command/system\n"
            'test_payload={"cmd":"deviceinfo","request_id":"diag01"}\n')

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

check("连接", wait_log(hwnd, "MQTT 已连接", 25))

# [诊断] 按钮
u.PostMessageW(u.GetDlgItem(hwnd, IDC_DIAG), BM_CLICK, 0, 0)
check("诊断按钮发送", wait_log(hwnd, "诊断: 已发送到", 8))
check("诊断收到 deviceinfo 响应", wait_log(hwnd, "设备信息", 10))

# [刷新分机] 按钮
before = get_log(hwnd).count("已请求分机列表")
u.PostMessageW(u.GetDlgItem(hwnd, IDC_REFRESH), BM_CLICK, 0, 0)
time.sleep(1.5)
check("刷新分机按钮", get_log(hwnd).count("已请求分机列表") > before)

# [复制日志] 按钮
u.PostMessageW(u.GetDlgItem(hwnd, IDC_COPYLOG), BM_CLICK, 0, 0)
ok = wait_log(hwnd, "已复制到剪贴板", 8) or wait_log(hwnd, "剪贴板被其它程序占用", 8)
check("复制日志按钮", ok)

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
