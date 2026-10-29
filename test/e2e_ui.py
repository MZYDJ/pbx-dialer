#!/usr/bin/env python
# -*- coding: utf-8 -*-
# E2E: 界面精简 + 快捷操作
#   1. show_diag 默认 0 -> [诊断]/[复制日志] 不创建, 并自动补写 show_diag=0
#   2. show_diag=1 -> 两个按钮出现
#   3. 被叫框回车 = 拨号
#   4. 被叫为空时在主叫框回车 -> 只跳焦点, 不拨号
#   5. 历史列表: 单击填入 / Esc 关闭
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
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]

IDC_CALLER, IDC_CALLEE, IDC_LOG = 1009, 1010, 1014
IDC_DIAL, IDC_DIAG, IDC_COPYLOG, IDC_REFRESH = 1012, 1015, 1016, 1017
IDC_HISTBTN, IDC_HISTLIST = 1018, 1019
WM_GETTEXT, WM_GETTEXTLENGTH, BM_CLICK = 0x000D, 0x000E, 0x00F5
WM_COMMAND, WM_KEYDOWN = 0x0111, 0x0100
CB_SETCURSEL = 0x014E
LB_SETCURSEL, LB_GETCOUNT = 0x0186, 0x018B
LBN_SELCHANGE = 1
VK_RETURN, VK_ESCAPE = 0x0D, 0x1B
WM_CLOSE = 0x0010

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


def text_of(hwnd, cid):
    h = u.GetDlgItem(hwnd, cid)
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


def win_pid(h):
    pd = wintypes.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(pd))
    return pd.value


def start_and_wait(timeout=20.0):
    proc = subprocess.Popen([EXE], cwd=os.path.dirname(EXE),
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = u.FindWindowW(None, TITLE)
        if h and u.GetDlgItem(h, IDC_LOG) and win_pid(h) == proc.pid:
            return proc, h
        time.sleep(0.2)
    return proc, 0


def stop(proc, hwnd):
    if hwnd:
        u.PostMessageW(hwnd, WM_CLOSE, 0, 0)
        time.sleep(0.6)
    try:
        proc.terminate()
    except Exception:
        pass
    time.sleep(0.6)


def hist_open(hwnd):
    """点 ▼ 打开被叫历史弹出列表 (WS_POPUP 不是子窗口, 按标题找)"""
    u.PostMessageW(u.GetDlgItem(hwnd, IDC_HISTBTN), BM_CLICK, 0, 0)
    time.sleep(0.4)
    return u.FindWindowW(None, HIST_TITLE)


def hist_pick(hwnd, idx):
    lst = hist_open(hwnd)
    u.SendMessageW(lst, LB_SETCURSEL, idx, 0)
    u.SendMessageW(hwnd, WM_COMMAND, (LBN_SELCHANGE << 16) | IDC_HISTLIST, lst)
    time.sleep(0.3)


def press_enter(hwnd, cid):
    u.SendMessageW(u.GetDlgItem(hwnd, cid), WM_KEYDOWN, VK_RETURN, 0)
    time.sleep(0.5)


print("== E2E 界面精简 / 回车拨号 / 历史双击 ==", flush=True)

# ---------- 场景 1: 默认不显示诊断类按钮 ----------
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\ncallee_history=8002|8003\n")

proc, hwnd = start_and_wait()
print("hwnd=0x%x" % hwnd, flush=True)
check("窗口出现", hwnd != 0)
check("默认无 [诊断] 按钮", not u.GetDlgItem(hwnd, IDC_DIAG))
check("默认无 [复制日志] 按钮", not u.GetDlgItem(hwnd, IDC_COPYLOG))
check("拨号按钮还在", bool(u.GetDlgItem(hwnd, IDC_DIAL)))
check("刷新分机按钮还在", bool(u.GetDlgItem(hwnd, IDC_REFRESH)))

time.sleep(1.0)
with open(INI, encoding="utf-8") as f:
    _ini = f.read()
check("ini 自动补写 show_diag=0", "show_diag=0" in _ini,
      [x for x in _ini.splitlines() if x.startswith("show_diag")])

check("连接", wait_log(hwnd, "MQTT 已连接", 25))
check("分机列表", wait_log(hwnd, "分机列表已刷新", 15))

# ---------- 场景 2: 被叫框回车 = 拨号 ----------
hist_pick(hwnd, 0)
check("单击历史填入 8002", text_of(hwnd, IDC_CALLEE).strip() == "8002",
      "被叫框=[%s]" % text_of(hwnd, IDC_CALLEE))
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLER), CB_SETCURSEL, 0, 0)   # 先选主叫
time.sleep(0.3)
press_enter(hwnd, IDC_CALLEE)
check("被叫框回车拨号", wait_log(hwnd, "拨号命令已发送", 8))

# ---------- 场景 3: 被叫为空时主叫框回车 -> 不拨号 ----------
with open(INI, encoding="utf-8") as f:
    before = f.read()
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLEE), 0x000C, 0, 0)   # WM_SETTEXT 空
time.sleep(0.3)
_log_before = get_log(hwnd)
press_enter(hwnd, IDC_CALLER)
time.sleep(1.0)
_log_after = get_log(hwnd)
check("被叫为空时主叫回车不拨号",
      _log_after.count("拨号命令已发送") == _log_before.count("拨号命令已发送"),
      "before=%d after=%d" % (_log_before.count("拨号命令已发送"),
                              _log_after.count("拨号命令已发送")))
check("也不报'请输入被叫号码'", "请输入被叫号码" not in _log_after)

# ---------- 场景 4: 历史列表 Esc 关闭 ----------
lst = hist_open(hwnd)
check("历史列表弹出", bool(lst) and u.IsWindowVisible(lst))
check("列表条数=2", u.SendMessageW(lst, LB_GETCOUNT, 0, 0) == 2,
      "count=%d" % u.SendMessageW(lst, LB_GETCOUNT, 0, 0))
u.SendMessageW(lst, WM_KEYDOWN, VK_ESCAPE, 0)
time.sleep(0.4)
check("Esc 关闭历史列表", not u.IsWindowVisible(lst))

stop(proc, hwnd)

# ---------- 场景 5: show_diag=1 -> 按钮出现 ----------
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\nshow_diag=1\n")

proc, hwnd = start_and_wait()
check("show_diag=1 时窗口出现", hwnd != 0)
check("show_diag=1 显示 [诊断]", bool(u.GetDlgItem(hwnd, IDC_DIAG)))
check("show_diag=1 显示 [复制日志]", bool(u.GetDlgItem(hwnd, IDC_COPYLOG)))
check("show_diag=1 刷新分机仍在", bool(u.GetDlgItem(hwnd, IDC_REFRESH)))
stop(proc, hwnd)

print("\n== 结果 ==", flush=True)
bad = [n for n, ok in RESULTS if not ok]
print("通过 %d / %d" % (len(RESULTS) - len(bad), len(RESULTS)), flush=True)
if bad:
    print("失败: %s" % ", ".join(bad), flush=True)
sys.exit(1 if bad else 0)