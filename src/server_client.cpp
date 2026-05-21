/*
 * 服务端 TCP 通信客户端。
 *
 * 与服务端（server_logger.py）通信实现：
 * - 长连接模式：接收实时数据推送 + 发送指令 + 日志查询
 * - 接收线程解析行协议（DATA:/ALERT:/PONG 等）
 * - 心跳线程每 10 秒发送 PING 保持连接
 *
 * 必须：winsock2.h 先于 windows.h
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "server_client.hpp"
#include "number_parser.hpp"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

static int s_wsaInit = 0;

/* 确保 WinSock2 已初始化（只执行一次） */
static int ensureWsa()
{
    if (s_wsaInit) return 1;
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 0;
    s_wsaInit = 1;
    return 1;
}

/*
 * ── TCP 连接工具 ──────────────────────────────────────────
 * 创建 TCP 连接，设置 5 秒收发超时，返回 socket 描述符。
 * 失败返回 -1。
 */
static int createSocket(const char* host, int port)
{
    if (!ensureWsa()) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    struct hostent* he = gethostbyname(host);
    if (!he) { closesocket(s); return -1; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    /* 5 秒超时，避免网络问题导致线程卡死 */
    int timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { closesocket(s); return -1; }
    return (int)s;
}

/*
 * 线程安全地向 TCP 连接发送一行文本。
 * CRITICAL_SECTION 保护 send 操作不被心跳和主线程同时调用。
 */
static int sendLine(ServerClient* sc, const char* line)
{
    EnterCriticalSection(&sc->sockMutex);
    if (sc->sock == INVALID_SOCKET) { LeaveCriticalSection(&sc->sockMutex); return 0; }
    char buf[512];
    snprintf(buf, sizeof(buf), "%s\n", line);
    int ret = send(sc->sock, buf, (int)strlen(buf), 0) > 0 ? 1 : 0;
    LeaveCriticalSection(&sc->sockMutex);
    return ret;
}

/* ── 初始化 ──────────────────────────────────────────────── */
void ServerClient_init(ServerClient* sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->sock = INVALID_SOCKET;
    sc->port = 9090;
    InitializeCriticalSection(&sc->sockMutex);
    ensureWsa();
}

/*
 * ── TCP 接收线程 ──────────────────────────────────────────
 * 持续读取 TCP 数据，解析行协议：
 *   PONG       → 跳过（心跳回复）
 *   CONNECTED  → 确认连接成功
 *   DATA:xxx   → 提取 4 个传感器数值，调用数据回调
 *   ALERT:xxx  → 调用报警回调
 *
 * 使用 memmove 管理部分接收缓冲区（处理粘包）。
 */
static DWORD WINAPI recvLoopFunc(LPVOID arg)
{
    ServerClient* sc = (ServerClient*)arg;
    char buffer[4096];          /* 部分接收缓冲区（处理粘包） */
    char temp[4096];            /* recv 临时缓冲区 */
    int bufLen = 0;

    while (sc->running && sc->connected)
    {
        int received;
        if (sc->sock == INVALID_SOCKET) break;
        received = recv(sc->sock, temp, sizeof(temp) - 1, 0);

        if (received > 0)
        {
            /* 追加到部分接收缓冲区 */
            if (bufLen + received >= (int)sizeof(buffer)) bufLen = 0;
            memcpy(buffer + bufLen, temp, received);
            bufLen += received;
            buffer[bufLen] = '\0';

            /* 逐行处理 */
            char* p = buffer;
            char* nl;
            while ((nl = strchr(p, '\n')) != NULL)
            {
                *nl = '\0';
                char* line = p;
                size_t llen = strlen(line);
                if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';

                if (strcmp(line, "PONG") == 0) { /* 心跳回复，忽略 */ }
                else if (strcmp(line, "CONNECTED") == 0)
                    printf("[ServerClient] 服务端确认连接\n");
                else if (strncmp(line, "DATA:", 5) == 0)
                {
                    /* 提取 DATA: 后面的数值 */
                    double vals[4];
                    int n = extractNumbers(line + 5, vals, 4);
                    if (n >= 4 && sc->dataCb)
                    {
                        SensorData d;
                        d.temperature = vals[0];
                        d.humidity = vals[1];
                        d.soilMoisture = vals[2];
                        d.lightIntensity = vals[3];
                        sc->dataCb(&d, sc->context);
                    }
                }
                else if (strncmp(line, "ALERT:", 6) == 0 && sc->alertCb)
                    sc->alertCb(line + 6, sc->context);

                p = nl + 1;
            }

            /* 将剩余未处理数据移到缓冲区开头 */
            if (p != buffer)
            {
                int remaining = bufLen - (int)(p - buffer);
                if (remaining > 0)
                    memmove(buffer, p, remaining);
                bufLen = remaining;
            }
        }
        else if (received == 0)
        {
            printf("[ServerClient] 服务端断开连接\n");
            break;
        }
        else
        {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;   /* 超时正常，继续等待 */
            if (sc->running) fprintf(stderr, "[ServerClient] recv 错误: %d\n", err);
            break;
        }
    }
    sc->connected = 0;
    return 0;
}

/* ── 心跳线程（每 10 秒 PING 一次） ──────────────────────── */
static DWORD WINAPI heartbeatLoopFunc(LPVOID arg)
{
    ServerClient* sc = (ServerClient*)arg;
    while (sc->running && sc->connected)
    {
        Sleep(10000);
        if (sc->running && sc->connected) sendLine(sc, "PING");
    }
    return 0;
}

/* ── 连接服务端 ──────────────────────────────────────────── */
int ServerClient_connect(ServerClient* sc, const char* host, int port)
{
    if (sc->connected) ServerClient_disconnect(sc);
    strncpy_s(sc->host, sizeof(sc->host), host, _TRUNCATE);
    sc->port = port;

    int s = createSocket(host, port);
    if (s < 0) return 0;

    sc->sock = (SOCKET)s;
    sc->running = 1;
    sc->connected = 1;

    sendLine(sc, "CONNECT");    /* 发送连接标识 */

    /* 启动接收和心跳线程 */
    sc->recvThread = CreateThread(NULL, 0, recvLoopFunc, sc, 0, NULL);
    sc->heartbeatThread = CreateThread(NULL, 0, heartbeatLoopFunc, sc, 0, NULL);

    return 1;
}

/* ── 断开连接 ────────────────────────────────────────────── */
void ServerClient_disconnect(ServerClient* sc)
{
    sc->running = 0;
    sc->connected = 0;

    /* 先关闭 socket 让 recv 立即返回，再 join 线程避免阻塞 */
    EnterCriticalSection(&sc->sockMutex);
    if (sc->sock != INVALID_SOCKET)
    {
        shutdown(sc->sock, SD_BOTH);
        closesocket(sc->sock);
        sc->sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&sc->sockMutex);

    if (sc->heartbeatThread)
    {
        WaitForSingleObject(sc->heartbeatThread, 3000);
        CloseHandle(sc->heartbeatThread);
        sc->heartbeatThread = NULL;
    }
    if (sc->recvThread)
    {
        WaitForSingleObject(sc->recvThread, 3000);
        CloseHandle(sc->recvThread);
        sc->recvThread = NULL;
    }
}

int ServerClient_isConnected(const ServerClient* sc) { return sc->connected; }

void ServerClient_setDataCallback(ServerClient* sc, ServerDataCallback cb, void* context)
{
    sc->dataCb = cb;
    sc->context = context;
}

void ServerClient_setAlertCallback(ServerClient* sc, ServerAlertCallback cb, void* context)
{
    sc->alertCb = cb;
}

/* ── 发送控制指令 ─────────────────────────────────────────── */
int ServerClient_sendCommand(ServerClient* sc, const char* cmd)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "CMD:%s", cmd);
    return sendLine(sc, buf);
}

