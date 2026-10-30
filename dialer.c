/*
 * PBX 拨号器 (pbxdialer)
 * ----------------------
 * 针对 The Smart PBX Manager MQTT API (V1.0.x) 的最小拨号工具。
 *
 * 功能:
 *   - 连接 PBX 对接的 MQTT Broker (host/port/user/pass/token)
 *   - 自动订阅 response / event 主题, 自动拉取分机列表
 *   - 选择主叫分机 + 输入被叫号码 -> 发送 dial 命令 (点击外呼)
 *   - 挂机 (hangup, 使用最近一次 dial 返回的 callid)
 *   - 状态日志: 连接状态 / 命令响应 / 来电事件等
 *
 * 实现:
 *   - 单文件 C, Win32 GUI (宽字符), 内置 MQTT 3.1.1 客户端 (QoS0)
 *   - 编译: gcc -O2 -municode -mwindows dialer.c -o pbxdialer.exe -lws2_32
 *   - 运行: 双击 exe, 无任何运行时依赖
 */

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define APP_TITLE   L"PBX 拨号器"
#define CLASS_NAME  L"PBXDialerWnd"
#define CONFIG_NAME L"pbxdialer.ini"

#define WM_APP_LOG    (WM_APP + 1)  /* lParam: wchar_t* (heap) 日志行 */
#define WM_APP_STATUS (WM_APP + 2)  /* lParam: wchar_t* (heap) 状态文本 */
#define WM_APP_PKT    (WM_APP + 3)  /* lParam: char* (heap) "topic\x01payload" */
#define WM_APP_EXTWAIT (WM_APP + 4) /* 开始计时: 等待分机列表响应 */
#define WM_APP_DIAG   (WM_APP + 5)  /* lParam: wchar_t* 诊断提示 */

#define IDT_EXTWAIT   1
#define IDT_DIALWAIT  2

/* 控件 ID */
#define IDC_HOST        1001
#define IDC_PORT        1002
#define IDC_USER        1003
#define IDC_PASS        1004
#define IDC_TOKEN       1005
#define IDC_CONNECT     1006
#define IDC_DISCONNECT  1007
#define IDC_STATUS      1008
#define IDC_CALLER      1009
#define IDC_CALLEE      1010
#define IDC_AUTOANSWER  1011
#define IDC_DIAL        1012
#define IDC_HANGUP      1013
#define IDC_LOG         1014
#define IDC_DIAG        1015
#define IDC_COPYLOG     1016
#define IDC_REFRESH     1017

/* 容量限制 */
#define MAX_EXT   2000     /* 主叫下拉框最多容纳的分机数 (去重后) */
#define MAX_HIST    12     /* 被叫号码历史条数 */

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char host[128];
    int  port;
    char user[96];
    char pass[96];
    char token[128];
    char caller[96];       /* 上次使用的主叫分机 (下次启动自动回填) */
    /* 被叫号码历史 (最近使用的在前), 只用于下拉框候选, 不自动回填 */
    char hist[MAX_HIST][40];
    int  hist_n;
    /* --- 以下为协议微调项, 直接在 ini 里改即可, 无需重新编译 --- */
    char topic_system[192];/* 命令主题模板, %s 代表 token */
    char topic_call[192];
    char sub_extra[256];   /* 追加订阅主题, 用 ; 分隔 */
    char dial_extra[256];  /* 拨号时追加的 JSON 片段, 如 "dialpermission":"3149" */
    char test_topic[192];  /* [诊断] 按钮发送的主题 (空=用 system 主题) */
    char test_payload[768];/* [诊断] 按钮发送的内容 (空=deviceinfo 请求) */
    int  cmd_qos;          /* 命令 QoS: 0 或 1 (默认 1) */
    int  autoanswer;       /* 拨号时是否自动接通 (autoanswer=yes), 默认 1=勾选 */
    int  show_diag;        /* 是否显示 [诊断]/[复制日志] 按钮, 默认 0=不显示 */
} Settings;

static HINSTANCE g_hInst;
static HWND  g_hwnd, g_hHost, g_hPort, g_hUser, g_hPass, g_hToken;
static HWND  g_hConnect, g_hDisconnect, g_hStatus;
static HWND  g_hCaller, g_hCallee, g_hAuto, g_hDial, g_hHangup, g_hLog;
static HWND  g_hRefresh, g_hDiag, g_hCopyLog;   /* 右侧三个按钮 (字体要单独设) */
static WNDPROC g_oldCalleeProc;         /* 被叫框/主叫框的原窗口过程 (回车拨号用) */
static WNDPROC g_oldCallerEditProc;
static HFONT g_font, g_fontLog;
static float g_sf = 1.0f;              /* DPI 缩放 */

static Settings  G;
static volatile LONG g_stop = 0;       /* 线程退出 */
static volatile LONG g_run = 0;        /* 应保持连接 */
static volatile LONG g_force_reconnect = 0;

static SOCKET g_sock = INVALID_SOCKET;
static int    g_connected = 0;
static DWORD  g_last_act = 0;          /* 最近一次收发时间 (tick) */
static short  g_seq = 1;               /* MQTT 包序号 */

static CRITICAL_SECTION g_cs;          /* 保护 g_box */

/* 待发命令队列 (UI 线程写入, 工作线程发送) */
typedef struct { char topic[160]; char payload[4096]; } Pending;
static Pending g_box[16];
static int     g_box_n = 0;

/* 最近一次呼叫标识 (用于挂机) */
static char g_last_callid[96]     = {0};
static char g_last_channelid[96]  = {0};

/* MQTT 接收缓冲 (400+ 分机的 JSON 可能上百 KB) */
static unsigned char g_rx[262144];
static int g_rx_len = 0;

/* 订阅跟踪: 记录每个 SUBSCRIBE 的包序号, 收到 SUBACK 时对应回显 */
#define MAX_SUBS 8
static char  g_sub_topic[MAX_SUBS][160];
static short g_sub_pid[MAX_SUBS];
static int   g_sub_n = 0;

static short g_last_pub_pid = 0;       /* 最近一次 QoS1 PUBLISH 的包序号 */

#define KEEPALIVE_MS 40000   /* 每 40s 发送 PINGREQ (keepalive=60s) */

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static int SC(int v) { return (int)(v * g_sf + 0.5f); }

static char *dup_a(const char *s) {
    char *p = (char *)malloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

static wchar_t *dup_w(const wchar_t *s) {
    int n = (int)wcslen(s);
    wchar_t *p = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    memcpy(p, s, (n + 1) * sizeof(wchar_t));
    return p;
}

/* UTF-8 -> 宽字符 (堆), 超过 maxbytes 时安全截断 */
static wchar_t *to_w(const char *s, int maxbytes) {
    if (!s) s = "";
    int n = (int)strlen(s);
    if (n > maxbytes) n = maxbytes;
    /* 回退到 UTF-8 序列边界 */
    while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, n, w, wlen);
    w[wlen] = 0;
    return w;
}

/* 宽字符 -> UTF-8 (堆) */
static char *to_a(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* 从简单 JSON 中提取字符串值: 查找 "key":"value" */
static int json_get_str(const char *json, const char *key, char *out, int outlen) {
    out[0] = 0;
    char pat[64];
    sprintf(pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    int n = 0;
    while (*p && *p != '"' && n < outlen - 1) {
        if (*p == '\\' && p[1] && n < outlen - 2) {
            out[n++] = *p++; out[n++] = *p++;      /* 保留转义序列 */
        } else if (*p == '\\') {
            break;                                  /* 空间不够, 安全截断 */
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return n > 0;
}

/* ------------------------------------------------------------------ */
/* 诊断日志 (exe 同目录 pbxdialer_debug.log)                            */
/*     出问题时把该文件发给技术支持即可看到全部收发报文                  */
/* ------------------------------------------------------------------ */

#define DEBUG_LOG_NAME L"pbxdialer_debug.log"
#define DEBUG_LOG_MAX  (1024 * 1024)     /* 超过 1MB 自动重置 */

static void dbg_path(wchar_t *out, int outlen) {
    GetModuleFileNameW(NULL, out, outlen);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
    lstrcatW(out, DEBUG_LOG_NAME);
}

/* 把控制字符转义成可见形式, 便于阅读 */
static void dbg_escape(char *dst, int dstlen, const char *src) {
    int n = 0;
    for (const char *p = src; *p && n < dstlen - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') { dst[n++] = '\\'; dst[n++] = 'r'; }
        else if (c == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
        else if (c == '\t') { dst[n++] = '\\'; dst[n++] = 't'; }
        else if (c < 0x20) { sprintf(dst + n, "\\x%02x", c); n += 4; }
        else dst[n++] = (char)c;
    }
    dst[n] = 0;
}

static void dbg_write(const char *text) {
    wchar_t path[512];
    dbg_path(path, 512);
    FILE *fp = _wfopen(path, L"rb");
    long size = 0;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fclose(fp);
    }
    fp = _wfopen(path, (size > DEBUG_LOG_MAX) ? L"wb" : L"ab");
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(fp, "[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, text);
    fclose(fp);
}

static void dbg_logf(const char *fmt, ...) {
    char buf[4600];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    dbg_write(buf);
}

/* ------------------------------------------------------------------ */
/* 日志 / 状态 (供 UI 线程调用)                                        */
/* ------------------------------------------------------------------ */

static void append_log(const wchar_t *line) {
    if (!g_hLog || !IsWindow(g_hLog)) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[16];
    wsprintfW(ts, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);

    int linelen = (int)wcslen(line);
    int extra = (int)wcslen(ts) + 2;   /* 时间戳 + "\r\n" */
    int maxchars = 50000;              /* 日志上限 (字符) */

    int full = GetWindowTextLengthW(g_hLog);
    int keep = maxchars - linelen - extra;
    if (keep < 0) keep = 0;

    wchar_t *buf = (wchar_t *)malloc(((size_t)full + linelen + extra + 2) * sizeof(wchar_t));
    if (!buf) return;
    GetWindowTextW(g_hLog, buf, full + 1);
    int slen = full;
    if (slen > keep) {
        memmove(buf, buf + (slen - keep), (size_t)(keep + 1) * sizeof(wchar_t));
        slen = keep;
    }
    lstrcpynW(buf + slen, ts, (int)wcslen(ts) + 1);
    lstrcpynW(buf + slen + wcslen(ts), line, linelen + 1);
    lstrcpynW(buf + slen + wcslen(ts) + linelen, L"\r\n", 3);
    SetWindowTextW(g_hLog, buf);
    free(buf);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void append_logf(const wchar_t *fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1000, fmt, ap);
    va_end(ap);
    buf[1000] = 0;
    append_log(buf);
}

static void post_log_w(const wchar_t *line) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)dup_w(line));
}

static void post_log_fw(const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 480, fmt, ap);
    va_end(ap);
    buf[480] = 0;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)dup_w(buf));
}

