#pragma once

#include <windows.h>

#define DISPATCHER_QUEUE_SIZE 16
#define LINE_MAX 512

// 错误数据回调类型
typedef void (*ErrorCallback)(const char* line, void* context);

// 线程安全的串口行队列 + 后台读取线程
typedef struct {
    // 响应队列（环形缓冲区）
    char queue[DISPATCHER_QUEUE_SIZE][LINE_MAX];
    int  queueHead;
    int  queueTail;
    int  queueCount;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE cv;

    volatile LONG expectingResponse;  // 是否在等待正常响应
    volatile LONG running;            // 读取线程运行标志

    HANDLE   readerThread;
    HANDLE   serialHandle;
    ErrorCallback errorCb;
    void*    errorCtx;
} SerialDispatcher;

// 初始化
void SerialDispatcher_init(SerialDispatcher* disp);

// 启动后台读取线程（串口必须已打开）
void SerialDispatcher_start(SerialDispatcher* disp, HANDLE serialHandle,
                            ErrorCallback onError, void* context);

// 停止并等待线程退出
void SerialDispatcher_stop(SerialDispatcher* disp);

// 标记正在等待正常响应（必须在 writeText 之前调用）
void SerialDispatcher_expectResponse(SerialDispatcher* disp);

// 取消等待标记（writeText 失败时回滚）
void SerialDispatcher_clearExpectResponse(SerialDispatcher* disp);

// 阻塞等待正常响应行（超时返回空字符串，返回值 1=有数据, 0=超时）
int SerialDispatcher_waitResponse(SerialDispatcher* disp, int timeoutMs,
                                  char* outLine, int maxLen);
