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

/* ── WinSock2 全局初始化（进程内只做一次） ──────────────── */
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

    /* DNS 解析 + 连接 */
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
 * 线程安全地发送一行文本。
 * CRITICAL_SECTION 保护 send 不被心跳和主线程同时调用。
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
 *
 * 设计目的：
 *   服务端会持续推送实时数据，不能每次 recv 都新建连接。
 *   用一个独立线程循环读取，按行协议拆包分发给回调。
 *
 * 粘包处理：
 *   TCP 是流式传输，一次 recv 可能收到多条数据（粘包），
 *   也可能只收到半条（拆包）。所以需要缓冲区暂存：
 *
 *   新数据 recv 到来
 *     ↓
 *   追加到 buffer 尾部
 *     ↓
 *   从 buffer 头部逐行处理（遇 \n 截断）
 *     ↓
 *   剩余未处理数据 memmove 到 buffer 头部
 *
 * 行协议格式：
 *   DATA:25.3 55.1 42 813   → 传感器数据（4 个数值）
 *   ALERT:温度异常           → 报警文本
 *   PONG                     → 心跳回复（忽略）
 *   CONNECTED               → 服务端确认连接
 */
static DWORD WINAPI recvLoopFunc(LPVOID arg)
{
    ServerClient* sc = (ServerClient*)arg;
    char buffer[4096];          /* 部分接收缓冲区（处理粘包） */
    char temp[4096];            /* recv 临时缓冲区 */
    int bufLen = 0;

    while (sc->running && sc->connected)
    {
        /* 阻塞读取，超时后 recv 返回 -1，WSAGetLastError = WSAETIMEDOUT */
        int received;
        if (sc->sock == INVALID_SOCKET) break;
        received = recv(sc->sock, temp, sizeof(temp) - 1, 0);

        if (received > 0)
        {
            /* ── 新数据追加到缓冲区尾部 ── */
            if (bufLen + received >= (int)sizeof(buffer)) bufLen = 0;  /* 溢出保护 */
            memcpy(buffer + bufLen, temp, received);
            bufLen += received;
            buffer[bufLen] = '\0';

            /* ── 从缓冲区头部逐行解析 ── */
            char* p = buffer;
            char* nl;
            while ((nl = strchr(p, '\n')) != NULL)
            {
                *nl = '\0';
                char* line = p;
                size_t llen = strlen(line);
                if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';  /* 兼容 \r\n */

                if (strcmp(line, "PONG") == 0) { /* 心跳回复，无需处理 */ }
                else if (strcmp(line, "CONNECTED") == 0)
                    printf("[ServerClient] 服务端确认连接\n");
                else if (strncmp(line, "DATA:", 5) == 0)
                {
                    /* DATA: 后面跟 4 个数字，解析后通过回调推给数据线程 */
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
                    sc->alertCb(line + 6, sc->context);  /* 报警文本直接传给回调 */

                p = nl + 1;  /* 移到下一行头部 */
            }

            /* ── 未处理完的残片移到缓冲区头部 ── */
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
            /* 服务端正常关闭连接，recv 返回 0 */
            printf("[ServerClient] 服务端断开连接\n");
            break;
        }
        else
        {
            /* 超时是正常的（recv 设置了 5 秒超时），继续等 */
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;
            if (sc->running) fprintf(stderr, "[ServerClient] recv 错误: %d\n", err);
            break;
        }
    }
    sc->connected = 0;  /* 通知外部连接已断开 */
    return 0;
}

/*
 * ── 心跳线程（每 10 秒一次） ───────────────────────────────
 *
 * 目的：
 *   路由器/运营商/NAT 网关会踢掉长时间空闲的 TCP 连接。
 *   每 10 秒发一个 PING 包维持连接活跃，服务器回复 PONG。
 *   心跳和正常数据发送共用同一个 socket，用 CRITICAL_SECTION
 *   防止并发 send 导致数据交叉。
 */
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

    /* 建立底层 TCP 连接 */
    int s = createSocket(host, port);
    if (s < 0) return 0;

    sc->sock = (SOCKET)s;
    sc->running = 1;
    sc->connected = 1;

    sendLine(sc, "CONNECT");    /* 通知服务端身份 */

    /* 启动接收和心跳两个后台线程 */
    sc->recvThread = CreateThread(NULL, 0, recvLoopFunc, sc, 0, NULL);
    sc->heartbeatThread = CreateThread(NULL, 0, heartbeatLoopFunc, sc, 0, NULL);

    return 1;
}

/*
 * ── 断开连接 ──────────────────────────────────────────────
 *
 * 优雅关闭流程：
 *   1. 设置标志位（让线程自行退出循环）
 *   2. shutdown + closesocket（让 recv 立即返回，不等待线程超时）
 *   3. WaitForSingleObject 等线程结束（最多 3 秒）
 *   4. CloseHandle 释放线程句柄
 *   如果不先关闭 socket 直接 Wait，线程可能卡在 recv 上直到 5 秒超时。
 */
void ServerClient_disconnect(ServerClient* sc)
{
    sc->running = 0;
    sc->connected = 0;

    /* 先关 socket 让 recv 立刻返回，再 join 线程 */
    EnterCriticalSection(&sc->sockMutex);
    if (sc->sock != INVALID_SOCKET)
    {
        shutdown(sc->sock, SD_BOTH);      /* 禁止收发 */
        closesocket(sc->sock);             /* 释放 socket 资源 */
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

/*
 * ── 同步查询历史日志 ──────────────────────────────────────
 *
 * 数据线程每分钟调用一次，拉取当日所有日志重建全量图表。
 * 建立临时 TCP 连接，发送 GET_LOGS 请求，阻塞等待直到
 * 收到 LOGS_END 结束标记为止。
 *
 * 响应特点：
 *   日志行可能非常多（数千行），响应大小不定。
 *   使用 32KB 静态缓冲区接收，一般足够。
 *   收到后过滤掉协议标记行（LOGS_BEGIN/END、ANOMALIES 等），
 *   只保留纯日志行给调用方。
 */
int ServerClient_fetchLogs(ServerClient* sc, const char* begin, const char* end,
                           char outLines[][256], int* outCount, int maxLines)
{
    *outCount = 0;
    int s = createSocket(sc->host, sc->port);
    if (s < 0) return 0;
    SOCKET sock = (SOCKET)s;

    /* 构造请求：GET_LOGS begin=2026-05-14T00:00:00 end=2026-05-14T10:00:00 */
    char req[512];
    snprintf(req, sizeof(req), "GET_LOGS begin=%s end=%s", begin, end);
    char fullReq[520];
    snprintf(fullReq, sizeof(fullReq), "%s\n", req);
    send(sock, fullReq, (int)strlen(fullReq), 0);

    /* 持续接收直到遇到 LOGS_END 标记 */
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
        /* 循环收直到收完或遇到结束标记 */
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

    /* ── 按行解析，过滤协议标记行 ── */
    char* line = response;
    char* nl;
    while ((nl = strchr(line, '\n')) != NULL && *outCount < maxLines)
    {
        *nl = '\0';
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';

        /* 跳过 CONNECTED、LOGS_BEGIN/END、ANOMALIES_BEGIN/END 等协议行 */
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