static void post_status(const wchar_t *text) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_STATUS, 0, (LPARAM)dup_w(text));
}

/* 把收到的报文 (主题 + 内容) 投递给 UI 线程; 用 \x01 分隔 */
static void post_msg(const char *topic, const char *payload, int len) {
    if (!g_hwnd) return;
    int tl = (int)strlen(topic);
    char *s = (char *)malloc((size_t)tl + 1 + len + 1);
    if (!s) return;
    memcpy(s, topic, tl);
    s[tl] = '\x01';
    memcpy(s + tl + 1, payload, len);
    s[tl + 1 + len] = 0;
    PostMessageW(g_hwnd, WM_APP_PKT, 0, (LPARAM)s);
}

/* 校验拨号字段: 只允许 数字 A-Z a-z * # + - . */
static int sanitize_number(const wchar_t *in, wchar_t *out, int outlen) {
    int n = 0;
    for (int i = 0; in[i] && n < outlen - 1; i++) {
        wchar_t c = in[i];
        int ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                 (c >= L'A' && c <= L'Z') || c == L'*' || c == L'#' ||
                 c == L'+' || c == L'-' || c == L'.';
        if (ok) out[n++] = c;
    }
    out[n] = 0;
    return n;
}

/* 从下拉框文本里取号码: "1000" 或 "1000 (张三)" 都只取 1000 */
static void caller_number(const wchar_t *in, wchar_t *out, int outlen) {
    wchar_t head[256] = {0};
    int n = 0;
    for (int i = 0; in[i] && n < 255; i++) {
        if (in[i] == L' ' || in[i] == L'\t' || in[i] == L'(' || in[i] == L'（') break;
        head[n++] = in[i];
    }
    head[n] = 0;
    sanitize_number(head, out, outlen);
}

/* ------------------------------------------------------------------ */
/* 配置文件 (exe 同目录 pbxdialer.ini)                                 */
/* ------------------------------------------------------------------ */

static void config_path(wchar_t *out, int outlen) {
    (void)outlen;
    GetModuleFileNameW(NULL, out, outlen);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
    lstrcatW(out, CONFIG_NAME);
}

/* ini 里的开关值: 1/0、yes/no、true/false、on/off 都认; 认不出来按"开"处理 */
static int parse_bool(const char *v) {
    if (!v) return 1;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T') return 1;
    if (*v == '0' || *v == 'n' || *v == 'N' || *v == 'f' || *v == 'F') return 0;
    return 1;
}

static void load_config(void) {
    wchar_t path[512];
    config_path(path, 512);
    G.host[0] = 0; G.port = 1883;
    G.user[0] = 0; G.pass[0] = 0; G.token[0] = 0; G.caller[0] = 0;
    G.hist_n = 0;
    /* 协议默认值 (ini 里同名键可覆盖) */
    lstrcpynA(G.topic_system, "device/%s/api/v1.0/command/system", 191);
    lstrcpynA(G.topic_call,   "device/%s/api/v1.0/command/call", 191);
    G.sub_extra[0] = 0;
    G.dial_extra[0] = 0;
    G.test_topic[0] = 0;
    G.test_payload[0] = 0;
    G.cmd_qos = 1;
    G.autoanswer = 1;               /* 默认勾选"自动接通" */
    G.show_diag = 0;                /* 默认不显示 [诊断]/[复制日志] */

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[4096] = {0};
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, NULL);
    CloseHandle(h);
    char *save = NULL;
    char *line = strtok_r(buf, "\r\n", &save);
    while (line) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *val = eq + 1;              /* 修正: 跳过 '=' 后才是值 */
            while (*val == ' ' || *val == '\t') val++;
            if (!strcmp(line, "host"))      lstrcpynA(G.host, val, 127);
            else if (!strcmp(line, "port")) G.port = atoi(val);
            else if (!strcmp(line, "user")) lstrcpynA(G.user, val, 95);
            else if (!strcmp(line, "pass")) lstrcpynA(G.pass, val, 95);
            else if (!strcmp(line, "token"))lstrcpynA(G.token, val, 127);
            else if (!strcmp(line, "caller"))lstrcpynA(G.caller, val, 95);
            else if (!strcmp(line, "callee_history")) {   /* 用 | 分隔, 最近在前 */
                char *save2 = NULL;
                char *tok = strtok_r(val, "|", &save2);
                G.hist_n = 0;
                while (tok && G.hist_n < MAX_HIST) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    int tl = (int)strlen(tok);
                    while (tl > 0 && (tok[tl-1] == ' ' || tok[tl-1] == '\t')) tok[--tl] = 0;
                    if (tl > 0) lstrcpynA(G.hist[G.hist_n++], tok, 39);
                    tok = strtok_r(NULL, "|", &save2);
                }
            }
            else if (!strcmp(line, "topic_system")) lstrcpynA(G.topic_system, val, 191);
            else if (!strcmp(line, "topic_call"))   lstrcpynA(G.topic_call, val, 191);
            else if (!strcmp(line, "sub_extra"))    lstrcpynA(G.sub_extra, val, 255);
            else if (!strcmp(line, "dial_extra"))   lstrcpynA(G.dial_extra, val, 255);
            else if (!strcmp(line, "test_topic"))   lstrcpynA(G.test_topic, val, 191);
            else if (!strcmp(line, "test_payload")) lstrcpynA(G.test_payload, val, 767);
            else if (!strcmp(line, "cmd_qos"))      G.cmd_qos = atoi(val);
            else if (!strcmp(line, "autoanswer"))   G.autoanswer = parse_bool(val);
            else if (!strcmp(line, "show_diag"))    G.show_diag = parse_bool(val);
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
    if (G.port < 1) G.port = 1883;
    if (G.cmd_qos != 0) G.cmd_qos = 1;
}

