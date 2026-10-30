#!/usr/bin/env python
# -*- coding: utf-8 -*-
# E2E: "自动接通"勾选框 —— 默认勾选 / 记住到 ini / 重启恢复 / 影响拨号报文
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
u.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
u.SetWindowTextW.restype = wintypes.BOOL
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]

IDC_CALLER, IDC_CALLEE, IDC_AUTO = 1009, 1010, 1011
IDC_DIAL, IDC_LOG = 1012, 1014
WM_GETTEXT, WM_GETTEXTLENGTH = 0x000D, 0x000E
BM_CLICK, BM_GETCHECK, BM_SETCHECK = 0x00F5, 0x00F0, 0x00F1
CB_SETCURSEL = 0x014E
CB_SHOWDROPDOWN = 0x014F
BST_UNCHECKED, BST_CHECKED = 0, 1
IDC_HISTBTN, IDC_HISTLIST = 1018, 1019
WM_COMMAND, LB_SETCURSEL = 0x0111, 0x0186
LBN_SELCHANGE = 1


def hist_pick(hwnd, idx):
    """选中被叫历史下拉框的第 idx 条"""
    u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLEE), CB_SETCURSEL, idx, 0)
    time.sleep(0.25)


# 测试前先清掉可能残留的实例
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
        u.PostMessageW(hwnd, 0x0010, 0, 0)
        time.sleep(0.6)
    try:
        proc.terminate()
    except Exception:
        pass
    time.sleep(0.6)


def check_state(hwnd):
    return u.SendMessageW(u.GetDlgItem(hwnd, IDC_AUTO), BM_GETCHECK, 0, 0)


def ini_has(text, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(INI, encoding="utf-8") as f:
                if text in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False


print("== E2E 自动接通 (autoanswer) ==", flush=True)

# 场景 1: ini 里没有 autoanswer 这个键 -> 应该默认勾选, 并且自动补写 autoanswer=1
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\ncallee_history=8002|8003\n")

proc, hwnd = start_and_wait()
print("hwnd=0x%x" % hwnd, flush=True)
check("窗口出现", hwnd != 0)
check("无该键时默认勾选", check_state(hwnd) == BST_CHECKED, "state=%d" % check_state(hwnd))
check("ini 自动补写 autoanswer=1", ini_has("autoanswer=1"))

check("连接", wait_log(hwnd, "MQTT 已连接", 25))
check("分机列表", wait_log(hwnd, "分机列表已刷新", 15))

# 勾选状态下拨号 -> 报文里 autoanswer 应为 yes
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLER), CB_SETCURSEL, 0, 0)
hist_pick(hwnd, 0)
time.sleep(0.3)
u.PostMessageW(u.GetDlgItem(hwnd, IDC_DIAL), BM_CLICK, 0, 0)
check("勾选时报文 autoanswer=yes", wait_log(hwnd, '"autoanswer":"yes"', 10))

# 场景 2: 取消勾选 -> 立即写回 ini
u.PostMessageW(u.GetDlgItem(hwnd, IDC_AUTO), BM_CLICK, 0, 0)
time.sleep(0.5)
check("取消勾选后状态为未勾选", check_state(hwnd) == BST_UNCHECKED,
      "state=%d" % check_state(hwnd))
check("取消勾选后 ini 写 0", ini_has("autoanswer=0"))

# 再拨一次 -> 报文里应是 no
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLER), CB_SETCURSEL, 0, 0)
hist_pick(hwnd, 1)
time.sleep(0.3)
u.PostMessageW(u.GetDlgItem(hwnd, IDC_DIAL), BM_CLICK, 0, 0)
check("未勾选时报文 autoanswer=no", wait_log(hwnd, '"autoanswer":"no"', 10))

stop(proc, hwnd)

# 场景 3: 重启 -> 应按 ini 恢复为未勾选
proc, hwnd = start_and_wait()
print("restart hwnd=0x%x" % hwnd, flush=True)
check("重启后恢复未勾选", hwnd != 0 and check_state(hwnd) == BST_UNCHECKED,
      "state=%d" % (check_state(hwnd) if hwnd else -1))
stop(proc, hwnd)

# 场景 4: ini 里写 autoanswer=1 -> 启动勾选
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\nautoanswer=1\n")
proc, hwnd = start_and_wait()
check("ini=1 时启动勾选", hwnd != 0 and check_state(hwnd) == BST_CHECKED,
      "state=%d" % (check_state(hwnd) if hwnd else -1))
stop(proc, hwnd)

# 场景 5: ini 里写 0 -> 启动不勾选; 也支持 no
with open(INI, "w", encoding="utf-8", newline="\n") as f:
    f.write("host=127.0.0.1\nport=1883\ntoken=testtoken001\nautoanswer=no\n")
proc, hwnd = start_and_wait()
check("ini=no 时启动不勾选", hwnd != 0 and check_state(hwnd) == BST_UNCHECKED,
      "state=%d" % (check_state(hwnd) if hwnd else -1))
stop(proc, hwnd)

bad = [r for r in RESULTS if not r[1]]
print("\n== 结果: %d/%d ==" % (len(RESULTS) - len(bad), len(RESULTS)), flush=True)
sys.exit(0 if not bad else 1)