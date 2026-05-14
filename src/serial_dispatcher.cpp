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

void SerialDispatcher::clearExpectResponse()
{
    m_expectingResponse = false;
}

std::string SerialDispatcher::waitResponse(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    // 带超时等待，防止 ESP32 无响应时永久阻塞
    m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
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
    char buf[256];
    std::string line;

    while (m_running)
    {
        DWORD bytesRead = 0;
        if (ReadFile(serialHandle, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0)
        {
            for (DWORD i = 0; i < bytesRead; i++)
            {
                char ch = buf[i];
                if (ch == '\n')
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();

                    if (line.empty())
                        continue;

                    // 分发
                    if (m_expectingResponse)
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_queue.push(std::move(line));
                        m_cv.notify_one();
                    }
                    else
                    {
                        if (m_errorCallback)
                            m_errorCallback(line);
                    }
                    line.clear();
                }
                else
                {
                    line += ch;
                }
            }
        }
        else
        {
            // 无数据时短暂休眠，不空转
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}
