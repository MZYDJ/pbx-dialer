#!/usr/bin/env python
# E2E: 记住主叫 / 被叫不自动回填 / 被叫历史下拉框 / 手输主叫不被覆盖
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
u.GetDlgItem.restype = wintypes.HWND
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]
u.PostMessageW.argtypes = [wintypes.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_size_t]

IDC_CALLER, IDC_CALLEE, IDC_LOG = 1009, 1010, 1014
IDC_DIAL, IDC_REFRESH = 1012, 1017
WM_GETTEXT, WM_GETTEXTLENGTH, BM_CLICK, CB_SETCURSEL = 0x000D, 0x000E, 0x00F5, 0x014E
CB_GETCOUNT, CB_GETLBTEXT = 0x0146, 0x0148
CB_SHOWDROPDOWN = 0x014F


def hist_open(hwnd):
    """展开被叫历史下拉框, 返回下拉框句柄"""
    h = u.GetDlgItem(hwnd, IDC_CALLEE)
    u.SendMessageW(h, CB_SHOWDROPDOWN, 1, 0)
    time.sleep(0.25)
    return h


def hist_click(hwnd, idx, notify=None):
    """选中第 idx 条 (下拉框选中即填入被叫框)"""
    h = u.GetDlgItem(hwnd, IDC_CALLEE)
    u.SendMessageW(h, CB_SETCURSEL, idx, 0)
    time.sleep(0.3)
    return h


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


def text_of(hwnd, cid):
    h = u.GetDlgItem(hwnd, cid)
    n = u.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    b = ctypes.create_unicode_buffer(n + 1)
    u.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.addressof(b))
    return b.value


def combo_items(hwnd, cid):
    h = u.GetDlgItem(hwnd, cid)
    n = u.SendMessageW(h, CB_GETCOUNT, 0, 0)
    out = []
    for i in range(n):
        b = ctypes.create_unicode_buffer(256)
        u.SendMessageW(h, CB_GETLBTEXT, i, ctypes.addressof(b))
        out.append(b.value)
    return out


def get_log(hwnd):
    return text_of(hwnd, IDC_LOG)


def wait_log(hwnd, needle, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in get_log(hwnd):
            return True
        time.sleep(0.2)
    return False


print("== E2E 主叫记忆 / 被叫历史 ==", flush=True)
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\n"
            "caller=3149\ncallee_history=8002|8003\n")

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
print("hwnd=0x%x" % hwnd, flush=True)

check("连接", wait_log(hwnd, "MQTT 已连接", 25))
check("分机列表", wait_log(hwnd, "分机列表已刷新", 15))

time.sleep(0.5)
caller = text_of(hwnd, IDC_CALLER)
check("启动后回填主叫 3149", caller.strip() == "3149", "主叫框=[%s]" % caller)
callee_txt = text_of(hwnd, IDC_CALLEE)
check("被叫不自动回填", callee_txt.strip() == "", "被叫框=[%s]" % callee_txt)

lst = hist_open(hwnd)
check("被叫下拉框展开", lst != 0 and u.SendMessageW(lst, CB_GETCOUNT, 0, 0) == 2)
check("历史列表有 2 条", u.SendMessageW(lst, CB_GETCOUNT, 0, 0) == 2,
      "count=%d" % u.SendMessageW(lst, CB_GETCOUNT, 0, 0))
hist_click(hwnd, 0)
check("单击历史第1条填入 8002", text_of(hwnd, IDC_CALLEE).strip() == "8002",
      "被叫框=[%s]" % text_of(hwnd, IDC_CALLEE))
hist_click(hwnd, 1)
check("单击历史第2条填入 8003", text_of(hwnd, IDC_CALLEE).strip() == "8003",
      "被叫框=[%s]" % text_of(hwnd, IDC_CALLEE))

# 手动刷新一次分机列表, 主叫框里手输的 3149 不能被覆盖成列表首项
u.PostMessageW(u.GetDlgItem(hwnd, IDC_REFRESH), BM_CLICK, 0, 0)
time.sleep(2.5)
caller = text_of(hwnd, IDC_CALLER)
check("刷新后主叫不被覆盖", caller.strip() == "3149", "主叫框=[%s]" % caller)

# 双击历史第 2 条 (8003) -> 直接拨号, 并验证历史被提到最前
hist_click(hwnd, 1)
u.PostMessageW(u.GetDlgItem(hwnd, IDC_DIAL), BM_CLICK, 0, 0)
check("选历史后点拨号成功", wait_log(hwnd, "拨号命令已发送", 8))

time.sleep(1.0)
_ini = open(INI, encoding="utf-8").read()
check("ini 记住主叫", "caller=3149" in _ini,
      [x for x in _ini.splitlines() if x.startswith("caller")])
check("新被叫提到历史最前", "callee_history=8003|8002" in _ini,
      [x for x in _ini.splitlines() if x.startswith("callee")])
lst = hist_open(hwnd)
check("下拉框历史同步为 2 条", u.SendMessageW(lst, CB_GETCOUNT, 0, 0) == 2,
      "count=%d" % u.SendMessageW(lst, CB_GETCOUNT, 0, 0))
check("被叫框保留刚拨号码", text_of(hwnd, IDC_CALLEE).strip() == "8003",
      "被叫框=[%s]" % text_of(hwnd, IDC_CALLEE))

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
