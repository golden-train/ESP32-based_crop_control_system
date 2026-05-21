#include "serial_dispatcher.hpp"
#include <string.h>

/*
 * 初始化串口分发器。
 * 清零所有字段，初始化同步原语（CRITICAL_SECTION + CONDITION_VARIABLE）。
 */
void SerialDispatcher_init(SerialDispatcher* disp)
{
    memset(disp, 0, sizeof(*disp));
    InitializeCriticalSection(&disp->mutex);
    InitializeConditionVariable(&disp->cv);
    disp->running = 0;
}

/*
 * 内部函数：向环形队列尾部推入一行文本。
 * 队列满时丢弃新数据而不阻塞（队列大小 DISPATCHER_QUEUE_SIZE = 16 够用）。
 */
static void queuePush(SerialDispatcher* disp, const char* line)
{
    if (disp->queueCount >= DISPATCHER_QUEUE_SIZE) return;
    strncpy_s(disp->queue[disp->queueTail], LINE_MAX, line, _TRUNCATE);
    disp->queueTail = (disp->queueTail + 1) % DISPATCHER_QUEUE_SIZE;
    disp->queueCount++;
}

/*
 * 内部函数：从环形队列头部弹出一行文本到 out。
 * 返回 1 = 有数据，0 = 队列空。
 */
static int queuePop(SerialDispatcher* disp, char* out, int maxLen)
{
    if (disp->queueCount == 0) return 0;
    strncpy_s(out, maxLen, disp->queue[disp->queueHead], _TRUNCATE);
    disp->queueHead = (disp->queueHead + 1) % DISPATCHER_QUEUE_SIZE;
    disp->queueCount--;
    return 1;
}

/*
 * 后台串口读取线程主循环。
 *
 * 持续调用 ReadFile 从串口读取数据，遇到 \n 则按当前状态分发：
 * - expectingResponse == 1 : 放入主队列，唤醒等待的主线程
 * - expectingResponse == 0 : 调用 errorCallback（推入错误队列）
 *
 * 这种设计解决了 ESP32 两种上传场景的并发问题：
 * 1. PC 发 "0" → ESP32 正常回传（由数据线程通过 waitResponse 消费）
 * 2. ESP32 检测到错误数据自发上传（由错误处理线程消费）
 */
static DWORD WINAPI readerLoopFunc(LPVOID arg)
{
    SerialDispatcher* disp = (SerialDispatcher*)arg;
    char buf[256];          /* ReadFile 的临时读取缓冲区 */
    char line[LINE_MAX];    /* 行累积缓冲区 */
    int lineLen = 0;

    while (disp->running)
    {
        DWORD bytesRead = 0;
        if (ReadFile(disp->serialHandle, buf, sizeof(buf) - 1, &bytesRead, NULL)
            && bytesRead > 0)
        {
            for (DWORD i = 0; i < bytesRead; i++)
            {
                char ch = buf[i];
                if (ch == '\n')
                {
                    /* 去掉末尾回车符，兼容 \r\n 和 \n 两种行尾 */
                    if (lineLen > 0 && line[lineLen - 1] == '\r')
                        lineLen--;

                    if (lineLen == 0) continue;

                    /* 截断超长行 */
                    if (lineLen >= LINE_MAX) lineLen = LINE_MAX - 1;
                    line[lineLen] = '\0';

                    if (disp->expectingResponse)
                    {
                        /* 正常响应 → 推入主队列，通知 waitResponse */
                        EnterCriticalSection(&disp->mutex);
                        queuePush(disp, line);
                        WakeConditionVariable(&disp->cv);
                        LeaveCriticalSection(&disp->mutex);
                    }
                    else
                    {
                        /* 非预期数据 → 错误回调（推入 g_errorQueue） */
                        if (disp->errorCb)
                            disp->errorCb(line, disp->errorCtx);
                    }
                    lineLen = 0;
                }
                else
                {
                    if (lineLen < LINE_MAX - 1)
                        line[lineLen++] = ch;
                }
            }
        }
        else
        {
            /* 无数据时短暂休眠，避免忙等空转 CPU */
            Sleep(5);
        }
    }
    return 0;
}

/*
 * 启动后台读取线程。
 * 必须在串口已打开的情况下调用（serialHandle 有效）。
 * onError 回调在串口读取线程中执行，注意回调内部必须线程安全。
 */
void SerialDispatcher_start(SerialDispatcher* disp, HANDLE serialHandle,
                            ErrorCallback onError, void* context)
{
    if (disp->running) return;

    disp->serialHandle = serialHandle;
    disp->errorCb = onError;
    disp->errorCtx = context;
    disp->running = 1;
    disp->readerThread = CreateThread(NULL, 0, readerLoopFunc, disp, 0, NULL);
}

/*
 * 停止读取线程并清理。
 * 设置 running = 0 通知线程退出，最多等待 3 秒后强制关闭。
 */
void SerialDispatcher_stop(SerialDispatcher* disp)
{
    if (!disp->running) return;
    disp->running = 0;
    WakeConditionVariable(&disp->cv);           /* 唤醒可能阻塞的等待者 */
    if (disp->readerThread)
    {
        WaitForSingleObject(disp->readerThread, 3000);
        CloseHandle(disp->readerThread);
        disp->readerThread = NULL;
    }
    DeleteCriticalSection(&disp->mutex);
}

/* 标记下一行串口数据为"正常响应"，必须在 writeText 之前调用 */
void SerialDispatcher_expectResponse(SerialDispatcher* disp)
{
    disp->expectingResponse = 1;
}

/*
 * 取消"正常响应"标记。
 * 当 writeText 失败时调用，防止后续错误数据被错误路由到主队列。
 */
void SerialDispatcher_clearExpectResponse(SerialDispatcher* disp)
{
    disp->expectingResponse = 0;
}

/*
 * 阻塞等待正常响应行。
 *
 * 调用前必须已经调用了 expectResponse() + writeText()。
 * 内部使用 CONDITION_VARIABLE 等待（可被 WakeConditionVariable 提前唤醒），
 * 超过 timeoutMs 仍未收到响应则超时返回 0。
 *
 * 返回值：1 = 成功获取响应行，0 = 超时或线程退出。
 */
int SerialDispatcher_waitResponse(SerialDispatcher* disp, int timeoutMs,
                                  char* outLine, int maxLen)
{
    EnterCriticalSection(&disp->mutex);

    while (disp->queueCount == 0 && disp->running)
    {
        /*
         * SleepConditionVariableCS 在等待期间释放锁，
         * 被唤醒后自动重新获取锁。返回 FALSE 表示超时。
         */
        if (!SleepConditionVariableCS(&disp->cv, &disp->mutex, timeoutMs))
            break;  /* 超时 */
    }

    int got = 0;
    if (disp->queueCount > 0)
        got = queuePop(disp, outLine, maxLen);

    LeaveCriticalSection(&disp->mutex);
    disp->expectingResponse = 0;    /* 无论是否超时，清除标记 */
    return got;
}
