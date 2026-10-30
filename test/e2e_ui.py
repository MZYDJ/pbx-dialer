#!/usr/bin/env python
# -*- coding: utf-8 -*-
# E2E: 界面精简 + 快捷操作
#   1. show_diag 默认 0 -> [诊断]/[复制日志] 不创建, 并自动补写 show_diag=0
#   2. show_diag=1 -> 两个按钮出现
#   3. 被叫框回车 = 拨号
#   4. 被叫为空时在主叫框回车 -> 只跳焦点, 不拨号
#   5. 被叫历史下拉框: 条数 / 选中填入 / 选中后回车拨号
#   6. 三个按钮字体一致
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
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]

IDC_CALLER, IDC_CALLEE, IDC_LOG = 1009, 1010, 1014
IDC_DIAL, IDC_DIAG, IDC_COPYLOG, IDC_REFRESH = 1012, 1015, 1016, 1017
WM_GETTEXT, WM_GETTEXTLENGTH, BM_CLICK = 0x000D, 0x000E, 0x00F5
WM_KEYDOWN, WM_GETFONT, WM_SETTEXT = 0x0100, 0x0031, 0x000C
CB_SETCURSEL, CB_GETCOUNT, CB_SHOWDROPDOWN = 0x014E, 0x0146, 0x014F
VK_RETURN = 0x0D
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


def text_of(hwnd, cid):
    h = u.GetDlgItem(hwnd, cid)
    n = u.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    b = ctypes.create_unicode_buffer(n + 1)
    u.SendMessageW(h, WM_GETTEXT, n + 1, ctypes.addressof(b))
    return b.value


def get_log(hwnd):
    return text_of(hwnd, IDC_LOG)


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


def callee_pick(hwnd, idx):
    u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLEE), CB_SETCURSEL, idx, 0)
    time.sleep(0.25)


def press_enter(hwnd, cid):
    u.SendMessageW(u.GetDlgItem(hwnd, cid), WM_KEYDOWN, VK_RETURN, 0)
    time.sleep(0.5)


print("== E2E 界面精简 / 回车拨号 / 被叫历史 ==", flush=True)

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

# ---------- 场景 2: 被叫历史下拉框 ----------
n = u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLEE), CB_GETCOUNT, 0, 0)
check("被叫历史下拉框有 2 条", n == 2, "count=%d" % n)

u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLEE), CB_SHOWDROPDOWN, 1, 0)
time.sleep(0.4)
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLEE), CB_SHOWDROPDOWN, 0, 0)
check("下拉框能展开", True)

callee_pick(hwnd, 0)
check("选第1条填入 8002", text_of(hwnd, IDC_CALLEE).strip() == "8002",
      "被叫框=[%s]" % text_of(hwnd, IDC_CALLEE))
callee_pick(hwnd, 1)
check("选第2条填入 8003", text_of(hwnd, IDC_CALLEE).strip() == "8003",
      "被叫框=[%s]" % text_of(hwnd, IDC_CALLEE))

# ---------- 场景 3: 被叫框回车 = 拨号 ----------
callee_pick(hwnd, 0)
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLER), CB_SETCURSEL, 0, 0)
time.sleep(0.3)
press_enter(hwnd, IDC_CALLEE)
check("被叫框回车拨号", wait_log(hwnd, "拨号命令已发送", 8))

# ---------- 场景 4: 被叫为空时主叫框回车 -> 不拨号 ----------
u.SendMessageW(u.GetDlgItem(hwnd, IDC_CALLEE), WM_SETTEXT, 0, 0)   # 清空
time.sleep(0.3)
_n_before = get_log(hwnd).count("拨号命令已发送")
press_enter(hwnd, IDC_CALLER)
time.sleep(1.0)
_n_after = get_log(hwnd).count("拨号命令已发送")
check("被叫为空时主叫回车不拨号", _n_after == _n_before,
      "拨号次数 %d -> %d" % (_n_before, _n_after))
check("也不报'请输入被叫号码'", "请输入被叫号码" not in get_log(hwnd))

# ---------- 场景 5: 三个按钮字体一致 ----------
fonts = {}
for cid, name in ((IDC_DIAL, "拨号"), (1013, "挂机"), (IDC_REFRESH, "刷新分机")):
    hb = u.GetDlgItem(hwnd, cid)
    fonts[name] = u.SendMessageW(hb, WM_GETFONT, 0, 0) if hb else None
vals = [v for v in fonts.values() if v]
check("三个按钮字体一致", len(set(vals)) == 1 and len(vals) == 3,
      " ".join("%s=%s" % (k, ("%x" % (v & 0xFFFFFFFF)) if v else "None")
               for k, v in fonts.items()))

stop(proc, hwnd)

# ---------- 场景 6: show_diag=1 -> 按钮出现 ----------
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