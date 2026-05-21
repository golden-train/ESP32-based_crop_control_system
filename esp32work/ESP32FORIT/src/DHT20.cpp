#include "DHT20.h"
#include <Adafruit_AHTX0.h>
#include <Wire.h>

static Adafruit_AHTX0 aht; 
static float s_lastTemp = 20.0f;   
static float s_lastHumi = 50.0f; 
static bool  s_ok       = false;   

void DHT20_start(void)
{
    Wire.begin(DHT20_SDA, DHT20_SCL);

    if (!aht.begin())
    {
        Serial.println("[DHT20] 初始化失败！请检查接线");
        s_ok = false;
    }
    else
    {
        s_ok = true;
        Serial.println("[DHT20] 已初始化");
    }
}
bool DHT20_read(float *temperature, float *humidity)
{
    if (!s_ok)
    {
        *temperature = s_lastTemp;
        *humidity    = s_lastHumi;
        return false;
    }

    sensors_event_t humi_event, temp_event;
    if (!aht.getEvent(&humi_event, &temp_event))
    {
        /* 读取失败，返回上次有效值 */
        *temperature = s_lastTemp;
        *humidity    = s_lastHumi;
        return false;
    }

    float t = temp_event.temperature;
    float h = humi_event.relative_humidity;

    if (t > -10.0f && t < 60.0f && h >= 0.0f && h <= 100.0f)
    {
        s_lastTemp = t;
        s_lastHumi = h;
        *temperature = t;
        *humidity    = h;
        return true;
    }//校验

    *temperature = s_lastTemp;
    *humidity    = s_lastHumi;
    return false;
}
