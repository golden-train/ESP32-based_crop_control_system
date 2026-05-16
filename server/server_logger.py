#!/usr/bin/env python3
"""
ESP32 传感器数据服务端日志服务 v2.1.0
==========================================
功能:
  - 通过 MQTT 订阅 esp32/data 和 esp32/alert
  - 每5分钟采样写入日志文件（7天轮转）
  - 异常数据即时写入日志
  - TCP 端口 9090 供 PC 客户端连接/数据转发

部署要求:
  pip install paho-mqtt

启动:
  python3 server_logger.py [--port 9090] [--mqtt-host 8.137.48.225]

开机自启(systemd):
  [Service]
  ExecStart=/usr/bin/python3 /path/to/server_logger.py
  Restart=always
  User=root
"""

import paho.mqtt.client as mqtt
import threading
import socket
import time
import os
import datetime
import sys
import argparse
import signal

# ============================================================ #
#                        配置区域                               #
# ============================================================ #
MQTT_HOST = "8.137.48.225"
MQTT_PORT = 1883
MQTT_USER = "esp32"
MQTT_PASSWORD = "20061001"

TCP_PORT = 9090
HEARTBEAT_TIMEOUT = 30        # PC 心跳超时(秒)
LOG_DIR = "logs"              # 日志目录
LOG_RETENTION_DAYS = 7        # 日志保留天数
SAMPLING_INTERVAL = 300       # 5分钟采样间隔(秒)

# ============================================================ #
#                        全局状态                               #
# ============================================================ #
pc_connected = False
pc_socket = None
pc_lock = threading.Lock()
last_sample_time = 0
last_anomaly_log_time = 0
latest_data = None
last_pc_heartbeat = time.time()

running = True

# ============================================================ #
#                        日志模块                               #
# ============================================================ #
def log_path(date=None):
    """获取当前日期的日志文件路径"""
    if date is None:
        date = datetime.date.today()
    return os.path.join(LOG_DIR, f"{date.isoformat()}.txt")


def log_write(line):
    """写入一行到当日日志文件（自动创建目录）"""
    os.makedirs(LOG_DIR, exist_ok=True)
    path = log_path()
    with open(path, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def log_data(temperature, humidity, soil_moisture, light_intensity):
    """记录一条正常数据（5分钟采样）"""
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {temperature:.1f} {humidity:.1f} {soil_moisture} {light_intensity:.0f}"
    log_write(line)


def log_anomaly(temperature, humidity, soil_moisture, light_intensity):
    """记录一条异常数据"""
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}][ANOMALY] {temperature:.1f} {humidity:.1f} {soil_moisture} {light_intensity:.0f}"
    log_write(line)
    print(f"    → 异常已记录")


def clean_old_logs():
    """清理超过7天的日志文件"""
    if not os.path.exists(LOG_DIR):
        return
    cutoff = datetime.date.today() - datetime.timedelta(days=LOG_RETENTION_DAYS)
    for fname in os.listdir(LOG_DIR):
        if not fname.endswith(".txt"):
            continue
        try:
            file_date = datetime.date.fromisoformat(fname.replace(".txt", ""))
            if file_date < cutoff:
                os.remove(os.path.join(LOG_DIR, fname))
                print(f"  [清理] 删除旧日志: {fname}")
        except ValueError:
            continue


def read_logs(begin_str, end_str):
    """读取指定时间范围内的日志行"""
    try:
        begin = datetime.datetime.fromisoformat(begin_str)
        end = datetime.datetime.fromisoformat(end_str)
    except ValueError:
        return []

    results = []
    # 遍历可能涉及的日志文件
    current = begin.date()
    end_date = end.date()
    while current <= end_date:
        path = log_path(current)
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    if len(line) < 20:
                        continue
                    try:
                        ts_str = line[1:20]  # "[2026-05-14 10:00:00]"
                        ts = datetime.datetime.strptime(ts_str, "%Y-%m-%d %H:%M:%S")
                        if begin <= ts <= end:
                            results.append(line.rstrip("\n"))
                    except (ValueError, IndexError):
                        continue
        current += datetime.timedelta(days=1)

    return results


def read_anomalies(begin_str, end_str):
    """读取指定时间范围内的异常日志行"""
    all_logs = read_logs(begin_str, end_str)
    return [line for line in all_logs if "[ANOMALY]" in line]


# ============================================================ #
#                     MQTT 回调                                 #
# ============================================================ #
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] 已连接到 {MQTT_HOST}:{MQTT_PORT}")
        # 使用通配符 # 订阅所有 esp32/ 开头的主题，避免独立订阅问题
        client.subscribe("esp32/#", qos=0)
        print("[MQTT] 已订阅 esp32/#")
    else:
        print(f"[MQTT] 连接失败, rc={rc}")


def on_disconnect(client, userdata, rc):
    print(f"[MQTT] 断开连接 rc={rc}，将在循环中自动重连")


