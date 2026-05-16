/*
 * DHT11 单独测试程序
 * 使用方法：PlatformIO 中编译上传
 * 接线: DHT11 DATA → GPIO32, VCC → 3.3V, GND → GND
 * 串口: 115200 baud
 */

#include <Arduino.h>
#include <DHTesp.h>

#define DHT_PIN 32

DHTesp dht;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n============================");
  Serial.println("DHT11 测试程序");
  Serial.println("引脚: GPIO32");
  Serial.println("============================");

  dht.setup(DHT_PIN, DHTesp::DHT11);
  Serial.println("DHT11 初始化完成");
}

void loop()
{
  TempAndHumidity values = dht.getTempAndHumidity();

  if (dht.getStatus() != 0)
  {
    Serial.print("[失败] 错误码: ");
    Serial.print(dht.getStatus());
    Serial.println(" 请检查接线");
  }
  else
  {
    Serial.print("[OK] 温度: ");
    Serial.print(values.temperature, 1);
    Serial.print(" C  湿度: ");
    Serial.print(values.humidity, 1);
    Serial.println(" %");
  }

  delay(2000);
}
