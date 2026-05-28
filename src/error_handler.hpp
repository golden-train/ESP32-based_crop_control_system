#pragma once

#include <windows.h>

/* 全局运行标志 */
extern volatile LONG g_running;

/* 初始化（仅创建同步原语） */
void ErrorHandler_init(void);

/* 清理 */
void ErrorHandler_cleanup(void);
