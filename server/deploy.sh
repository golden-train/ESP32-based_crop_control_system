#!/bin/bash
# ESP32 服务端日志服务 部署脚本
# 在 Ubuntu 服务器上以 root 执行

set -e

echo "===== ESP32 Server Logger 部署 ====="

# 1. 安装依赖
echo "[1/4] 安装 Python 依赖..."
pip3 install paho-mqtt

# 2. 创建目录并复制文件
echo "[2/4] 创建 /opt/esp32-server..."
mkdir -p /opt/esp32-server
cp server_logger.py /opt/esp32-server/
cp requirements.txt /opt/esp32-server/
chmod +x /opt/esp32-server/server_logger.py

# 3. 安装 systemd 服务
echo "[3/4] 安装 systemd 服务..."
cp esp32-server-logger.service /etc/systemd/system/
systemctl daemon-reload

# 4. 启动并启用开机自启
echo "[4/4] 启动服务..."
systemctl enable esp32-server-logger
systemctl start esp32-server-logger
systemctl status esp32-server-logger

echo "===== 部署完成 ====="
