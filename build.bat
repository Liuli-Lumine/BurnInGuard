@echo off
setlocal
cd /d "%~dp0"

where g++ >nul 2>&1
if errorlevel 1 (
    echo [错误] 没找到 g++，请先安装 MinGW-w64 并加入 PATH
    exit /b 1
)
where windres >nul 2>&1
if errorlevel 1 (
    echo [错误] 没找到 windres，请先安装 MinGW-w64 并加入 PATH
    exit /b 1
)

echo [1/2] 编译资源...
pushd src
rem -c 65001: 资源脚本是 UTF-8，里面含中文说明
windres -c 65001 -i BurnInGuard.rc -o BurnInGuard_res.o
if errorlevel 1 (popd & exit /b 1)

echo [2/2] 编译程序...
g++ -std=c++17 -O2 -municode -mwindows -static -Wall ^
    -o "%~dp0BurnInGuard.exe" BurnInGuard.cpp BurnInGuard_res.o ^
    -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32
if errorlevel 1 (popd & exit /b 1)
del BurnInGuard_res.o >nul 2>&1
popd

echo.
echo 完成: %~dp0BurnInGuard.exe
