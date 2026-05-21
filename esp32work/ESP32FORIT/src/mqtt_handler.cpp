#include "mqtt_handler.h"
#include "command_handler.h"
#include "sensor_task.h"
#include "config.h"
#include <Arduino.h>

/* ── 全局对象定义 ───────────────────────────────── */
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

/* ── MQTT 回调：收到 PC/服务端下发的指令 ─────────── */
static void mqttCallback(char* topic, byte* payload, unsigned int len)
{
    if (len < 1) return;
    char cmd = (char)payload[0];

    Serial.print("[MQTT] 收到指令: ");
    Serial.println(cmd);

    /* 'T': 立即触发一次传感器上报 */
    if (cmd == 'T')
    {
        publishSensors();
        Serial.println("[触发] 立即上报");
        return;
    }

    executeCommand(cmd);
}

/* ── 重连 MQTT（阻塞） ──────────────────────────── */
static void reconnectMQTT()
{
    while (!mqttClient.connected())
    {
        Serial.print("[MQTT] 连接中...");
        if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD))
        {
            Serial.println("已连接");
            mqttClient.subscribe("esp32/cmd");
            mqttClient.publish("esp32/status", "online");
            Serial.println("[就绪] 每5分钟上报，发'T'手动触发一次");
        }
        else
        {
            Serial.print("失败, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" 5秒后重试");
            delay(5000);
        }
    }
}

/* ── 连接 WiFi（阻塞直到成功） ──────────────────── */
void wifi_connect(void)
{
    Serial.print("[WiFi] 连接 ");
    Serial.print(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" 已连接");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
}

/* ── 配置 MQTT 回调并连接 ───────────────────────── */
void mqtt_begin(void)
{
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    reconnectMQTT();
}

/* ── 保持 MQTT 连接（loop 中周期调用） ──────────── */
void mqtt_loop(void)
{
    if (!mqttClient.connected())
        reconnectMQTT();
    mqttClient.loop();
}
