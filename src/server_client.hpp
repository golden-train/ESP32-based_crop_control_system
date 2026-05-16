#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

struct SensorData;

// 与服务端（server_logger.py）通信的 TCP 客户端
// 长连接模式：接收实时数据推送 + 发送指令 + 日志查询
class ServerClient
{
public:
    using DataCallback  = std::function<void(const SensorData&)>;
    using AlertCallback = std::function<void(const std::string&)>;

    ServerClient();
    ~ServerClient();

    // 连接服务端（启动接收线程 + 心跳线程）
    bool connect(const std::string &host, int port);

    // 断开连接
    void disconnect();

    bool isConnected() const { return m_connected; }

    // 设置实时数据回调
    void setDataCallback(DataCallback cb);
    void setAlertCallback(AlertCallback cb);

    // 发送控制指令到 ESP32
    bool sendCommand(const std::string &cmd);

    // 同步查询历史日志（临时独立连接）
    bool fetchLogs(const std::string &begin, const std::string &end,
                   std::vector<std::string> &outLines);

private:
    void receiveLoop();
    void heartbeatLoop();
    bool sendLine(const std::string &line);
    int createSocket(const std::string &host, int port);
    bool syncRequest(const std::string &request,
                     const std::string &endMarker,
                     std::vector<std::string> &outLines);

    std::string m_host;
    int m_port = 9090;
    int m_sock = -1;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::thread m_recvThread;
    std::thread m_heartbeatThread;
    DataCallback  m_dataCb;
    AlertCallback m_alertCb;
    mutable std::mutex m_sockMutex;
};