static void save_config(void) {
    wchar_t path[512];
    config_path(path, 512);
    /* 顺手记住当前主叫分机 (save_config 只在 UI 线程调用) */
    if (g_hCaller && IsWindow(g_hCaller)) {
        wchar_t w[256] = {0}, num[128] = {0};
        GetWindowTextW(g_hCaller, w, 255);
        caller_number(w, num, 128);
        char *a = to_a(num);
        G.caller[0] = 0;
        lstrcpynA(G.caller, a, 95);
        free(a);
    }
    /* 记住"自动接通"勾选状态 */
    if (g_hAuto && IsWindow(g_hAuto))
        G.autoanswer = (SendMessageW(g_hAuto, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char hist[800] = {0};
    for (int i = 0; i < G.hist_n && i < MAX_HIST; i++) {
        if (i) strncat(hist, "|", sizeof(hist) - strlen(hist) - 1);
        strncat(hist, G.hist[i], sizeof(hist) - strlen(hist) - 1);
    }
    char *buf = (char *)malloc(4096);
    if (!buf) { CloseHandle(h); return; }
    int n = sprintf(buf,
        "host=%s\r\nport=%d\r\nuser=%s\r\npass=%s\r\ntoken=%s\r\ncaller=%s\r\n"
        "callee_history=%s\r\nautoanswer=%d\r\nshow_diag=%d\r\n"
        "topic_system=%s\r\ntopic_call=%s\r\nsub_extra=%s\r\ndial_extra=%s\r\n"
        "test_topic=%s\r\ntest_payload=%s\r\ncmd_qos=%d\r\n",
        G.host, G.port, G.user, G.pass, G.token, G.caller, hist, G.autoanswer, G.show_diag,
        G.topic_system, G.topic_call, G.sub_extra, G.dial_extra,
        G.test_topic, G.test_payload, G.cmd_qos);
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    free(buf);
    CloseHandle(h);
}

/* 从界面控件读入设置 */
static void read_settings_from_ui(void) {
    wchar_t t[256];
    GetWindowTextW(g_hHost, t, 200);  G.host[0] = 0;
    { char *a = to_a(t); lstrcpynA(G.host, a, 127); free(a); }
    GetWindowTextW(g_hPort, t, 200);  G.port = _wtoi(t);
    GetWindowTextW(g_hUser, t, 200);  G.user[0] = 0;
    { char *a = to_a(t); lstrcpynA(G.user, a, 95); free(a); }
    GetWindowTextW(g_hPass, t, 200);  G.pass[0] = 0;
    { char *a = to_a(t); lstrcpynA(G.pass, a, 95); free(a); }
    GetWindowTextW(g_hToken, t, 200); G.token[0] = 0;
    { char *a = to_a(t); lstrcpynA(G.token, a, 127); free(a); }
    if (G.port < 1 || G.port > 65535) G.port = 1883;
}

static void fill_ui_from_settings(void) {
    SetWindowTextW(g_hHost, to_w(G.host, 200));
    wchar_t tmp[32];
    wsprintfW(tmp, L"%d", G.port);
    SetWindowTextW(g_hPort, tmp);
    SetWindowTextW(g_hUser, to_w(G.user, 200));
    SetWindowTextW(g_hPass, to_w(G.pass, 200));
    SetWindowTextW(g_hToken, to_w(G.token, 200));
    if (G.caller[0])
        SetWindowTextW(g_hCaller, to_w(G.caller, 200));
    if (g_hCallee && IsWindow(g_hCallee)) {
        SendMessageW(g_hCallee, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < G.hist_n; i++) {
            wchar_t *w = to_w(G.hist[i], 64);
            SendMessageW(g_hCallee, CB_ADDSTRING, 0, (LPARAM)w);
            free(w);
        }
        SendMessageW(g_hCallee, CB_SETCURSEL, (WPARAM)-1, 0);
        SetWindowTextW(g_hCallee, L"");     /* 被叫不自动回填, 避免误拨 */
    }
    /* 自动接通: 按 ini 恢复勾选状态 (默认勾选) */
    if (g_hAuto && IsWindow(g_hAuto))
        SendMessageW(g_hAuto, BM_SETCHECK,
                     G.autoanswer ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ------------------------------------------------------------------ */
/* MQTT: 网络收发                                                      */
/* ------------------------------------------------------------------ */

static int send_all(SOCKET s, const void *buf, int len) {
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int r = send(s, p + sent, len - sent, 0);
        if (r <= 0) return 0;
        sent += r;
    }
    return 1;
}

static void close_sock(void) {
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    g_connected = 0;
    g_rx_len = 0;
}

static int encode_varint(unsigned char *buf, int val) {
    int n = 0;
    do {
        unsigned char b = (unsigned char)(val % 128);
        val /= 128;
        if (val > 0) b |= 0x80;
        buf[n++] = b;
    } while (val > 0);
    return n;
}

static int decode_varint(const unsigned char *p, int avail, int *val) {
    *val = 0;
    int mult = 1, n = 0;
    for (; n < avail && n < 4; n++) {
        unsigned char b = p[n];
        *val += (b & 0x7f) * mult;
        if (!(b & 0x80)) return n + 1;
        mult *= 128;
    }
    return -1;
}

/* 用模板拼主题: 模板里的 %s 会替换成 token; 没有 %s 就原样使用 */
static void build_topic(char *out, int outlen, const char *tpl, const char *token) {
    const char *pos = strstr(tpl, "%s");
    if (!pos) { lstrcpynA(out, tpl, outlen); return; }
    int pre = (int)(pos - tpl);
    if (pre >= outlen - 1) pre = outlen - 1;
    memcpy(out, tpl, (size_t)pre);
    out[pre] = 0;
    lstrcpynA(out + pre, token, outlen - pre);
    int used = (int)strlen(out);
    if (used < outlen - 1)
        lstrcpynA(out + used, pos + 2, outlen - used);
}

/* CONNECT: TCP + MQTT 握手 */
static int connect_once(void) {
    close_sock();
    post_status(L"连接中...");

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        post_log_fw(L"创建 socket 失败: %d", WSAGetLastError());
        return 0;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)G.port);
    {
        u_long ip = inet_addr(G.host);
        if (ip == INADDR_NONE) {
            post_log_fw(L"IP 地址无效: %hs", G.host);
            closesocket(s);
            return 0;
        }
        sa.sin_addr.S_un.S_addr = ip;
    }

    /* 非阻塞连接 + 5 秒超时 */
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv;
        tv.tv_sec = 5; tv.tv_usec = 0;
        if (select(s + 1, NULL, &wf, NULL, &tv) != 1) {
            post_log_fw(L"连接超时: %hs:%d", G.host, G.port);
            closesocket(s);
            return 0;
        }
        int soerr = 0, slen = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen);
        if (soerr != 0) {
            post_log_fw(L"连接被拒绝: %hs:%d", G.host, G.port);
            closesocket(s);
            return 0;
        }
    } else if (rc == SOCKET_ERROR) {
        post_log_fw(L"连接失败: %hs:%d", G.host, G.port);
        closesocket(s);
        return 0;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    /* Winsock 的超时值是 DWORD 毫秒, 不是 BSD timeval (timeval 会被当成 0 = 永久阻塞) */
    DWORD rcvto = 500;      /* 收超时 500ms */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcvto, sizeof(rcvto));
    DWORD sndto = 3000;     /* 发超时 3s */
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&sndto, sizeof(sndto));

    /* 构造 MQTT CONNECT (3.1.1, clean session, keepalive 60s) */
    char clientid[48];
    sprintf(clientid, "pbxdialer-%04x", (unsigned)(GetTickCount() & 0xFFFF));
    int flags = 0x02;                        /* clean session */
    if (G.user[0]) flags |= 0x80;
    if (G.pass[0]) flags |= 0x40;

    unsigned char body[512];
    int n = 0;
    body[n++] = 0x00; body[n++] = 0x04;
    body[n++] = 'M'; body[n++] = 'Q'; body[n++] = 'T'; body[n++] = 'T';
    body[n++] = 0x04;
    body[n++] = (unsigned char)flags;
    body[n++] = 0x00; body[n++] = 0x3C;      /* keepalive 60 */
    int idlen = (int)strlen(clientid);
    body[n++] = 0; body[n++] = (unsigned char)idlen;
    memcpy(body + n, clientid, idlen); n += idlen;
    if (G.user[0]) {
        int ulen = (int)strlen(G.user);
        body[n++] = 0; body[n++] = (unsigned char)ulen;
        memcpy(body + n, G.user, ulen); n += ulen;
    }
    if (G.pass[0]) {
        int plen = (int)strlen(G.pass);
        body[n++] = 0; body[n++] = (unsigned char)plen;
        memcpy(body + n, G.pass, plen); n += plen;
    }
    unsigned char pkt[528];
    pkt[0] = 0x10;
    int hn = 1 + encode_varint(pkt + 1, n);
    memcpy(pkt + hn, body, n);

    dbg_logf("[CONNECT] %s:%d user=%s clientid=%s", G.host, G.port,
             G.user[0] ? G.user : "(none)", clientid);

    if (!send_all(s, pkt, hn + n)) {
        post_log_fw(L"发送 CONNECT 失败");
        closesocket(s);
        return 0;
    }

    /* 读取 CONNACK: 最多等 5 秒 */
    unsigned char ack[4];
    int got = 0;
    int ok = 0;
    for (int i = 0; i < 50 && got < 4; i++) {
        int r = recv(s, (char *)ack + got, 4 - got, 0);
        if (r > 0) { got += r; continue; }
        if (r == 0) break;
        if (WSAGetLastError() != WSAETIMEDOUT) break;
    }
    if (got >= 4 && ack[0] == 0x20 && ack[1] == 0x02) {
        int code = ack[3];
        dbg_logf("[CONNACK] %s:%d code=%d", G.host, G.port, code);
        if (code == 0) {
            ok = 1;
        } else {
            const wchar_t *msg = L"未知原因";
            switch (code) {
                case 1: msg = L"协议错误"; break;
                case 2: msg = L"客户端 ID 被拒绝"; break;
                case 3: msg = L"服务器不可用"; break;
                case 4: msg = L"用户名或密码错误"; break;
                case 5: msg = L"未授权"; break;
            }
            post_log_fw(L"服务器拒绝连接 (code=%d, %ls)", code, msg);
        }
    } else if (got > 0) {
        post_log_fw(L"CONNACK 无效或超时");
    } else {
        post_log_fw(L"CONNACK 超时");
    }

    if (!ok) {
        closesocket(s);
        return 0;
    }

    g_sock = s;
    g_connected = 1;
    g_last_act = GetTickCount();
    return 1;
}

