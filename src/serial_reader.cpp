#include "serial_reader.hpp"
#include <stdio.h>
#include <string.h>

/*
 * 初始化串口读取器。
 * 将句柄设为 INVALID_HANDLE_VALUE 表示未打开，清空行缓冲区。
 */
void SerialReader_init(SerialReader* sr)
{
    sr->handle = INVALID_HANDLE_VALUE;
    sr->bufferLen = 0;
}

/*
 * 打开串口。
 *
 * Windows 串口路径规则：COM6 → \\.\COM6。对于 COM10 以上的端口，
 * 必须加 \\.\ 前缀才能被 CreateFile 识别。
 *
 * 关键配置：
 * - DTR/RTS 禁用：防止 Windows 打开串口时电平变化导致 ESP32 复位
 * - PurgeComm 调用：清空上次残留的脏数据
 * - ReadIntervalTimeout = MAXDWORD：ReadFile 立即返回，不阻塞
 *
 * 返回 1 成功，0 失败。
 */
int SerialReader_open(SerialReader* sr, const char* portName, int baudRate)
{
    /* 构造 Windows 串口路径: COM6 → \\.\COM6 */
    char fullPath[32];
    snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", portName);

    sr->handle = CreateFileA(
        fullPath,
        GENERIC_READ | GENERIC_WRITE,
        0,              /* 独占模式，不允许其他进程同时打开串口 */
        NULL,
        OPEN_EXISTING,  /* 必须已存在 —— 串口设备始终存在 */
        0,
        NULL);

    if (sr->handle == INVALID_HANDLE_VALUE)
        return 0;

    /* 获取当前 DCB（串口控制块），修改需要的参数后再写回 */
    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(sr->handle, &dcb))
    {
        SerialReader_close(sr);
        return 0;
    }

    /* 配置串口参数：115200, 8-N-1 */
    dcb.BaudRate = (DWORD)baudRate;
    dcb.ByteSize = 8;                           /* 8 位数据位 */
    dcb.Parity = NOPARITY;                      /* 无校验 */
    dcb.StopBits = ONESTOPBIT;                  /* 1 位停止位 */
    dcb.fDtrControl = DTR_CONTROL_DISABLE;      /* 禁止 DTR 防止 ESP32 复位 */
    dcb.fRtsControl = RTS_CONTROL_DISABLE;      /* 禁止 RTS 防干扰 */

    if (!SetCommState(sr->handle, &dcb))
    {
        SerialReader_close(sr);
        return 0;
    }

    /* 清空收发缓冲区，去除上次残留的脏数据 */
    PurgeComm(sr->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    /*
     * 设置读取超时：立即返回，不等待。
     * ReadIntervalTimeout = MAXDWORD 配合超时乘数/常数为 0 的行为是：
     * ReadFile 立即返回缓冲区中已有的数据，若无数据则返回 0。
     * 这样不会阻塞 SerialDispatcher 的读取循环。
     */
    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    SetCommTimeouts(sr->handle, &timeouts);

    sr->bufferLen = 0;
    return 1;
}

/*
 * 关闭串口。
 * 释放句柄，防止资源泄漏。清空内部缓冲区避免下次打开时混淆。
 */
void SerialReader_close(SerialReader* sr)
{
    if (sr->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(sr->handle);
        sr->handle = INVALID_HANDLE_VALUE;
    }
    sr->bufferLen = 0;
}

/* 返回 1 = 串口已打开，0 = 未打开 */
int SerialReader_isOpen(const SerialReader* sr)
{
    return sr->handle != INVALID_HANDLE_VALUE;
}

/* 返回原生串口句柄，供 SerialDispatcher 使用 */
HANDLE SerialReader_nativeHandle(const SerialReader* sr)
{
    return sr->handle;
}

/*
 * 阻塞读取一行数据（以 \n 结尾）。
 *
 * 逐字节读取直到遇到换行符，丢弃 \r（兼容 Windows \r\n 和 Unix \n）。
 * 内部循环累积字符到 buffer 中，读到完整行后拷贝到 outLine 并返回 1。
 * 若串口断开或无完整行，返回 0。
 */
int SerialReader_readLine(SerialReader* sr, char* outLine, int maxLen)
{
    if (!SerialReader_isOpen(sr))
        return 0;

    char ch;
    DWORD bytesRead = 0;

    while (ReadFile(sr->handle, &ch, 1, &bytesRead, NULL) && bytesRead > 0)
    {
        if (ch == '\n')
        {
            /* 去掉末尾的回车符（\r\n 兼容） */
            if (sr->bufferLen > 0 && sr->buffer[sr->bufferLen - 1] == '\r')
                sr->bufferLen--;

            sr->buffer[sr->bufferLen] = '\0';
            strncpy_s(outLine, maxLen, sr->buffer, _TRUNCATE);
            sr->bufferLen = 0;
            return 1;
        }
        /* 缓冲区未满则追加字符 */
        if (sr->bufferLen < (int)sizeof(sr->buffer) - 1)
            sr->buffer[sr->bufferLen++] = ch;
    }

    return 0;
}

/*
 * 向串口发送文本（用于向 ESP32 发送控制指令 "0"～"9"）。
 *
 * outError 输出参数指示是否发生错误（写中断、串口断开等）。
 * 返回 1 = 全部写入成功，0 = 写入不完全或失败。
 */
int SerialReader_writeText(SerialReader* sr, const char* text, int* outError)
{
    *outError = 0;
    if (!SerialReader_isOpen(sr))
    {
        *outError = 1;
        return 0;
    }

    DWORD written = 0;
    DWORD len = (DWORD)strlen(text);
    if (!WriteFile(sr->handle, text, len, &written, NULL))
    {
        *outError = 1;
        return 0;
    }
    return written == len;
}
