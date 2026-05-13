#include "serial_dispatcher.hpp"
#include <chrono>
#include <stdexcept>

SerialDispatcher::SerialDispatcher() = default;

SerialDispatcher::~SerialDispatcher()
{
    stop();
}

void SerialDispatcher::start(HANDLE serialHandle, ErrorCallback onError)
{
    if (m_running)
        return;

    m_errorCallback = std::move(onError);
    m_running = true;
    m_readerThread = std::thread(&SerialDispatcher::readerLoop, this, serialHandle);
}

void SerialDispatcher::stop()
{
    m_running = false;
    // 唤醒可能阻塞在条件变量上的读取线程
    m_cv.notify_all();
    if (m_readerThread.joinable())
        m_readerThread.join();
}

void SerialDispatcher::expectResponse()
{
    m_expectingResponse = true;
}

std::string SerialDispatcher::waitResponse()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    // 阻塞等待，直到队列中有数据
    m_cv.wait(lock, [this] {
        return !m_queue.empty() || !m_running;
    });

    if (!m_queue.empty())
    {
        std::string line = std::move(m_queue.front());
        m_queue.pop();
        m_expectingResponse = false;
        return line;
    }

    m_expectingResponse = false;
    return {};
}

void SerialDispatcher::readerLoop(HANDLE serialHandle)
{
    while (m_running)
    {
        // 逐字节读取，直到获得完整一行
        char ch;
        DWORD bytesRead = 0;
        std::string line;

        while (m_running)
        {
            if (ReadFile(serialHandle, &ch, 1, &bytesRead, nullptr) && bytesRead > 0)
            {
                if (ch == '\n')
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    break; // 读完一行
                }
                line += ch;
            }
            else
            {
                // 暂无数据，短暂休眠后重试
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (line.empty())
            continue;

        // 分发：主线程度在等待响应 → 放入队列由主线程消费
        if (m_expectingResponse)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push(std::move(line));
            m_cv.notify_one();
        }
        else
        {
            // 自发上传的错误数据 → 在读取线程中直接调用错误回调
            if (m_errorCallback)
                m_errorCallback(line);
        }
    }
}