/* 订阅一个主题 (单独一个 SUBSCRIBE 包, 便于逐个确认结果) */
static int subscribe_one(const char *topic) {
    int tl = (int)strlen(topic);
    if (++g_seq >= 32000) g_seq = 1;
    short pid = g_seq;
    int rem = 2 + (2 + tl + 1);
    unsigned char pkt[320];
    pkt[0] = 0x82;
    int hn = 1 + encode_varint(pkt + 1, rem);
    int n = hn;
    pkt[n++] = (unsigned char)(pid >> 8);
    pkt[n++] = (unsigned char)(pid & 0xFF);
    pkt[n++] = (unsigned char)(tl >> 8); pkt[n++] = (unsigned char)(tl & 0xFF);
    memcpy(pkt + n, topic, tl); n += tl;
    pkt[n++] = 0x00;                        /* QoS 0 */
    if (g_sub_n < MAX_SUBS) {
        lstrcpynA(g_sub_topic[g_sub_n], topic, 159);
        g_sub_pid[g_sub_n] = pid;
        g_sub_n++;
    }
    return send_all(g_sock, pkt, n);
}

/* 订阅 response / event, 另加通配 (兼容不同固件的主题命名) */
static short g_wild_pid = 0;
static int   g_sub_fallback_done = 0;

static void subscribe_fallback(void) {
    char t[160];
    if (g_sub_fallback_done) return;
    g_sub_fallback_done = 1;
    sprintf(t, "device/%s/api/v1.0/response", G.token);
    subscribe_one(t);
    sprintf(t, "device/%s/api/v1.0/event", G.token);
    subscribe_one(t);
    post_log_w(L"通配订阅被拒绝, 已改用 response/event 精确订阅");
}

static int subscribe_all(void) {
    char t[192];
    g_sub_n = 0;
    g_sub_fallback_done = 0;
    /* 先订阅通配: 一次覆盖 response / event 以及固件可能使用的其它主题,
       且避免多主题重复投递 */
    sprintf(t, "device/%s/#", G.token);
    g_wild_pid = (short)(g_seq + 1);
    int ok = subscribe_one(t);
    /* ini 里 sub_extra 可追加自定义订阅 (多个用 ; 分隔) */
    if (G.sub_extra[0]) {
        char tmp[256];
        lstrcpynA(tmp, G.sub_extra, 255);
        char *save = NULL;
        char *one = strtok_r(tmp, ";", &save);
        while (one && g_sub_n < MAX_SUBS) {
            while (*one == ' ') one++;
            if (*one) {
                char topic2[192];
                build_topic(topic2, sizeof(topic2), one, G.token);
                subscribe_one(topic2);
                ok++;
            }
            one = strtok_r(NULL, ";", &save);
        }
    }
    return ok;
}

/* PUBLISH: cmd 用 QoS1 (能看到服务器确认), 便于诊断 */
static int publish_qos(const char *topic, const char *payload, int qos) {
    int tl = (int)strlen(topic), pl = (int)strlen(payload);
    int extra = qos ? 2 : 0;
    unsigned char hdr[8];
    int hn = 1 + encode_varint(hdr + 1, 2 + tl + pl + extra);
    hdr[0] = qos ? 0x32 : 0x30;             /* PUBLISH, QoS1 / QoS0 */
    unsigned char *pkt = (unsigned char *)malloc(hn + 2 + tl + pl + extra);
    memcpy(pkt, hdr, hn);
    pkt[hn]     = (unsigned char)(tl >> 8);
    pkt[hn + 1] = (unsigned char)(tl & 0xFF);
    memcpy(pkt + hn + 2, topic, tl);
    int n = hn + 2 + tl;
    if (qos) {
        if (++g_seq >= 32000) g_seq = 1;
        short pid = g_seq;
        g_last_pub_pid = pid;
        pkt[n++] = (unsigned char)(pid >> 8);
        pkt[n++] = (unsigned char)(pid & 0xFF);
        dbg_logf("[TX] pid=%d topic=%s payload=%s", pid, topic, payload);
    } else {
        dbg_logf("[TX] topic=%s payload=%s", topic, payload);
    }
    memcpy(pkt + n, payload, pl);
    n += pl;
    int ok = send_all(g_sock, pkt, n);
    free(pkt);
    if (ok) g_last_act = GetTickCount();
    return ok;
}

static int publish(const char *topic, const char *payload) {
    return publish_qos(topic, payload, G.cmd_qos);
}

static void publish_extlist_request(void) {
    char topic[160];
    build_topic(topic, sizeof(topic), G.topic_system, G.token);
    char json[128];
    sprintf(json, "{\"cmd\":\"extension_list\",\"request_id\":\"%08x\"}",
            (unsigned)GetTickCount());
    if (publish(topic, json))
        post_log_w(L"已请求分机列表 (extension_list)");
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_EXTWAIT, 0, 0);
}

/* ------------------------------------------------------------------ */
/* MQTT: 接收处理                                                      */
/* ------------------------------------------------------------------ */

static void process_packet(unsigned char first, const unsigned char *body, int blen) {
    switch (first & 0xF0) {
    case 0x30: { /* PUBLISH */
        int qos = (first & 0x06) >> 1;
        if (blen < 2) break;
        int tlen = (body[0] << 8) | body[1];
        if (tlen > 150 || blen < 2 + tlen) break;
        char topic[160];
        memcpy(topic, body + 2, tlen);
        topic[tlen] = 0;
        const unsigned char *rest = body + 2 + tlen;
        int rlen = blen - 2 - tlen;
        if (qos > 0) {
            if (rlen < 2) break;
            if (qos == 1) {
                unsigned char pk[4];
                pk[0] = 0x40; pk[1] = 0x02;
                pk[2] = rest[0]; pk[3] = rest[1];
                send_all(g_sock, pk, 4);
            }
            rest += 2;
            rlen -= 2;
        }
        if (rlen > 0) {
            /* 跳过自己发出的命令主题 (通配订阅可能把自己发的消息回显回来) */
            if (strstr(topic, "/command/")) break;
            char esc[4096];
            char tmp[4096];
            int tlen = rlen < (int)sizeof(tmp) - 1 ? rlen : (int)sizeof(tmp) - 1;
            memcpy(tmp, rest, (size_t)tlen);
            tmp[tlen] = 0;
            dbg_escape(esc, sizeof(esc), tmp);
            dbg_logf("[RX] qos=%d topic=%s len=%d payload=%s%s", qos, topic, rlen, esc,
                     (rlen > tlen) ? " ...(报文过长, 已截断, 解析用完整报文)" : "");
            post_msg(topic, (const char *)rest, rlen);
        }
        break;
    }
    case 0x40: { /* PUBACK */
        if (blen >= 2) {
            short pid = (short)((body[0] << 8) | body[1]);
            dbg_logf("[PUBACK] pid=%d (server accepted)", pid);
        }
        break;
    }
    case 0x90: { /* SUBACK: 每个订阅一个返回码, 0x80 = 失败 */
        if (blen >= 2) {
            short pid = (short)((body[0] << 8) | body[1]);
            const char *topic = "?";
            for (int i = 0; i < g_sub_n; i++)
                if (g_sub_pid[i] == pid) topic = g_sub_topic[i];
            unsigned char rc = (blen >= 3) ? body[2] : 0xFF;
            dbg_logf("[SUBACK] pid=%d rc=0x%02x topic=%s", pid, rc, topic);
            if (rc == 0x80) {
                if (pid == g_wild_pid) {
                    subscribe_fallback();       /* 通配被拒 -> 退回精确主题 */
                } else {
                    wchar_t w[240];
                    wchar_t *wt = to_w(topic, 140);
                    wsprintfW(w, L"订阅被服务器拒绝: %ls", wt);
                    free(wt);
                    post_log_w(w);
                }
            }
        }
        break;
    }
    default:
        break;
    }
    g_last_act = GetTickCount();
}

