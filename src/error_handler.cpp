/*
 * error_handler：错误数据队列与错误处理线程实现。
 * 使用 CRITICAL_SECTION + CONDITION_VARIABLE 实现线程安全的生产者-消费者。
 */

#include "error_handler.hpp"
#include "number_parser.hpp"
#include "system_utils.hpp"
#include <stdio.h>
#include <string.h>

/* ── 全局变量定义 ──────────────────────────────────────── */
volatile LONG g_running = 1;

/* 错误数据队列（环形缓冲区，多线程共享） */
static char      s_queue[ERROR_Q_SIZE][ERROR_LINE_MAX];
static int       s_head = 0;
static int       s_tail = 0;
static int       s_count = 0;
static CRITICAL_SECTION s_mutex;
static CONDITION_VARIABLE s_cv;

void ErrorHandler_init(void)
{
    InitializeCriticalSection(&s_mutex);
    InitializeConditionVariable(&s_cv);
}

void ErrorHandler_cleanup(void)
{
    DeleteCriticalSection(&s_mutex);
}

/* 唤醒错误处理线程（数据源退出时调用，避免线程卡在 SleepConditionVariableCS） */
void ErrorHandler_wake(void)
{
    WakeConditionVariable(&s_cv);
}

/* 串口读取线程 → 入队回调 */
void errorQueuePush(const char *line, void *context)
{
    (void)context;
    EnterCriticalSection(&s_mutex);
    if (s_count < ERROR_Q_SIZE)
    {
        strncpy_s(s_queue[s_tail], ERROR_LINE_MAX, line, _TRUNCATE);
        s_tail = (s_tail + 1) % ERROR_Q_SIZE;
        s_count++;
    }
    LeaveCriticalSection(&s_mutex);
    WakeConditionVariable(&s_cv);
}

/*
 * 错误数据处理线程：从队列取出数据，解析并记录异常日志。
 * 遇到无法解析的行则直接输出到控制台。
 */
DWORD WINAPI errorHandlerThreadFunc(LPVOID arg)
{
    (void)arg;
    while (g_running)
    {
        EnterCriticalSection(&s_mutex);
        while (s_count == 0 && g_running)
            SleepConditionVariableCS(&s_cv, &s_mutex, 500);

        while (s_count > 0 && g_running)
        {
            char line[ERROR_LINE_MAX];
            strncpy_s(line, sizeof(line), s_queue[s_head], _TRUNCATE);
            s_head = (s_head + 1) % ERROR_Q_SIZE;
            s_count--;
            LeaveCriticalSection(&s_mutex);

            SensorData data;
            if (tryParseSensorData(line, &data))
            {
                printf("[异常数据] 温度: %.1f℃  湿度: %.1f%%"
                       "  土壤: %.1f%%  光照: %.1flux\n",
                       data.temperature, data.humidity,
                       data.soilMoisture, data.lightIntensity);
            }
            else
            {
                logMessage("[异常上报] ");
                logMessage(line);
            }

            EnterCriticalSection(&s_mutex);
        }
        LeaveCriticalSection(&s_mutex);
    }
    return 0;
}
