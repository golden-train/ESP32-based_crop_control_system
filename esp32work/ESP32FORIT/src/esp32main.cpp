/*
 * ESP32 传感器节点 — 主入口
 *
 * 职责：初始化硬件 → 连接网络 → 主循环保持运行。
 * 所有业务逻辑均拆分到独立模块：
 *   config.h         — 引脚/阈值/网络配置
 *   command_handler  — 执行器指令处理
 *   sensor_task      — 传感器采集/发布/报警
 *   mqtt_handler     — WiFi/MQTT 连接与回调
 *   DHT20            — 温湿度传感器驱动
 *   BH1750           — 光照传感器驱动
 *   LED              — RGB 指示灯驱动
 */
#include "config.h"
#include "command_handler.h"
#include "sensor_task.h"
#include "mqtt_handler.h"
#include "LED.h"
#include "DHT20.h"
#include "BH1750.h"
#include <Arduino.h>

static unsigned long s_lastPublish   = 0;
static unsigned long s_lastAlertCheck = 0;

void setup()
{
    Serial.begin(115200);

    /* 初始化执行器引脚 */
    pinMode(EXT_LED_PIN, OUTPUT);
    pinMode(PUMP_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(EXT_LED_PIN, HIGH);
    digitalWrite(PUMP_PIN, HIGH);
    digitalWrite(BUZZER_PIN, LOW);

    Led_start();
    DHT20_start();
    BH1750_start();
    wifi_connect();
    mqtt_begin();

    blink_led(LED_G, 200, 200, 3);  /* 启动成功 */
}

void loop()
{
    /* 保持 MQTT 连接并处理收到的消息 */
    mqtt_loop();

    /* 串口命令接收 */
    while (Serial.available() > 0)
    {
        char cmd = Serial.read();
        if (cmd == 'T')
        {
            publishSensors();
            Serial.println("[触发] 立即上报");
        }
        else
        {
            executeCommand(cmd);
        }
    }

    /* 定时发布传感器数据 */
    unsigned long now = millis();
    if (now - s_lastPublish >= PUBLISH_INTERVAL)
    {
        s_lastPublish = now;
        publishSensors();
    }

    /* 定时自我判断（超限告警） */
    if (now - s_lastAlertCheck >= ALERT_INTERVAL)
    {
        s_lastAlertCheck = now;
        checkAndAlert();
    }

    delay(10);
}