/* 读取并解析一个完整数据包; 返回 -1 表示连接出错, 否则为处理包数 */
static int drain_rx(void) {
    if (g_rx_len < (int)sizeof(g_rx) - 1024) {
        int r = recv(g_sock, (char *)g_rx + g_rx_len,
                     (int)sizeof(g_rx) - 1024 - g_rx_len, 0);
        if (r > 0) {
            g_rx_len += r;
        } else if (r == 0) {
            return -1;
        } else {
            int e = WSAGetLastError();
            if (e != WSAETIMEDOUT && e != WSAEWOULDBLOCK) return -1;
        }
    }
    int processed = 0;
    for (;;) {
        if (g_rx_len < 2) break;
        int rem = 0;
        int vb = decode_varint(g_rx + 1, g_rx_len - 1, &rem);
        if (vb < 0) break;
        int hl = 1 + vb;                 /* 固定头 1 字节 + 变长长度字段 */
        int need = hl + rem;
        if (g_rx_len < need) break;
        process_packet(g_rx[0], g_rx + hl, rem);
        g_rx_len -= need;
        memmove(g_rx, g_rx + need, g_rx_len);
        processed++;
    }
    return processed;
}

/* 发送 PINGREQ */
static void send_ping(void) {
    unsigned char pk[2] = { 0xC0, 0x00 };
    if (send_all(g_sock, pk, 2))
        g_last_act = GetTickCount();
}

/* 发送待发队列 */
static void flush_outbox(void) {
    Pending local[16];
    int n = 0;
    EnterCriticalSection(&g_cs);
    n = g_box_n;
    if (n > 16) n = 16;
    memcpy(local, g_box, (size_t)n * sizeof(Pending));
    g_box_n = 0;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < n; i++) {
        if (!g_connected) break;
        if (!publish(local[i].topic, local[i].payload)) {
            post_log_w(L"发送失败: 网络错误");
            continue;
        }
        post_log_fw(L"→ 已发送 %hs", local[i].payload);
    }
}

