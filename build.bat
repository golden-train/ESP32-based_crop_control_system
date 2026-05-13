@echo off
chcp 65001 >NUL
setlocal enabledelayedexpansion

set SRC=src
set OUT=src\output
set SFML=C:\tool\SFML-2.6.2
set CXX=g++
set "CXXFLAGS=-std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -I%SFML%/include -I%SRC%"
set "LDFLAGS=-L%SFML%/lib -lsfml-graphics -lsfml-window -lsfml-system"

if not exist "%OUT%" mkdir "%OUT%"

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
