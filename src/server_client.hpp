#pragma once

#include <windows.h>
#include "sensor_data.hpp"

// 数据接收回调
typedef void (*ServerDataCallback)(const SensorData* data, void* context);
typedef void (*ServerAlertCallback)(const char* alert, void* context);

// 与服务端（server_logger.py）通信的 TCP 客户端
typedef struct {
    char   host[128];
    int    port;
    SOCKET sock;
    volatile LONG running;
    volatile LONG connected;
    HANDLE recvThread;
    HANDLE heartbeatThread;
    CRITICAL_SECTION sockMutex;
    ServerDataCallback  dataCb;
    ServerAlertCallback alertCb;
    void* context;
} ServerClient;

// 初始化
void ServerClient_init(ServerClient* sc);

// 连接服务端（启动接收线程 + 心跳线程）
int ServerClient_connect(ServerClient* sc, const char* host, int port);

// 断开连接
void ServerClient_disconnect(ServerClient* sc);

// 是否已连接
int ServerClient_isConnected(const ServerClient* sc);

// 设置实时数据回调
void ServerClient_setDataCallback(ServerClient* sc, ServerDataCallback cb, void* context);
void ServerClient_setAlertCallback(ServerClient* sc, ServerAlertCallback cb, void* context);

// 发送控制指令
int ServerClient_sendCommand(ServerClient* sc, const char* cmd);

// 极简指令发送：临时连接→发CMD→等200ms→关闭
int ServerClient_sendQuickCmd(const char* host, int port, const char* cmd);

// 同步查询历史日志（临时独立连接）
int ServerClient_fetchLogs(ServerClient* sc, const char* begin, const char* end,
                           char outLines[][256], int* outCount, int maxLines);
