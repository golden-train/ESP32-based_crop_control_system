/*
 * DHT11 单独测试程序
 * 接线: DHT11 DATA → GPIO32, VCC → 3.3V, GND → GND
 * 波特率: 115200
 */

#include <DHT.h>

#define DHT_PIN 32
#define DHT_TYPE DHT11

DHT dht(DHT_PIN, DHT_TYPE);

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n============================");
  Serial.println("DHT11 测试程序");
  Serial.println("引脚: GPIO32");
  Serial.println("============================");

  dht.begin();
  Serial.println("DHT11 初始化完成");
}

void loop()
{
  // 读取温湿度
  float temp = dht.readTemperature();
  float humi = dht.readHumidity();

  // 检查是否读取成功
  if (isnan(temp) || isnan(humi))
  {
    Serial.println("[失败] DHT11 无响应，请检查接线");
  }
  else
  {
    Serial.print("[OK] 温度: ");
    Serial.print(temp, 1);
    Serial.print(" ℃  湿度: ");
    Serial.print(humi, 1);
    Serial.println(" %");
  }

  delay(2000);
}
