#ifndef _CONFIG_H
#define _CONFIG_H

/* ── WiFi 配置 ──────────────────────────────────── */
#define WIFI_SSID       "MJ7"
#define WIFI_PASSWORD   "zjy20061001"

/* ── MQTT 配置 ──────────────────────────────────── */
#define MQTT_HOST       "8.137.48.225"
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "ESP32_Sensor"
#define MQTT_USER       "esp32"
#define MQTT_PASSWORD   "20061001"

/* ── 传感器引脚 ─────────────────────────────────── */
#define SOIL_PIN    34     /* 土壤湿度 ADC */

/* ── 执行器引脚 ─────────────────────────────────── */
#define     EXT_LED_PIN   23   /* 补光灯 */
#define PUMP_PIN      22   /* 水泵继电器（低电平开启） */
#define BUZZER_PIN     25   /* 蜂鸣器 */

/* ── 自我判断阈值 ──────────────────────────────── */
#define TEMP_HIGH      40.0f
#define TEMP_LOW        0.0f
#define HUMI_HIGH      1000.0f
#define HUMI_LOW        0.0f
#define SOIL_DRY        0
#define SOIL_WET        100
#define LIGHT_DARK      10
#define LIGHT_BRIGHT  1500

/* ── 时间间隔 (ms) ──────────────────────────────── */
#define PUBLISH_INTERVAL    300000   /* 定时上报间隔：5 分钟 */
#define ALERT_INTERVAL       3000   /* 自我判断间隔 */

#endif
