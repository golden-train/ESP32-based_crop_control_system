#pragma once

#include <windows.h>

// 串口读取器：负责打开 COM 口，逐行读取数据
typedef struct {
    HANDLE handle;
    char buffer[1024];  // 接收缓冲区，累积字符直到遇到换行符
    int   bufferLen;
} SerialReader;

// 初始化
void SerialReader_init(SerialReader* sr);

// 打开串口，portName 如 "COM6"，baudRate 如 115200
int SerialReader_open(SerialReader* sr, const char* portName, int baudRate);

// 关闭串口
void SerialReader_close(SerialReader* sr);

// 是否已打开（返回 1 已打开，0 未打开）
int SerialReader_isOpen(const SerialReader* sr);

// 获取原生串口句柄
HANDLE SerialReader_nativeHandle(const SerialReader* sr);

// 阻塞读取一行（返回 1 表示成功读到一行，0 暂无完整行）
int SerialReader_readLine(SerialReader* sr, char* outLine, int maxLen);

// 发送文本（用于向 ESP32 发送控制指令）
int SerialReader_writeText(SerialReader* sr, const char* text, int* outError);
