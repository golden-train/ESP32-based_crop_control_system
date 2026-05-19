#include "server_client.hpp"
#include "sensor_data.hpp"
#include "number_parser.hpp"
#include <iostream>
#include <cstring>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static bool s_wsaInit = false;

// ============================================================ //
//                        Winsock 初始化                         //
// ============================================================ //
static bool ensureWsa()
{
    if (s_wsaInit) return true;
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
    s_wsaInit = true;
    return true;
}

// ============================================================ //
//                     TCP 连接工具                              //
// ============================================================ //
int ServerClient::createSocket(const std::string &host, int port)
{
    if (!ensureWsa()) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    struct hostent *he = gethostbyname(host.c_str());
    if (!he) { closesocket(s); return -1; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (::connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { closesocket(s); return -1; }
    return static_cast<int>(s);
}

bool ServerClient::sendLine(const std::string &line)
{
    std::lock_guard<std::mutex> lock(m_sockMutex);
    if (m_sock < 0) return false;
    std::string data = line + "\n";
    return ::send(static_cast<SOCKET>(m_sock), data.c_str(), (int)data.size(), 0) > 0;
}

// ============================================================ //
//                  连接 / 断开                                  //
// ============================================================ //
ServerClient::ServerClient() { ensureWsa(); }

ServerClient::~ServerClient() { disconnect(); }

bool ServerClient::connect(const std::string &host, int port)
{
    if (m_connected) disconnect();
    m_host = host; m_port = port;

    int s = createSocket(host, port);
    if (s < 0) return false;

    m_sock = s;
    m_running = true;
    m_connected = true;

    sendLine("CONNECT");

    m_recvThread = std::thread(&ServerClient::receiveLoop, this);
    m_heartbeatThread = std::thread(&ServerClient::heartbeatLoop, this);
    return true;
}

void ServerClient::disconnect()
{
    m_running = false;
    m_connected = false;

    // 先关闭 socket 让 recv 立即返回，再 join 线程避免阻塞
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        if (m_sock >= 0)
        {
            shutdown(static_cast<SOCKET>(m_sock), SD_BOTH);
            closesocket(static_cast<SOCKET>(m_sock));
            m_sock = -1;
        }
    }

    if (m_heartbeatThread.joinable()) m_heartbeatThread.join();
    if (m_recvThread.joinable()) m_recvThread.join();
}

// ============================================================ //
//                 回调设置                                      //
// ============================================================ //
void ServerClient::setDataCallback(DataCallback cb) { m_dataCb = std::move(cb); }
void ServerClient::setAlertCallback(AlertCallback cb) { m_alertCb = std::move(cb); }

// ============================================================ //
//                 接收线程                                      //
// ============================================================ //
void ServerClient::receiveLoop()
{
    std::string buffer;
    char temp[4096];

    while (m_running && m_connected)
    {
        int received;
        if (m_sock < 0) break;
        received = ::recv(static_cast<SOCKET>(m_sock), temp, sizeof(temp) - 1, 0);

        if (received > 0)
        {
            temp[received] = '\0';
            buffer += temp;

            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos)
            {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                if (line == "PONG") continue;
                else if (line == "CONNECTED")
                    std::cout << "[ServerClient] 服务端确认连接" << std::endl;
                else if (line.substr(0, 5) == "DATA:")
                {
                    std::string payload = line.substr(5);
                    std::vector<double> vals = extractNumbers(payload);
                    if (vals.size() >= 4 && m_dataCb)
                    {
                        SensorData d;
                        d.temperature = vals[0]; d.humidity = vals[1];
                        d.soilMoisture = vals[2]; d.lightIntensity = vals[3];
                        m_dataCb(d);
                    }
                }
                else if (line.substr(0, 6) == "ALERT:" && m_alertCb)
                    m_alertCb(line.substr(6));
            }
        }
        else if (received == 0)
        {
            std::cout << "[ServerClient] 服务端断开连接" << std::endl;
            break;
        }
        else
        {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;
            if (m_running) std::cerr << "[ServerClient] recv 错误: " << err << std::endl;
            break;
        }
    }
    m_connected = false;
}

void ServerClient::heartbeatLoop()
{
    while (m_running && m_connected)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (m_running && m_connected) sendLine("PING");
    }
}

// ============================================================ //
//                 同步请求（临时连接）                           //
// ============================================================ //
bool ServerClient::syncRequest(const std::string &request,
                               const std::string &endMarker,
                               std::vector<std::string> &outLines)
{
    outLines.clear();
    int s = createSocket(m_host, m_port);
    if (s < 0) return false;
    SOCKET sock = static_cast<SOCKET>(s);

    std::string req = request + "\n";
    ::send(sock, req.c_str(), (int)req.size(), 0);

    char buf[4096];
    std::string response;
    int n = ::recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) { buf[n] = '\0'; response = buf; }
    else { closesocket(sock); return false; }

    while (response.find(endMarker) == std::string::npos)
    {
        n = ::recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0'; response += buf;
    }
    closesocket(sock);

    std::istringstream stream(response);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();
        if (line == "LOGS_BEGIN" || line == "LOGS_END" ||
            line == "ANOMALIES_BEGIN" || line == "ANOMALIES_END" ||
            line == "CONNECTED") continue;
        outLines.push_back(line);
    }
    return !outLines.empty();
}

bool ServerClient::fetchLogs(const std::string &begin, const std::string &end,
                             std::vector<std::string> &outLines)
{
    return syncRequest("GET_LOGS begin=" + begin + " end=" + end, "LOGS_END", outLines);
}

bool ServerClient::sendCommand(const std::string &cmd)
{
    return sendLine("CMD:" + cmd);
}

bool ServerClient::sendQuickCmd(const std::string &host, int port,
                                const std::string &cmd)
{
    if (!ensureWsa()) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    struct hostent *he = gethostbyname(host.c_str());
    if (!he) { closesocket(s); return false; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    // 3 秒超时，避免卡死
    int timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (::connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { closesocket(s); return false; }

    // 发送 CMD:x 后等待 200ms 让 TCP 传输完成再关闭
    std::string req = "CMD:" + cmd + "\n";
    ::send(s, req.c_str(), (int)req.size(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    closesocket(s);
    return true;
}
