/*
 * ESP32 传感器数据监控系统 — 主入口
 *
 * 职责仅限于：初始化、启动数据线程、运行 UI 主循环、清理退出。
 * 所有业务逻辑均在独立模块中实现：
 *   data_source  —— 三种数据源模式（服务端/MQTT/串口）
 *   error_handler —— 错误队列与处理线程
 *   system_utils  —— 公共工具函数与全局变量
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "system_utils.hpp"
#include "error_handler.hpp"
#include "data_source.hpp"
#include "sensor_display.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

static int ensureWsa(void)
{
    static int inited = 0;
    if (inited) return 1;
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return 0;
    inited = 1;
    return 1;
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* 初始化同步原语与网络栈 */
    ErrorHandler_init();
    ensureWsa();

    /* 创建 UI 窗口 */
    SensorDisplay display;
    g_display = &display;

    /* 启动数据线程（含两种模式自动降级） */
    HANDLE dataThread = CreateThread(NULL, 0, dataThreadFunc, NULL, 0, NULL);

    /* UI 主循环 */
    while (display.isOpen() && g_running)
    {
        display.processEvents();
        display.render();

        processUiCommands();

        Sleep(16);   /* ≈ 60 FPS */
    }

    /* 清理 */
    g_running = 0;
    if (dataThread)
    {
        WaitForSingleObject(dataThread, 3000);
        CloseHandle(dataThread);
    }

    ErrorHandler_cleanup();
    return 0;
}
