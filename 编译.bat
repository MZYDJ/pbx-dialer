@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

where gcc >nul 2>nul
if errorlevel 1 (
    echo [错误] 没找到 gcc。
    echo        请安装 MinGW-w64 ^(x86_64^), 并把它的 bin 目录加入 PATH,
    echo        或者直接使用已编译好的 拨号.exe。
    pause
    exit /b 1
)

echo 正在编译 dialer.c ...
gcc -O2 -municode -mwindows -static dialer.c -o 拨号.exe -lws2_32 -luser32 -lgdi32
if errorlevel 1 (
    echo [失败] 编译出错, 请把上面的信息发给维护者。
    pause
    exit /b 1
)

echo [完成] 已生成 拨号.exe
pause