/* 入队 (UI 线程调用) */
static int enqueue(const char *topic, const char *payload) {
    EnterCriticalSection(&g_cs);
    if (g_box_n >= 16) {
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    lstrcpynA(g_box[g_box_n].topic, topic, 159);
    lstrcpynA(g_box[g_box_n].payload, payload, 4095);
    g_box_n++;
    LeaveCriticalSection(&g_cs);
    return 1;
}

/* ------------------------------------------------------------------ */
/* MQTT 工作线程                                                       */
/* ------------------------------------------------------------------ */

static DWORD WINAPI mqtt_thread(LPVOID param) {
    (void)param;
    Sleep(500);
    for (;;) {
        if (g_stop) break;
        if (!g_run) { Sleep(300); continue; }

        if (g_force_reconnect) {
            g_force_reconnect = 0;
            if (g_connected) {
                close_sock();
                post_status(L"未连接");
            }
        }

        if (!g_connected) {
            if (connect_once()) {
                post_status(L"已连接");
                post_log_fw(L"MQTT 已连接 %hs:%d", G.host, G.port);
                if (subscribe_all())
                    post_log_w(L"已订阅 device/<Token>/# 主题");
                publish_extlist_request();
            } else {
                post_status(L"未连接");
                post_log_w(L"连接失败, 2 秒后重试...");
                Sleep(2000);
                continue;
            }
        }

        flush_outbox();

        int rxr = drain_rx();
        if (rxr < 0) {
            post_log_w(L"连接已断开");
            close_sock();
            post_status(L"未连接");
            Sleep(1000);
            continue;
        }

        DWORD now = GetTickCount();
        if ((int)(now - g_last_act) > KEEPALIVE_MS)
            send_ping();

        Sleep(50);
    }
    close_sock();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 分机列表处理 (UI 线程)                                              */
/* ------------------------------------------------------------------ */

/* 往主叫下拉框里加一个分机 (自动去重) */
static int ext_add(const char *number, const char *username,
                   char seen[][32], int *seen_n, int maxseen) {
    if (!number || !number[0]) return 0;
    for (int i = 0; i < *seen_n; i++)
        if (!strcmp(seen[i], number)) return 0;     /* 已存在 */
    if (*seen_n >= maxseen) return 0;
    lstrcpynA(seen[*seen_n], number, 31);
    (*seen_n)++;
    wchar_t item[256];
    if (username && username[0]) {
        wchar_t *un = to_w(username, 120);
        wsprintfW(item, L"%hs (%ls)", number, un);
        free(un);
    } else {
        wsprintfW(item, L"%hs", number);
    }
    SendMessageW(g_hCaller, CB_ADDSTRING, 0, (LPARAM)item);
    return 1;
}

/* 在 [begin,end) 里按 {...} 逐个解析分机信息 */
static int ext_scan_objects(const char *begin, const char *end,
                            char seen[][32], int *seen_n, int maxseen) {
    int count = 0;
    const char *q = begin;
    while (q < end) {
        const char *cs = (const char *)memchr(q, '{', (size_t)(end - q));
        if (!cs) break;
        const char *ce = (const char *)memchr(cs, '}', (size_t)(end - cs));
        if (!ce) break;
        int clen = (int)(ce - cs) + 1;
        char chunk[768];
        if (clen > (int)sizeof(chunk) - 1) clen = (int)sizeof(chunk) - 1;
        memcpy(chunk, cs, (size_t)clen);
        chunk[clen] = 0;
        char number[64] = {0}, username[128] = {0};
        json_get_str(chunk, "number", number, 64);
        json_get_str(chunk, "username", username, 128);
        if (!username[0]) json_get_str(chunk, "fullname", username, 128);
        count += ext_add(number, username, seen, seen_n, maxseen);
        q = ce + 1;
    }
    return count;
}

/* 兜底: 直接把所有 "number" 的值捞出来 (应对结构不同的固件) */
static int ext_scan_numbers(const char *json, char seen[][32], int *seen_n, int maxseen) {
    int count = 0;
    const char *q = json;
    while ((q = strstr(q, "\"number\"")) != NULL) {
        char number[64] = {0}, username[128] = {0};
        if (json_get_str(q, "number", number, 64) && number[0]) {
            /* 同一对象里往后找 username / fullname */
            const char *obj_end = strchr(q, '}');
            const char *un = strstr(q, "\"username\"");
            const char *fn = strstr(q, "\"fullname\"");
            if (un && (!obj_end || un < obj_end)) json_get_str(un, "username", username, 128);
            else if (fn && (!obj_end || fn < obj_end)) json_get_str(fn, "fullname", username, 128);
            count += ext_add(number, username, seen, seen_n, maxseen);
        }
        q += 8;
    }
    return count;
}

static void parse_extlist(const char *json) {
    /* 记住当前主叫框内容 (可能是手输的号码, 也可能是选中了列表项) */
    wchar_t cur[256] = {0};
    GetWindowTextW(g_hCaller, cur, 255);

    SendMessageW(g_hCaller, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hCaller, CB_INITSTORAGE, (WPARAM)MAX_EXT, (LPARAM)(MAX_EXT * 12));
    static char seen[MAX_EXT][32];
    int seen_n = 0;
    int count = 0;

    /* 1) 标准位置: "extlist": [ ... ] */
    const char *arr = strstr(json, "\"extlist\"");
    if (arr) {
        const char *lb = strchr(arr, '[');
        if (lb) {
            const char *begin = lb + 1;
            const char *endp = strchr(begin, ']');
            if (!endp) endp = json + strlen(json);
            count = ext_scan_objects(begin, endp, seen, &seen_n, MAX_EXT);
        }
    }
    /* 2) 没有 extlist 或里面没解析出东西: 全局兜底扫描 */
    if (count == 0) {
        const char *jend = json + strlen(json);
        count = ext_scan_objects(json, jend, seen, &seen_n, MAX_EXT);
        if (count == 0) count = ext_scan_numbers(json, seen, &seen_n, MAX_EXT);
    }

    /* 恢复主叫: 能对上列表项就选中它; 对不上(手输号码)就原样保留, 不要覆盖 */
    if (count > 0) {
        int matched = -1;
        /* 注意: CB_GETLBTEXTLEN 返回的是"字符数", 文本是 wchar_t,
           必须按 wchar_t 宽度备缓冲 (之前按字节 malloc 会踩坏堆) */
        wchar_t cur_w[256] = {0};
        lstrcpynW(cur_w, cur, 255);
        int cl = lstrlenW(cur_w);
        if (cl > 0) {
            for (int i = 0; i < count; i++) {
                int tl = (int)SendMessageW(g_hCaller, CB_GETLBTEXTLEN, (WPARAM)i, 0);
                if (tl != cl || tl > 250) continue;
                wchar_t t[256];
                t[0] = 0;
                SendMessageW(g_hCaller, CB_GETLBTEXT, (WPARAM)i, (LPARAM)t);
                if (!lstrcmpW(t, cur_w)) { matched = i; break; }
            }
        }
        if (matched >= 0) {
            SendMessageW(g_hCaller, CB_SETCURSEL, (WPARAM)matched, 0);
        } else if (cl > 0) {
            SendMessageW(g_hCaller, CB_SETCURSEL, (WPARAM)-1, 0);
            SetWindowTextW(g_hCaller, cur_w);
        } else {
            SendMessageW(g_hCaller, CB_SETCURSEL, 0, 0);
        }
    }

    append_logf(L"分机列表已刷新: 共 %d 个分机", count);
    if (count >= MAX_EXT)
        append_logf(L"注意: 已达显示上限 %d 个, 更多分机请直接在框里输入号码", MAX_EXT);
    if (count == 0) {
        append_log(L"注意: PBX 没有返回任何分机。主叫分机可直接在框里手动输入;");
        append_log(L"      原始回复见 pbxdialer_debug.log, 请点 [复制日志] 发给技术支持");
    }
    if (g_hwnd) KillTimer(g_hwnd, IDT_EXTWAIT);
}

/* ------------------------------------------------------------------ */
/* 收到 MQTT 包 (UI 线程)                                              */
/* ------------------------------------------------------------------ */

static void handle_packet(char *packed) {
    /* packed = "topic\x01payload" */
    char *sep = strchr(packed, '\x01');
    const char *topic = packed;
    const char *json = "";
    if (sep) { *sep = 0; json = sep + 1; }

    char status[64] = {0}, message[256] = {0}, ev[64] = {0};

    if (strstr(json, "extlist") || strstr(json, "\"number\"")) {
        /* 分机列表: 不论主题/字段名, 只要带分机号就按列表处理 */
        parse_extlist(json);
        return;
    }
    if (strstr(json, "\"event\"")) {                /* 事件 */
        json_get_str(json, "event", ev, 64);
        char a[64] = {0}, b[64] = {0}, c[64] = {0};
        if (strcmp(ev, "invite") == 0) {
            json_get_str(json, "from", a, 64);
            json_get_str(json, "to", b, 64);
            json_get_str(json, "callid", c, 64);
            append_logf(L"来电: %hs → %hs (callid=%hs)",
                        a[0] ? a : "?", b[0] ? b : "?", c[0] ? c : "-");
        } else if (strcmp(ev, "extension_status") == 0) {
            json_get_str(json, "extension", a, 64);
            json_get_str(json, "status", b, 64);
            append_logf(L"分机状态: %hs → %hs", a[0] ? a : "?", b[0] ? b : "?");
        } else if (strcmp(ev, "ivrdtmf") == 0) {
            json_get_str(json, "caller", a, 64);
            json_get_str(json, "dtmf", b, 64);
            append_logf(L"IVR 按键: 分机 %hs 按键 %hs", a[0] ? a : "?", b[0] ? b : "?");
        } else if (strcmp(ev, "hangup") == 0) {
            json_get_str(json, "callid", a, 64);
            append_logf(L"通话结束 (callid=%hs)", a[0] ? a : "-");
            g_last_callid[0] = 0;
            g_last_channelid[0] = 0;
        } else if (strcmp(ev, "callstatus") == 0) {
            json_get_str(json, "extension", a, 64);
            json_get_str(json, "status", b, 64);
            json_get_str(json, "callid", c, 64);
            append_logf(L"通话状态: 分机 %hs %hs (callid=%hs)",
                        a[0] ? a : "?", b[0] ? b : "?", c[0] ? c : "-");
        } else if (ev[0]) {
            append_logf(L"事件: %hs", ev);
        } else {
            append_logf(L"收到未知事件报文 (详见 debug 日志)");
        }
        return;
    }
    if (strstr(json, "deviceinfo")) {               /* 设备信息 */
        char name[128] = {0}, sn[64] = {0};
        json_get_str(json, "devicename", name, 128);
        json_get_str(json, "sn", sn, 64);
        append_logf(L"设备信息: 名称=%hs 序列号=%hs",
                    name[0] ? name : "-", sn[0] ? sn : "-");
        return;
    }

    /* 其余按命令响应处理 */
    char callid[96] = {0}, channelid[96] = {0};
    json_get_str(json, "status", status, 64);
    json_get_str(json, "message", message, 256);
    json_get_str(json, "callid", callid, 96);
    json_get_str(json, "channelid", channelid, 96);
    if (callid[0]) {
        lstrcpynA(g_last_callid, callid, 95);
        if (g_hwnd) KillTimer(g_hwnd, IDT_DIALWAIT);
    }
    if (channelid[0]) lstrcpynA(g_last_channelid, channelid, 95);

    if (status[0] || message[0]) {
        wchar_t extra[160] = {0};
        if (callid[0]) wsprintfW(extra, L"  callid=%hs", callid);
        append_logf(L"响应: status=%hs message=%hs%ls",
                    status[0] ? status : "-",
                    message[0] ? message : "-",
                    extra);
    } else {
        wchar_t *w = to_w(json, 160);
        append_logf(L"收到报文 [%hs]: %ls", topic, w);
        free(w);
    }
}

/* ------------------------------------------------------------------ */
/* UI 操作                                                             */
/* ------------------------------------------------------------------ */

static void on_connect(void) {
    read_settings_from_ui();
    if (G.host[0] == 0) {
        MessageBoxW(g_hwnd, L"请填写 MQTT 地址 (Broker IP)", APP_TITLE, MB_ICONWARNING);
        return;
    }
    if (G.token[0] == 0) {
        MessageBoxW(g_hwnd, L"请填写 Token (默认为设备序列号, 在 PBX 管理页面 MQTT 设置中查看)",
                    APP_TITLE, MB_ICONWARNING);
        return;
    }
    save_config();
    if (!g_run) {
        g_run = 1;
        append_logf(L"正在连接 %hs:%d ...", G.host, G.port);
    } else {
        g_force_reconnect = 1;
        append_log(L"参数已更新, 正在重新连接...");
    }
}

static void on_disconnect(void) {
    if (!g_run && !g_connected) return;
    g_run = 0;
    append_log(L"正在断开...");
    post_status(L"未连接");
}

static void on_dial(void) {
    wchar_t caller_w[256] = {0}, callee_w[128] = {0};
    GetWindowTextW(g_hCaller, caller_w, 255);
    GetWindowTextW(g_hCallee, callee_w, 127);
    wchar_t caller[128] = {0}, callee[128] = {0};
    caller_number(caller_w, caller, 128);
    sanitize_number(callee_w, callee, 128);
    if (caller[0] == 0) {
        append_log(L"拨号失败: 请选择或输入主叫分机");
        return;
    }
    if (callee[0] == 0) {
        append_log(L"拨号失败: 请输入被叫号码");
        return;
    }
    if (!g_connected) {
        append_log(L"拨号失败: MQTT 未连接");
        return;
    }
    G.autoanswer = (SendDlgItemMessageW(g_hwnd, IDC_AUTOANSWER, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    const char *aa = G.autoanswer ? "yes" : "no";
    char *ca = to_a(caller);
    char *cb = to_a(callee);
    char json[1024];
    sprintf(json,
        "{\"cmd\":\"dial\",\"request_id\":\"%08x\",\"caller\":\"%s\","
        "\"callee\":\"%s\",\"autoanswer\":\"%s\"}",
        (unsigned)GetTickCount(), ca, cb, aa);
    free(ca); free(cb);
    /* ini 里的 dial_extra 可追加字段, 例如 "dialpermission":"3149" */
    if (G.dial_extra[0]) {
        int len = (int)strlen(json);
        if (len > 0 && json[len - 1] == '}') {
            json[len - 1] = 0;
            strncat(json, ",", sizeof(json) - strlen(json) - 1);
            strncat(json, G.dial_extra, sizeof(json) - strlen(json) - 1);
            strncat(json, "}", sizeof(json) - strlen(json) - 1);
        }
    }
    char topic[160];
    build_topic(topic, sizeof(topic), G.topic_call, G.token);
    if (!enqueue(topic, json)) {
        append_log(L"拨号失败: 发送队列已满");
        return;
    }
    append_logf(L"拨号命令已发送: %ls → %ls", caller, callee);
    if (g_hwnd) SetTimer(g_hwnd, IDT_DIALWAIT, 12000, NULL);
    /* 记住主叫; 被叫进历史下拉框 (最近的在最前, 自动去重) */
    {
        char *keep = to_a(callee);
        int found = -1;
        for (int i = 0; i < G.hist_n; i++)
            if (!strcmp(G.hist[i], keep)) { found = i; break; }
        if (found > 0) {
            char tmp[40];
            lstrcpynA(tmp, G.hist[found], 39);
            for (int i = found; i > 0; i--) lstrcpynA(G.hist[i], G.hist[i - 1], 39);
            lstrcpynA(G.hist[0], tmp, 39);
        } else if (found < 0) {
            int n = (G.hist_n < MAX_HIST) ? G.hist_n : MAX_HIST - 1;
            for (int i = n; i > 0; i--) lstrcpynA(G.hist[i], G.hist[i - 1], 39);
            lstrcpynA(G.hist[0], keep, 39);
            G.hist_n = n + 1;
        }
        free(keep);
        if (g_hCallee && IsWindow(g_hCallee)) {
            SendMessageW(g_hCallee, CB_RESETCONTENT, 0, 0);
            for (int i = 0; i < G.hist_n; i++) {
                wchar_t *w = to_w(G.hist[i], 64);
                SendMessageW(g_hCallee, CB_ADDSTRING, 0, (LPARAM)w);
                free(w);
            }
            SendMessageW(g_hCallee, CB_SETCURSEL, (WPARAM)-1, 0);
            SetWindowTextW(g_hCallee, callee);   /* 拨完保留刚拨的号码, 方便再拨 */
        }
        save_config();
    }
    SetFocus(g_hCallee);
}

/* [刷新分机] 按钮: 重新请求一次分机列表 (不用断开重连) */
static void on_refresh(void) {
    if (!g_connected) {
        append_log(L"刷新分机列表失败: MQTT 未连接");
        return;
    }
    char topic[192];
    build_topic(topic, sizeof(topic), G.topic_system, G.token);
    char json[160];
    sprintf(json, "{\"cmd\":\"extension_list\",\"request_id\":\"%08x\"}",
            (unsigned)GetTickCount());
    if (!enqueue(topic, json)) {
        append_log(L"刷新分机列表失败: 发送队列已满");
        return;
    }
    append_log(L"已请求分机列表 (extension_list)");
    if (g_hwnd) SetTimer(g_hwnd, IDT_EXTWAIT, 12000, NULL);
}

/* [诊断] 按钮: 把 ini 里 test_topic / test_payload 的内容发出去, 用于在
   现场试探正确的主题与报文格式 (默认发 deviceinfo 请求) */
static void on_diag(void) {
    if (!g_connected) {
        append_log(L"诊断发送失败: MQTT 未连接");
        return;
    }
    char topic[192];
    if (G.test_topic[0]) build_topic(topic, sizeof(topic), G.test_topic, G.token);
    else                 build_topic(topic, sizeof(topic), G.topic_system, G.token);

    char payload[768];
    if (G.test_payload[0]) {
        lstrcpynA(payload, G.test_payload, 767);
    } else {
        sprintf(payload, "{\"cmd\":\"deviceinfo\",\"request_id\":\"%08x\"}",
                (unsigned)GetTickCount());
    }
    if (!enqueue(topic, payload)) {
        append_log(L"诊断发送失败: 发送队列已满");
        return;
    }
    append_logf(L"诊断: 已发送到 %hs", topic);
    append_logf(L"诊断: 内容 %hs", payload);
    append_log(L"诊断: 等待响应, 详细报文见 pbxdialer_debug.log");
}

/* [复制日志] 按钮: 把 debug 日志 (尾部) 放进剪贴板, 方便直接粘贴给技术支持 */
static void on_copylog(void) {
    wchar_t path[512];
    dbg_path(path, 512);
    FILE *fp = _wfopen(path, L"rb");
    if (!fp) {
        append_log(L"没有找到 pbxdialer_debug.log (尚未产生收发记录)");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    long keep = size > 40000 ? 40000 : size;
    fseek(fp, size - keep, SEEK_SET);
    if (keep > 0 && fgetc(fp) != EOF) { /* 跳过可能被截断的半行 */ }
    unsigned char *buf = (unsigned char *)malloc((size_t)keep + 1);
    long got = 0;
    if (buf) {
        got = (long)fread(buf, 1, (size_t)keep, fp);
        buf[got] = 0;
    }
    fclose(fp);
    if (!buf) return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char *)buf, (int)got, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, (const char *)buf, (int)got, w, wlen);
        w[wlen] = 0;
        /* 剪贴板常被输入法等程序短暂占用, 重试若干次 */
        BOOL opened = FALSE;
        for (int i = 0; i < 20 && !opened; i++) {
            opened = OpenClipboard(g_hwnd);
            if (!opened) Sleep(50);
        }
        if (opened) {
            EmptyClipboard();
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)wlen + 1) * sizeof(wchar_t));
            if (hg) {
                void *dst = GlobalLock(hg);
                if (dst) {
                    memcpy(dst, w, ((size_t)wlen + 1) * sizeof(wchar_t));
                    GlobalUnlock(hg);
                    SetClipboardData(CF_UNICODETEXT, hg);
                }
            }
            CloseClipboard();
            append_logf(L"诊断日志已复制到剪贴板 (%d 字符), 可直接粘贴发送", wlen);
        } else {
            /* 剪贴板被其它程序占用时, 提示用户手动打开日志文件 */
            append_log(L"剪贴板被其它程序占用, 请再点一次 [复制日志]");
            append_log(L"或直接打开同目录的 pbxdialer_debug.log, 全选复制后发送");
        }
        free(w);
    }
    free(buf);
}

