#pragma once

/*
 * data_source：数据源模式入口
 *
 * 数据线程按优先级依次尝试：
 *   服务端 TCP → MQTT
 *
 * dataThreadFunc 作为线程入口，从最高优先级开始尝试，
 * 成功后循环运行，失败则降级到下一种模式。
 */

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// 数据线程入口（CreateThread 使用）
DWORD WINAPI dataThreadFunc(LPVOID arg);

#ifdef __cplusplus
}
#endif
