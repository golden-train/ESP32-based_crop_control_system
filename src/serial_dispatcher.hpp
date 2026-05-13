#pragma once

#include <windows.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <functional>

// 线程安全的串口行队列 + 后台读取线程
// 解决 ESP32 两种上传场景的并发问题：
//   1. PC 发 "0" → ESP32 正常回传（由主线程消费）
//   2. ESP32 检测到错误数据自发上传（由错误处理线程消费）
class SerialDispatcher
{
public:
    // 错误数据回调类型
    using ErrorCallback = std::function<void(const std::string &)>;

    SerialDispatcher();
    ~SerialDispatcher();

    // 启动后台读取线程（串口必须已打开）
    void start(HANDLE serialHandle, ErrorCallback onError);

    // 停止并等待线程退出
    void stop();

    // 标记正在等待正常响应（必须在 writeText 之前调用，防竞态）
    void expectResponse();

    // 取消等待标记（writeText 失败时回滚）
    void clearExpectResponse();

    // 阻塞等待正常响应行（调用前必须先调 expectResponse()）
    // 超时返回空字符串
    std::string waitResponse(int timeoutMs = 3000);

private:
    void readerLoop(HANDLE serialHandle);

    std::queue<std::string> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::atomic<bool> m_expectingResponse{false}; // 主线程是否在等待正常响应
    std::atomic<bool> m_running{false};           // 读取线程运行标志

    std::thread m_readerThread;
    ErrorCallback m_errorCallback;
};
