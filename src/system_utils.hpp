#pragma once

/*
 * system_utils：系统公共工具模块
 *
 * 存放所有模块共享的全局变量指针和公共工具函数。
 * 各模式函数（runXxxMode）在启动时设置这些全局指针，
 * 供子线程和回调函数访问。
 */

#include "sensor_display.hpp"
#include "analysis_rules.hpp"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 全局变量（各模块共享） ────────────────────────────── */
extern SensorDisplay *g_display;
extern AnalysisRules *g_rules;

/* ── 模式切换 ──────────────────────────────────────────── */
extern volatile int g_currentMode;       // 1=演示, 0=服务端
extern volatile int g_modeTogglePending; // UI 请求切换

/* ── 服务端地址 ──────────────────────────────────────────── */
#define SERVER_HOST "8.137.48.225"
#define SERVER_PORT 9090

/* ── 工具函数 ──────────────────────────────────────────── */

// 输出一行到控制台 + SFML 日志区
void logMessage(const char *msg);

// 输出传感器数据到控制台 + 更新 SFML 仪表盘
void processData(const SensorData *data);

// 指令字符 → 中文标签
const char *cmdLabel(char cmd);

// ISO 时间字符串（用于服务端日志查询）
void toIsoString(time_t t, char *buf, int bufSize);

// 极简 TCP 指令发送：单连接 → CMD:x → 200ms → 关闭
int sendCmdRaw(const char *host, int port, char cmd);

// 向服务端查询最新一条数据：GET_LATEST → 返回值写入 line
int fetchLatestRaw(const char *host, int port, char *line, int maxLen);

// 检查并处理手动指令队列（各模式函数共用）
void processManualCommands(void);

// 5 分钟分析点：feedData → checkUrgent → 发指令 → 推送 UI
void runAnalysisPoint(SensorData *data);

// UI 线程处理手动指令（含 'T' 触发拉取最新数据）
void processUiCommands(void);

#ifdef __cplusplus
}
#endif