static void on_hangup(void) {
    if (!g_connected) {
        append_log(L"挂机失败: MQTT 未连接");
        return;
    }
    const char *cid = g_last_channelid[0] ? g_last_channelid : g_last_callid;
    if (cid[0] == 0) {
        append_log(L"挂机失败: 没有可挂断的通话 (尚未收到 dial 响应)");
        return;
    }
    char json[512];
    sprintf(json, "{\"cmd\":\"hangup\",\"request_id\":\"%08x\",\"callid\":\"%s\"}",
            (unsigned)GetTickCount(), cid);
    char topic[160];
    build_topic(topic, sizeof(topic), G.topic_call, G.token);
    if (!enqueue(topic, json)) {
        append_log(L"挂机失败: 发送队列已满");
        return;
    }
    append_logf(L"挂机命令已发送 (callid=%hs)", cid);
}

/* ------------------------------------------------------------------ */
/* 窗口                                                                */
/* ------------------------------------------------------------------ */

/* ---------------- 回车拨号 ---------------- */

/* 被叫框: 回车直接拨号 */
static LRESULT CALLBACK CalleeKeyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) { on_dial(); return 0; }
    return CallWindowProcW(g_oldCalleeProc, h, m, w, l);
}

/* 主叫框: 回车 -> 被叫为空就跳到被叫框, 否则直接拨 */
static LRESULT CALLBACK CallerKeyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        wchar_t buf[8] = {0};
        GetWindowTextW(g_hCallee, buf, 8);
        if (buf[0]) on_dial();
        else SetFocus(g_hCallee);
        return 0;
    }
    return CallWindowProcW(g_oldCallerEditProc, h, m, w, l);
}

static HWND mk_ctrl(const wchar_t *cls, const wchar_t *text, DWORD style,
                    int x, int y, int w, int h, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, cls, text,
                           WS_CHILD | WS_VISIBLE | style,
                           SC(x), SC(y), SC(w), SC(h),
                           g_hwnd, (HMENU)(INT_PTR)id, g_hInst, NULL);
}

static HWND mk_ctrl_plain(const wchar_t *cls, const wchar_t *text, DWORD style,
                          int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, text,
                           WS_CHILD | WS_VISIBLE | style,
                           SC(x), SC(y), SC(w), SC(h),
                           g_hwnd, (HMENU)(INT_PTR)id, g_hInst, NULL);
}

static void create_ui(void) {
    /* 连接设置 */
    mk_ctrl_plain(L"BUTTON", L"连接设置", BS_GROUPBOX, 10, 8, 760, 118, 0);
    mk_ctrl_plain(L"STATIC", L"MQTT 地址:", 0, 22, 32, 80, 20, 0);
    g_hHost  = mk_ctrl(L"EDIT", L"", ES_AUTOHSCROLL, 110, 29, 170, 24, IDC_HOST);
    mk_ctrl_plain(L"STATIC", L"端口:", 0, 296, 32, 36, 20, 0);
    g_hPort  = mk_ctrl(L"EDIT", L"1883", ES_AUTOHSCROLL, 338, 29, 70, 24, IDC_PORT);
    mk_ctrl_plain(L"STATIC", L"Token:", 0, 424, 32, 50, 20, 0);
    g_hToken = mk_ctrl(L"EDIT", L"", ES_AUTOHSCROLL, 478, 29, 282, 24, IDC_TOKEN);
    mk_ctrl_plain(L"STATIC", L"用户名:", 0, 22, 64, 60, 20, 0);
    g_hUser  = mk_ctrl(L"EDIT", L"", ES_AUTOHSCROLL, 110, 61, 170, 24, IDC_USER);
    mk_ctrl_plain(L"STATIC", L"密码:", 0, 296, 64, 36, 20, 0);
    g_hPass  = mk_ctrl(L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD, 338, 61, 422, 24, IDC_PASS);
    g_hConnect = mk_ctrl_plain(L"BUTTON", L"连  接", BS_PUSHBUTTON, 110, 92, 90, 28, IDC_CONNECT);
    g_hDisconnect = mk_ctrl_plain(L"BUTTON", L"断  开", BS_PUSHBUTTON, 212, 92, 90, 28, IDC_DISCONNECT);
    g_hStatus = mk_ctrl_plain(L"STATIC", L"状态: 未连接", 0, 320, 97, 440, 20, IDC_STATUS);

    /* 拨号 */
    mk_ctrl_plain(L"BUTTON", L"拨号", BS_GROUPBOX, 10, 134, 760, 118, 0);
    mk_ctrl_plain(L"STATIC", L"主叫分机:", 0, 22, 158, 70, 20, 0);
    g_hCaller = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                CBS_DROPDOWN,
                                SC(110), SC(155), SC(180), SC(240),
                                g_hwnd, (HMENU)(INT_PTR)IDC_CALLER, g_hInst, NULL);
    mk_ctrl_plain(L"STATIC", L"被叫号码:", 0, 316, 158, 66, 20, 0);
    g_hCallee = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                SC(386), SC(155), SC(210), SC(300),
                                g_hwnd, (HMENU)(INT_PTR)IDC_CALLEE, g_hInst, NULL);
    g_hAuto = mk_ctrl_plain(L"BUTTON", L"自动接通",
                            BS_AUTOCHECKBOX, 616, 159, 110, 20, IDC_AUTOANSWER);
    g_hDial   = mk_ctrl_plain(L"BUTTON", L"拨  号", BS_PUSHBUTTON, 110, 194, 130, 34, IDC_DIAL);
    g_hHangup = mk_ctrl_plain(L"BUTTON", L"挂  机", BS_PUSHBUTTON, 256, 194, 130, 34, IDC_HANGUP);
    if (G.show_diag) {
        /* 只有 ini 里 show_diag=1 才显示这两个按钮 */
        g_hDiag    = mk_ctrl_plain(L"BUTTON", L"诊  断", BS_PUSHBUTTON, 402, 194, 100, 34, IDC_DIAG);
        g_hRefresh = mk_ctrl_plain(L"BUTTON", L"刷新分机", BS_PUSHBUTTON, 510, 194, 110, 34, IDC_REFRESH);
        g_hCopyLog = mk_ctrl_plain(L"BUTTON", L"复制日志", BS_PUSHBUTTON, 628, 194, 130, 34, IDC_COPYLOG);
    } else {
        g_hRefresh = mk_ctrl_plain(L"BUTTON", L"刷新分机", BS_PUSHBUTTON, 402, 194, 130, 34, IDC_REFRESH);
    }

    /* 日志 */
    mk_ctrl_plain(L"BUTTON", L"状态日志", BS_GROUPBOX, 10, 260, 760, 292, 0);
    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                             SC(20), SC(272), SC(740), SC(272),
                             g_hwnd, (HMENU)(INT_PTR)IDC_LOG, g_hInst, NULL);
}

