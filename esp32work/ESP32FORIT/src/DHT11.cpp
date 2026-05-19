#include "DHT11.h"
#include <DHTesp.h>

static DHTesp dht;
static float s_lastTemp = 20.0f;  // 默认值
static float s_lastHumi = 50.0f;

void DHT11_start(void)
{
  dht.setup(DHT_PIN, DHTesp::DHT11);
}

bool DHT11_read(float *temperature, float *humidity)
{
  // 最多重试 3 次
  for (int retry = 0; retry < 3; retry++)
  {
    TempAndHumidity values = dht.getTempAndHumidity();

    if (dht.getStatus() != 0)
    {
      delay(10);
      continue;
    }

    float t = values.temperature;
    float h = values.humidity;

    // 值域校验：拒绝明显错误的读数
    bool tempOk = (t > -10.0f && t < 60.0f);
    bool humiOk = (h > 0.0f && h <= 100.0f);

    if (tempOk && humiOk)
    {
      s_lastTemp = t;
      s_lastHumi = h;
      *temperature = t;
      *humidity = h;
      return true;
    }

    delay(10);
  }

  // 所有重试都失败，返回上次有效值
  *temperature = s_lastTemp;
  *humidity = s_lastHumi;
  return false;
}
