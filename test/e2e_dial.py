#!/usr/bin/env python
# E2E 拨号: ini 自动连接 -> 选主叫 -> 填被叫 -> 拨号 -> 响应 -> 挂机
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
HIST_TITLE = "被叫历史"

u = ctypes.windll.user32
u.FindWindowW.restype = wintypes.HWND
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
u.PostMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
u.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
u.SetWindowTextW.restype = wintypes.BOOL

IDC_CALLER, IDC_CALLEE = 1009, 1010
IDC_CONNECT, IDC_DIAL, IDC_HANGUP, IDC_LOG = 1006, 1012, 1013, 1014
IDC_HISTBTN, IDC_HISTLIST = 1018, 1019
WM_GETTEXT, WM_GETTEXTLENGTH, BM_CLICK = 0x000D, 0x000E, 0x00F5
CB_SETCURSEL = 0x014E
WM_COMMAND, LB_SETCURSEL = 0x0111, 0x0186
LBN_SELCHANGE = 1


def hist_pick(hwnd, idx):
    """打开历史列表并选中第 idx 条 (等价于用户点一下)"""
    u.PostMessageW(u.GetDlgItem(hwnd, IDC_HISTBTN), BM_CLICK, 0, 0)
    time.sleep(0.4)
    lst = u.FindWindowW(None, HIST_TITLE)
    u.SendMessageW(lst, LB_SETCURSEL, idx, 0)
    u.SendMessageW(hwnd, WM_COMMAND, (LBN_SELCHANGE << 16) | IDC_HISTLIST, lst)
    time.sleep(0.2)

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


print("== E2E 拨号流程 ==", flush=True)
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\ncallee_history=8002|8003\n")

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
check("分机列表", wait_log(hwnd, "分机列表已刷新", 15))

# 主叫: 点第一个分机
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLER), CB_SETCURSEL, 0, 0)
# 被叫: 从历史列表里选第 1 条 (8002)
hist_pick(hwnd, 0)
time.sleep(0.3)
u.PostMessageW(u.GetDlgItem(hwnd, IDC_DIAL), BM_CLICK, 0, 0)
print("已点击拨号", flush=True)

check("拨号命令已发送", wait_log(hwnd, "拨号命令已发送", 8))
check("dial 响应 Success", wait_log(hwnd, "响应: status=Success", 12))
check("响应含 callid", wait_log(hwnd, "callid", 5))

time.sleep(0.6)
_ini = open(INI, encoding="utf-8").read()
check("ini 记住主叫号码", "caller=8001" in _ini,
      [l for l in _ini.splitlines() if l.startswith("caller")])
check("ini 记住被叫历史", "callee_history=8002|8003" in _ini,
      [l for l in _ini.splitlines() if l.startswith("callee")])

u.PostMessageW(u.GetDlgItem(hwnd, IDC_HANGUP), BM_CLICK, 0, 0)
print("已点击挂机", flush=True)
check("挂机命令已发送", wait_log(hwnd, "挂机命令已发送", 8))
check("挂机响应", wait_log(hwnd, "Channel Hungup", 12))

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
