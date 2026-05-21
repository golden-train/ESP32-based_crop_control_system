#ifndef _MQTT_HANDLER_H
#define _MQTT_HANDLER_H

#include <PubSubClient.h>
#include <WiFi.h>

/* MQTT 客户端对象（全局，供 sensor_task.cpp 中 extern 引用） */
extern WiFiClient    wifiClient;
extern PubSubClient  mqttClient;

/* 连接 WiFi（阻塞直到成功） */
void wifi_connect(void);

/* 配置 MQTT 回调并连接 Broker */
void mqtt_begin(void);

/* 保持 MQTT 连接（在 loop 中调用） */
void mqtt_loop(void);

#endif
