#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>

// MQTT 客户端：通过 Mosquitto CLI 工具实现发布/订阅
// 订阅在后台线程中运行，收到消息时调用回调
class MqttClient
{
public:
    using MessageCallback = std::function<void(const std::string &topic,
                                               const std::string &payload)>;

    MqttClient();
    ~MqttClient();

    // 连接 MQTT Broker 并启动订阅线程
    bool begin(const std::string &host, int port,
               const std::string &username = "",
               const std::string &password = "");

    // 停止订阅线程并断开
    void end();

    // 发布消息到指定主题
    bool publish(const std::string &topic, const std::string &payload);

    // 设置消息回调（在订阅线程中调用，注意线程安全）
    void setMessageCallback(MessageCallback cb);

    bool isRunning() const { return m_running; }

private:
    void subscriberThread();

    std::string m_host;
    std::string m_username;
    std::string m_password;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    MessageCallback m_callback;
};
