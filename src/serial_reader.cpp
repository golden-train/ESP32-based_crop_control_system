#include "serial_reader.hpp"
#include <stdexcept>

SerialReader::SerialReader() : m_handle(INVALID_HANDLE_VALUE) {}

SerialReader::~SerialReader()
{
    close();
}

bool SerialReader::open(const std::string &portName, int baudRate)
{
    // 构造Windows串口路径: COM6 → \\.\COM6
    std::string fullPath = "\\\\.\\" + portName;

    m_handle = CreateFileA(
        fullPath.c_str(),
        GENERIC_READ | GENERIC_WRITE, // 读写权限
        0,                            // 独占模式
        nullptr,
        OPEN_EXISTING,                // 必须已存在
        0,
        nullptr);

    if (m_handle == INVALID_HANDLE_VALUE)
        return false;

    // 配置串口参数
    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(m_handle, &dcb))
    {
        close();
        return false;
    }

    dcb.BaudRate = static_cast<DWORD>(baudRate); // 波特率
    dcb.ByteSize = 8;                             // 8 位数据位
    dcb.Parity = NOPARITY;                        // 无校验
    dcb.StopBits = ONESTOPBIT;                    // 1 位停止位
    dcb.fDtrControl = DTR_CONTROL_DISABLE;        // 禁止 DTR（防止 ESP32 复位）
    dcb.fRtsControl = RTS_CONTROL_DISABLE;        // 禁止 RTS（防干扰）

    if (!SetCommState(m_handle, &dcb))
    {
        close();
        return false;
    }

    // 清空收发缓冲区，去除上次残留的脏数据
    PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    // 设置读取超时：立即返回，不等待
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = MAXDWORD;  // 字符间无间隔等待
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;    // 不阻塞等待
    SetCommTimeouts(m_handle, &timeouts);

    m_buffer.clear();
    return true;
}

void SerialReader::close()
{
    if (m_handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    m_buffer.clear();
}

bool SerialReader::isOpen() const
{
    return m_handle != INVALID_HANDLE_VALUE;
}

HANDLE SerialReader::nativeHandle() const
{
    return m_handle;
}

bool SerialReader::readLine(std::string &outLine)
{
    if (!isOpen())
        return false;

    char ch;
    DWORD bytesRead = 0;

    // 逐字节读取，直到遇到换行符
    while (ReadFile(m_handle, &ch, 1, &bytesRead, nullptr) && bytesRead > 0)
    {
        if (ch == '\n')
        {
            // 去掉末尾的回车符（\r）
            if (!m_buffer.empty() && m_buffer.back() == '\r')
                m_buffer.pop_back();

            outLine = std::move(m_buffer);
            m_buffer.clear();
            return true;
        }
        m_buffer += ch;
    }

    return false; // 暂无完整行
}

bool SerialReader::writeText(const std::string &text, bool &outError)
{
    outError = false;
    if (!isOpen())
    {
        outError = true;
        return false;
    }

    DWORD written = 0;
    if (!WriteFile(m_handle, text.c_str(),
                   static_cast<DWORD>(text.size()), &written, nullptr))
    {
        outError = true;
        return false;
    }
    return written == text.size();
}
