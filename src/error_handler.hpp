#pragma once

/*
 * error_handler：错误数据队列与错误处理线程
 *
 * 错误数据通过 errorQueuePush 推入环型队列，
 * errorHandlerThreadFunc 线程从中取出并处理。
 */

#include <windows.h>

/* 错误队列容量 */
#define ERROR_Q_SIZE    64
#define ERROR_LINE_MAX  512

/* ── 全局运行标志（其他模块也引用） ────────────────────── */
extern volatile LONG g_running;

/* ── 队列操作 ──────────────────────────────────────────── */

// 初始化同步原语（main 启动时调用）
void ErrorHandler_init(void);

// 清理同步原语（main 退出时调用）
void ErrorHandler_cleanup(void);

// 错误数据入队
void errorQueuePush(const char *line, void *context);

// 错误数据处理线程入口（CreateThread 使用）
DWORD WINAPI errorHandlerThreadFunc(LPVOID arg);

// 唤醒错误处理线程（数据源退出时调用，防止线程卡在等待中）
void ErrorHandler_wake(void);
