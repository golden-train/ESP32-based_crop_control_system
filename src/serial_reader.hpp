#pragma once

#include <string>
#include <functional>
#include <windows.h>

// 串口读取器：负责打开COM口，逐行读取数据
// 回调模式：每接收到完整一行文本，调用一次回调函数
class SerialReader
{
public:
    // 行回调：参数为接收到的完整一行文本（不含换行符）
    using LineCallback = std::function<void(const std::string &)>;

    SerialReader();
    ~SerialReader();

    // 打开串口，portName 如 "COM6"，baudRate 如 115200
    bool open(const std::string &portName, int baudRate);

    // 关闭串口
    void close();

    // 是否已打开
    bool isOpen() const;

    // 获取原生串口句柄（供 SerialDispatcher 使用）
    HANDLE nativeHandle() const;

    // 阻塞读取一行（返回 true 表示成功读到一行）
    bool readLine(std::string &outLine);

    // 发送文本（用于向ESP32发送控制指令）
    bool writeText(const std::string &text, bool &outError);

private:
    HANDLE m_handle;
    std::string m_buffer; // 接收缓冲区，累积字符直到遇到换行符
};