static void apply_font(HWND h, HFONT f) {
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_LOG: {
        wchar_t *s = (wchar_t *)lParam;
        if (s) { append_log(s); free(s); }
        return 0;
    }
    case WM_APP_STATUS: {
        wchar_t *s = (wchar_t *)lParam;
        if (s) {
            wchar_t buf[256];
            wsprintfW(buf, L"状态: %s", s);
            SetWindowTextW(g_hStatus, buf);
            free(s);
        }
        return 0;
    }
    case WM_APP_PKT: {
        char *s = (char *)lParam;
        if (s) { handle_packet(s); free(s); }
        return 0;
    }
    case WM_APP_EXTWAIT:
        SetTimer(hwnd, IDT_EXTWAIT, 12000, NULL);
        return 0;
    case WM_APP_DIAG: {
        wchar_t *s = (wchar_t *)lParam;
        if (s) { append_log(s); free(s); }
        return 0;
    }
    case WM_TIMER:
        if (wParam == IDT_EXTWAIT) {
            KillTimer(hwnd, IDT_EXTWAIT);
            append_log(L"提示: 12 秒内未收到分机列表响应。请确认 Token 与 PBX 后台一致；");
            append_log(L"      详细收发报文见同目录 pbxdialer_debug.log");
        } else if (wParam == IDT_DIALWAIT) {
            KillTimer(hwnd, IDT_DIALWAIT);
            append_log(L"提示: 12 秒内未收到拨号响应。若电话未响，请把 pbxdialer_debug.log 发给技术支持");
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CONNECT:    on_connect();    return 0;
        case IDC_DISCONNECT: on_disconnect(); return 0;
        case IDC_DIAL:       on_dial();       return 0;
        case IDC_HANGUP:     on_hangup();     return 0;
        case IDC_DIAG:       on_diag();       return 0;
        case IDC_REFRESH:    on_refresh();    return 0;
        case IDC_COPYLOG:    on_copylog();    return 0;
        case IDC_AUTOANSWER: save_config();   return 0;   /* 勾选变化立即写入 ini */
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nShow) {
    (void)hPrev; (void)cmdLine;
    g_hInst = hInst;

    /* 单实例: 避免同时开两个窗口争抢同一个 ini 和 MQTT 客户端 ID */
    HANDLE hOnce = CreateMutexW(NULL, FALSE, L"Global\\pbxdialer_single_instance");
    if (hOnce && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(CLASS_NAME, APP_TITLE);
        if (old) { ShowWindow(old, SW_RESTORE); SetForegroundWindow(old); }
        CloseHandle(hOnce);
        return 0;
    }

    /* DPI 感知 (Win10 1703+), 失败则按系统缩放 */
    typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
    SetDpiCtxFn setCtx = (SetDpiCtxFn)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (setCtx) setCtx((HANDLE)-4);  /* PER_MONITOR_AWARE_V2 */

    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);
    InitializeCriticalSection(&g_cs);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    /* DPI: 先创建窗口再取 (per-monitor v2 下创建时已知) */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(0, CLASS_NAME, APP_TITLE,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             0, 0, 0, 0,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    UINT dpi = 0;
    typedef UINT (WINAPI *GetDpiFn)(HWND);
    GetDpiFn getDpi = (GetDpiFn)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (getDpi) dpi = getDpi(g_hwnd);
    if (dpi == 0) dpi = 96;
    g_sf = (float)dpi / 96.0f;

    int w = SC(786), h = SC(572);
    SetWindowPos(g_hwnd, NULL, (sw - w) / 2, (sh - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    /* 字体 */
    g_font = CreateFontW(-SC(12), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
    g_fontLog = CreateFontW(-SC(12), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    /* 先读配置: show_diag 决定要不要创建 [诊断]/[复制日志] 按钮 */
    load_config();
    create_ui();
    /* 回车拨号: 给被叫框/主叫框装键盘处理 */
    if (g_hCallee) {
        /* 组合框的焦点在编辑子窗口上, 钩它才能拦到回车 */
        COMBOBOXINFO ci;
        memset(&ci, 0, sizeof(ci));
        ci.cbSize = sizeof(ci);
        if (GetComboBoxInfo(g_hCallee, &ci) && ci.hwndItem)
            g_oldCalleeProc = (WNDPROC)SetWindowLongPtrW(ci.hwndItem, GWLP_WNDPROC,
                                                         (LONG_PTR)CalleeKeyProc);
    }
    if (g_hCaller) {
        /* 注意: 只给组合框的编辑子窗口装钩子。直接给 COMBOBOX 本身 SetWindowLongPtr
           会导致 SetWindowTextW 失效(主叫回填不出来), 而焦点本来就在编辑子窗口上。 */
        COMBOBOXINFO ci;
        memset(&ci, 0, sizeof(ci));
        ci.cbSize = sizeof(ci);
        if (GetComboBoxInfo(g_hCaller, &ci) && ci.hwndItem)
            g_oldCallerEditProc = (WNDPROC)SetWindowLongPtrW(ci.hwndItem, GWLP_WNDPROC,
                                                             (LONG_PTR)CallerKeyProc);
    }
    apply_font(g_hHost, g_font);    apply_font(g_hPort, g_font);
    apply_font(g_hUser, g_font);    apply_font(g_hPass, g_font);
    apply_font(g_hToken, g_font);   apply_font(g_hStatus, g_font);
    apply_font(g_hCallee, g_font);  apply_font(g_hAuto, g_font);
    apply_font(g_hConnect, g_font); apply_font(g_hDisconnect, g_font);
    apply_font(g_hDial, g_font);    apply_font(g_hHangup, g_font);
    apply_font(g_hRefresh, g_font); /* 这三个之前漏了, 会退回系统默认字体, 和别的按钮不一致 */
    apply_font(g_hDiag, g_font);    apply_font(g_hCopyLog, g_font);
    apply_font(g_hCaller, g_font);
    apply_font(g_hLog, g_fontLog);

    /* 填充界面 */
    fill_ui_from_settings();
    save_config();      /* 补齐/规范化 ini 键 (含 caller / callee_history) */

    append_log(L"PBX 拨号器启动。填写 MQTT 地址和 Token 后点击 [连接]。");
    append_log(L"若已保存配置, 程序将自动连接。");

    /* 启动 MQTT 工作线程, 自动连接 */
    if (G.host[0] && G.token[0]) {
        g_run = 1;
        append_logf(L"正在连接 %hs:%d ...", G.host, G.port);
    }
    HANDLE hThread = CreateThread(NULL, 0, mqtt_thread, NULL, 0, NULL);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_stop = 1;
    if (hThread) {
        WaitForSingleObject(hThread, 1500);
        CloseHandle(hThread);
    }
    DeleteCriticalSection(&g_cs);
    WSACleanup();
    return 0;
}