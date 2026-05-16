#include "mqtt_client.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>

static const char *MOSQ_DIR = "C:\\Program Files\\Mosquitto\\";

MqttClient::MqttClient() = default;

MqttClient::~MqttClient()
{
    end();
}

bool MqttClient::begin(const std::string &host, int port,
                       const std::string &username,
                       const std::string &password)
{
    if (m_running)
        return true;

    m_host     = host;
    m_username = username;
    m_password = password;

    std::string testCmd = std::string(MOSQ_DIR) + "mosquitto_pub --help >nul 2>&1";
    if (system(testCmd.c_str()) != 0)
    {
        std::cerr << "[MQTT] 未找到 Mosquitto 客户端工具" << std::endl;
        return false;
    }

    m_running = true;
    m_thread = std::thread(&MqttClient::subscriberThread, this);
    return true;
}

void MqttClient::end()
{
    if (m_running)
    {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
    }
}

void MqttClient::setMessageCallback(MessageCallback cb)
{
    m_callback = std::move(cb);
}

bool MqttClient::publish(const std::string &topic, const std::string &payload)
{
    std::string cmd = std::string(MOSQ_DIR) + "mosquitto_pub -h "
                      + m_host + " -t \"" + topic + "\" -m \""
                      + payload + "\"";
    if (!m_username.empty())
        cmd += " -u \"" + m_username + "\" -P \"" + m_password + "\"";
    cmd += " >nul 2>&1";

    int ret = system(cmd.c_str());
    return (ret == 0);
}

void MqttClient::subscriberThread()
{
    std::string cmd = std::string(MOSQ_DIR) + "mosquitto_sub -h "
                      + m_host + " -t \"esp32/data\" -t \"esp32/alert\""
                      + " -F \"%t\\t%p\"";
    if (!m_username.empty())
        cmd += " -u \"" + m_username + "\" -P \"" + m_password + "\"";
    cmd += " 2>nul";

    FILE *pipe = _popen(cmd.c_str(), "r");
    if (!pipe)
    {
        std::cerr << "[MQTT] 无法启动 mosquitto_sub" << std::endl;
        m_running = false;
        return;
    }

    char line[1024];
    while (m_running && fgets(line, sizeof(line), pipe))
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0 || !m_callback)
            continue;

        char *tabPos = strchr(line, '\t');
        if (!tabPos)
            continue;

        *tabPos = '\0';
        std::string topic(line);
        std::string payload(tabPos + 1);

        if (!topic.empty())
            m_callback(topic, payload);
    }

    _pclose(pipe);
    m_running = false;
}