def on_message(client, userdata, msg):
    global last_sample_time, latest_data

    topic = msg.topic
    payload = msg.payload.decode("utf-8").strip()

    if topic == "esp32/data":
        print(f"[收到] DATA: {payload}")
        latest_data = payload
        # 5分钟采样写入日志
        now = time.time()
        if now - last_sample_time >= SAMPLING_INTERVAL:
            last_sample_time = now
            values = payload.split()
            if len(values) >= 4:
                log_data(float(values[0]), float(values[1]),
                         int(float(values[2])), float(values[3]))
                print(f"[日志] {datetime.datetime.now().strftime('%H:%M:%S')} {payload}")

        # PC 在线时实时转发
        with pc_lock:
            if pc_connected and pc_socket:
                try:
                    pc_socket.sendall(f"DATA:{payload}\n".encode("utf-8"))
                    print(f"[转发] DATA → PC: {payload}")
                except Exception as e:
                    print(f"[转发错误] DATA 发送失败: {e}")

    elif topic == "esp32/alert":
        global last_anomaly_log_time
        clean_payload = payload[1:] if payload.startswith("!") else payload
        values = clean_payload.split()
        if len(values) >= 4:
            # 异常日志同样按5分钟采样记录
            now = time.time()
            if now - last_anomaly_log_time >= SAMPLING_INTERVAL:
                last_anomaly_log_time = now
                log_anomaly(float(values[0]), float(values[1]),
                            int(float(values[2])), float(values[3]))
                print(f"[异常] {datetime.datetime.now().strftime('%H:%M:%S')} {payload}")

        # PC 在线时转发异常
        with pc_lock:
            if pc_connected and pc_socket:
                try:
                    pc_socket.sendall(f"ALERT:{payload}\n".encode("utf-8"))
                except:
                    pass


# ============================================================ #
#                     TCP 服务端（PC 连接）                      #
# ============================================================ #
def handle_pc_client(client_sock):
    """处理 PC 客户端连接（运行在独立线程中）

    主连接：先发 CONNECTED，等 PC 发 CONNECT 确认后标记为主连接
    临时查询：发给 CONNECTED 后如果收到 GET_LOGS 等命令，处理完自动关闭
    """
    global pc_connected, pc_socket, last_pc_heartbeat

    addr = client_sock.getpeername()
    is_main_conn = False
    buffer = ""

    try:
        client_sock.sendall(b"CONNECTED\n")
        # 初始阻塞式等待 CONNECT（无超时）
        found_connect = False
        while not found_connect and running:
            try:
                data = client_sock.recv(4096)
            except:
                break
            if not data:
                break
            buffer += data.decode("utf-8")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                if line == "CONNECT":
                    is_main_conn = True
                    found_connect = True
                    with pc_lock:
                        pc_connected = True
                        pc_socket = client_sock
                        last_pc_heartbeat = time.time()
                    print(f"[TCP] PC 主连接已建立: {addr}")
                    break
                elif line.startswith("GET_LOGS") or line.startswith("GET_ANOMALIES") or line.startswith("GET_LATEST"):
                    # 临时查询：处理完即返回
                    if line.startswith("GET_LOGS"):
                        parts = line.split()
                        begin_str = end_str = ""
                        for p in parts[1:]:
                            if p.startswith("begin="): begin_str = p[6:]
                            elif p.startswith("end="): end_str = p[4:]
                        result = read_logs(begin_str, end_str)
                        try:
                            client_sock.sendall(b"LOGS_BEGIN\n")
                            for rl in result:
                                client_sock.sendall((rl + "\n").encode("utf-8"))
                            client_sock.sendall(b"LOGS_END\n")
                        except: pass
                    elif line.startswith("GET_ANOMALIES"):
                        parts = line.split()
                        begin_str = end_str = ""
                        for p in parts[1:]:
                            if p.startswith("begin="): begin_str = p[6:]
                            elif p.startswith("end="): end_str = p[4:]
                        result = read_anomalies(begin_str, end_str)
                        try:
                            client_sock.sendall(b"ANOMALIES_BEGIN\n")
                            for rl in result:
                                client_sock.sendall((rl + "\n").encode("utf-8"))
                            client_sock.sendall(b"ANOMALIES_END\n")
                        except: pass
                    elif line == "GET_LATEST":
                        # 返回最新一条数据（用于 PC 轮询）
                        resp = f"LATEST:{latest_data}\n" if latest_data else "LATEST:NODATA\n"
                        try:
                            client_sock.sendall(resp.encode("utf-8"))
                        except: pass
                    return  # 临时查询结束
                # 其他未知命令也按临时查询处理
                # 但先不管，循环继续等数据

        if not found_connect:
            return  # 没收到 CONNECT，直接关闭

        # 收到 CONNECT 后启用 5 秒超时用于心跳检测
        client_sock.settimeout(5.0)

        while running:
            try:
                data = client_sock.recv(4096)
            except socket.timeout:
                with pc_lock:
                    if time.time() - last_pc_heartbeat > HEARTBEAT_TIMEOUT:
                        print(f"[TCP] PC 心跳超时，断开连接")
                        break
                continue
            except:
                break

            if not data:
                break

            buffer += data.decode("utf-8")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                if not line:
                    continue

                # 心跳
                if line == "PING":
                    with pc_lock:
                        last_pc_heartbeat = time.time()
                    try:
                        client_sock.sendall(b"PONG\n")
                    except:
                        break
                    continue

                # 断开
                if line == "DISCONNECT":
                    print(f"[TCP] PC 主动断开: {addr}")
                    return

                # 获取日志（正常不应走到这里，但兼容处理）
                if line.startswith("GET_LOGS"):
                    parts = line.split()
                    begin_str = end_str = ""
                    for p in parts[1:]:
                        if p.startswith("begin="): begin_str = p[6:]
                        elif p.startswith("end="): end_str = p[4:]
                    logs = read_logs(begin_str, end_str)
                    try:
                        client_sock.sendall(b"LOGS_BEGIN\n")
                        for log_line in logs:
                            client_sock.sendall((log_line + "\n").encode("utf-8"))
                        client_sock.sendall(b"LOGS_END\n")
                    except:
                        break
                    continue

                # 获取异常
                if line.startswith("GET_ANOMALIES"):
                    parts = line.split()
                    begin_str = end_str = ""
                    for p in parts[1:]:
                        if p.startswith("begin="): begin_str = p[6:]
                        elif p.startswith("end="): end_str = p[4:]
                    anomalies = read_anomalies(begin_str, end_str)
                    try:
                        client_sock.sendall(b"ANOMALIES_BEGIN\n")
                        for al in anomalies:
                            client_sock.sendall((al + "\n").encode("utf-8"))
                        client_sock.sendall(b"ANOMALIES_END\n")
                    except:
                        break
                    continue

                # 发送命令到 ESP32
                if line.startswith("CMD:"):
                    cmd = line[4:]
                    print(f"[TCP] PC 下发指令: {cmd}")
                    mqtt_client.publish("esp32/cmd", cmd)
                    continue

    finally:
        if is_main_conn:
            with pc_lock:
                if pc_socket is client_sock:
                    pc_connected = False
                    pc_socket = None
        try:
            client_sock.close()
        except:
            pass
        print(f"[TCP] 连接已关闭: {addr}")


