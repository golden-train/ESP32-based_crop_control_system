#include "sensor_task.h"
#include "mqtt_handler.h"
#include "DHT20.h"
#include "config.h"
#include <Arduino.h>

/* ADC → lux 近似转换 */
float adc_to_lux(int raw)
{
    if (raw <= 0) return 0.0f;
    float voltage = raw * 3.3f / 4095.0f;
    if (voltage < 0.01f) return 0.0f;
    float diff = 3.3f - voltage;
    if (diff < 0.01f) diff = 0.01f;
    return (voltage / diff) * 500.0f;
}

/*
 * 构建传感器数据行。
 * 格式："温度 湿度 土壤 光照\n"
 * 示例："25.3 55.1 42 813"
 * 返回字符串长度（不含结尾 \0）。
 */
int buildDataLine(char *buf, int bufSize,
                  float *outTemp, float *outHumi,
                  int *outSoil, float *outLight)
{
    float temp, humi;
    DHT20_read(&temp, &humi);

    int soil_raw = analogRead(SOIL_PIN);
    int light_raw = analogRead(LIGHT_PIN);

    int soil_pct = 100 - (soil_raw * 100 / 4095);
    float light_lux = adc_to_lux(light_raw);

    *outTemp  = temp;
    *outHumi  = humi;
    *outSoil  = soil_pct;
    *outLight = light_lux;

    return snprintf(buf, bufSize, "%.1f %.1f %d %d",
                    temp, humi, soil_pct, (int)light_lux);
}

/*
 * 读取传感器并通过 MQTT 发布（需外部链接 mqttClient）。
 * 串口同时输出一行用于调试。
 */
void publishSensors(void)
{
    char line[64];
    float temp, humi, light;
    int soil;
    buildDataLine(line, sizeof(line), &temp, &humi, &soil, &light);

    Serial.println(line);

    if (mqttClient.connected())
        mqttClient.publish("esp32/data", line);
}

/* 超限检测：任一传感器超限则发布告警 + 蜂鸣器 */
void checkAndAlert(void)
{
    char line[64];
    float temp, humi, light;
    int soil;
    buildDataLine(line, sizeof(line), &temp, &humi, &soil, &light);

    bool alert = false;
    if (temp > TEMP_HIGH || temp < TEMP_LOW)   alert = true;
    if (humi > HUMI_HIGH || humi < HUMI_LOW)   alert = true;
    if (soil < SOIL_DRY || soil > SOIL_WET)    alert = true;
    if (light < LIGHT_DARK || light > LIGHT_BRIGHT) alert = true;

    if (!alert) return;

    char alertLine[80];
    snprintf(alertLine, sizeof(alertLine), "!%s", line);
    Serial.println(alertLine);
    if (mqttClient.connected())
        mqttClient.publish("esp32/alert", alertLine);

    digitalWrite(BUZZER_PIN, HIGH);
}
