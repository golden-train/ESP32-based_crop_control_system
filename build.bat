@echo off
chcp 65001 >NUL
setlocal enabledelayedexpansion

set SRC=src
set OUT=output
set TEMP=temp
set SFML=C:\tool\SFML-2.6.2
set CXX=g++
set "CXXFLAGS=-std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -static-libgcc -static-libstdc++ -I%SFML%/include -I%SRC% -Wno-cpp"
set "LDFLAGS=-L%SFML%/lib -lsfml-graphics -lsfml-window -lsfml-system -lopengl32 -lgdi32 -lwinmm -lws2_32 -lwinhttp"

REM 先杀掉残留的 system.exe（避免链接器无法覆盖）
tasklist /FI "IMAGENAME eq system.exe" 2>NUL | find /I "system.exe" >NUL
if not errorlevel 1 (
    taskkill /f /im system.exe >NUL 2>&1
    timeout /t 1 /nobreak >NUL
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%TEMP%" mkdir "%TEMP%"

REM 清理旧的中间文件
del /Q "%OUT%\*.o" "%OUT%\system.exe" 2>NUL

echo ^=^=^> Compiling all source files...
for %%f in ("%SRC%\*.cpp") do (
    echo   %%~nxf
    %CXX% %CXXFLAGS% -c "%%f" -o "%OUT%\%%~nf.o"
    if !errorlevel! neq 0 (
        echo FAILED: %%~nf.cpp
        exit /b !errorlevel!
    )
)

echo ^=^=^> Linking %OUT%\system.exe...
%CXX% "%OUT%\*.o" -o "%OUT%\system.exe" %LDFLAGS%
if %errorlevel% neq 0 (
    echo LINK FAILED
    exit /b %errorlevel%
)

echo ^=^=^> Copying SFML DLLs...
copy "%SFML%\bin\sfml-graphics-2.dll" "%OUT%\" >NUL
copy "%SFML%\bin\sfml-window-2.dll"   "%OUT%\" >NUL
copy "%SFML%\bin\sfml-system-2.dll"   "%OUT%\" >NUL

echo ^=^=^> DONE: %OUT%\system.exe