/*
 * 极简指令发送：临时 TCP 连接 → 发送 CMD → 等 200ms → 关闭。
 * 无线程，无心跳，适用于偶尔发送指令的场景。
 */
int ServerClient_sendQuickCmd(const char* host, int port, const char* cmd)
{
    if (!ensureWsa()) return 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 0;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    struct hostent* he = gethostbyname(host);
    if (!he) { closesocket(s); return 0; }
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    { closesocket(s); return 0; }

    char req[256];
    snprintf(req, sizeof(req), "CMD:%s\n", cmd);
    send(s, req, (int)strlen(req), 0);
    Sleep(200);     /* 等待 TCP 数据发送完成再关闭 */
    closesocket(s);
    return 1;
}

/*
 * ── 同步查询历史日志 ──────────────────────────────────────
 *
 * 建立临时 TCP 连接，发送 GET_LOGS 请求，等待直到收到 LOGS_END 结束标记。
 * 响应可能较大（数千行），使用 32KB 静态缓冲区接收。
 * 成功后按行解析，跳过协议标记行（LOGS_BEGIN/END、CONNECTED 等）。
 */
int ServerClient_fetchLogs(ServerClient* sc, const char* begin, const char* end,
                           char outLines[][256], int* outCount, int maxLines)
{
    *outCount = 0;
    int s = createSocket(sc->host, sc->port);
    if (s < 0) return 0;
    SOCKET sock = (SOCKET)s;

    char req[512];
    snprintf(req, sizeof(req), "GET_LOGS begin=%s end=%s", begin, end);
    char fullReq[520];
    snprintf(fullReq, sizeof(fullReq), "%s\n", req);
    send(sock, fullReq, (int)strlen(fullReq), 0);

    /* 持续接收直到遇到 LOGS_END */
    char buf[4096];
    char response[32768];
    int respLen = 0;
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0)
    {
        buf[n] = '\0';
        if (respLen + n < (int)sizeof(response))
        {
            memcpy(response + respLen, buf, n);
            respLen += n;
        }
        while (strstr(response, "LOGS_END") == NULL)
        {
            n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            if (respLen + n < (int)sizeof(response))
            {
                memcpy(response + respLen, buf, n);
                respLen += n;
            }
        }
    }
    closesocket(sock);

    if (respLen == 0) return 0;
    response[respLen] = '\0';

    /* 逐行解析，过滤掉协议标记行 */
    char* line = response;
    char* nl;
    while ((nl = strchr(line, '\n')) != NULL && *outCount < maxLines)
    {
        *nl = '\0';
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';

        if (line[0] != '\0' &&
            strcmp(line, "LOGS_BEGIN") != 0 &&
            strcmp(line, "LOGS_END") != 0 &&
            strcmp(line, "ANOMALIES_BEGIN") != 0 &&
            strcmp(line, "ANOMALIES_END") != 0 &&
            strcmp(line, "CONNECTED") != 0)
        {
            strncpy_s(outLines[*outCount], 256, line, _TRUNCATE);
            (*outCount)++;
        }
        line = nl + 1;
    }

    return *outCount > 0;
}