def tcp_server():
    """TCP 服务端主循环（多线程处理连接）"""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(("0.0.0.0", TCP_PORT))
        server.listen(5)
        server.settimeout(1.0)
        print(f"[TCP] 服务端启动，监听端口 {TCP_PORT}")
    except Exception as e:
        print(f"[TCP] 绑定端口失败: {e}")
        return

    while running:
        try:
            client, addr = server.accept()
            t = threading.Thread(target=handle_pc_client, args=(client,), daemon=True)
            t.start()
        except socket.timeout:
            continue
        except Exception as e:
            if running:
                print(f"[TCP] 接受连接错误: {e}")

    server.close()


# ============================================================ #
#                     MQTT 客户端（模块级引用）                   #
# ============================================================ #
mqtt_client = None


def start_mqtt():
    global mqtt_client
    mqtt_client = mqtt.Client(client_id="ServerLogger")
    mqtt_client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    mqtt_client.on_connect = on_connect
    mqtt_client.on_disconnect = on_disconnect
    mqtt_client.on_message = on_message

    # 自动重连
    mqtt_client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()
    return mqtt_client


# ============================================================ #
#                         入口                                  #
# ============================================================ #
def main():
    global running, TCP_PORT, MQTT_HOST, MQTT_PORT

    parser = argparse.ArgumentParser(description="ESP32 传感器数据服务端日志服务")
    parser.add_argument("--port", type=int, default=TCP_PORT, help=f"TCP 端口 (默认 {TCP_PORT})")
    parser.add_argument("--mqtt-host", default=MQTT_HOST, help=f"MQTT 主机 (默认 {MQTT_HOST})")
    parser.add_argument("--mqtt-port", type=int, default=MQTT_PORT, help=f"MQTT 端口 (默认 {MQTT_PORT})")
    args = parser.parse_args()

    TCP_PORT = args.port
    MQTT_HOST = args.mqtt_host
    MQTT_PORT = args.mqtt_port

    def signal_handler(sig, frame):
        global running
        print("\n[服务] 收到关闭信号，正在退出...")
        running = False
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print("=" * 50)
    print("  ESP32 传感器数据服务端日志服务 v2.1.0")
    print("=" * 50)
    print(f"  MQTT:     {MQTT_HOST}:{MQTT_PORT}")
    print(f"  TCP:      端口 {TCP_PORT}")
    print(f"  日志目录: {os.path.abspath(LOG_DIR)}")
    print(f"  日志保留: {LOG_RETENTION_DAYS} 天")
    print("=" * 50)

    # 清理旧日志
    clean_old_logs()

    # 定时器：每天凌晨清理旧日志
    def daily_cleaner():
        while running:
            time.sleep(3600 * 6)  # 每6小时检查一次
            clean_old_logs()

    cleaner_thread = threading.Thread(target=daily_cleaner, daemon=True)
    cleaner_thread.start()

    # 启动 MQTT
    mqtt = start_mqtt()

    # 启动 TCP 服务端
    tcp_server()


if __name__ == "__main__":
    main()
