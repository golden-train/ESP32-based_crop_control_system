#include "LED.h"
#include "DHT11.h"
#include <Arduino.h>

#define SOIL_PIN   34
#define LIGHT_PIN  35

#define EXT_LED_PIN   23   // 额外指示灯（光照异常指示）
#define PUMP_PIN      22   // 水泵（低电平触发）
#define BUZZER_PIN    26   // 蜂鸣器

//—— 自我判断阈值（数据超限时自动上报 PC） ——//
#define TEMP_HIGH      40.0f   // 温度上限 ℃
#define TEMP_LOW        0.0f   // 温度下限 ℃
#define HUMI_HIGH      80.0f   // 湿度上限 %
#define HUMI_LOW       20.0f   // 湿度下限 %
#define SOIL_DRY        30     // 土壤干旱阈值 %
#define SOIL_WET        80     // 土壤过湿阈值 %
#define LIGHT_DARK       5     // 光照过低阈值 %
#define LIGHT_BRIGHT    95     // 光照过强阈值 %

#define ALERT_INTERVAL 3000   // 自动检查间隔（毫秒）

static unsigned long s_lastAlertCheck = 0;

//=====================================================================//
//                        串口命令协议                                  //
//                                                                     //
//  '0'  读取一次传感器数据并输出                                        //
//  '1'  光照异常      → 开 EXT_LED(23) + 红灯慢闪 3 次              //
//  '2'  湿度异常      → 开 水泵(22)    + 蓝灯闪 3 次                //
//  '3'  温度异常      → 开 蜂鸣器(26)  + 红灯快闪 5 次              //
//  '4'  土壤湿度异常  → 开 水泵(22)    + 红灯急促闪 3 次            //
//  '9'  关闭所有执行器
//=====================================================================//

//—— 读取所有传感器并输出一行数据 ——//
static void read_sensors(void)
{
  float temp, humi;

  if (!DHT11_read(&temp, &humi))
    return;

  int soil_raw = analogRead(SOIL_PIN);
  int light_raw = analogRead(LIGHT_PIN);

  int soil_pct = 100 - (soil_raw * 100 / 4095);
  int light_pct = 100 - (light_raw * 100 / 4095);

  Serial.print(temp, 1);
  Serial.print(" ");
  Serial.print(humi, 1);
  Serial.print(" ");
  Serial.print(soil_pct);
  Serial.print(" ");
  Serial.println(light_pct);
}

//—— 读取传感器并检查阈值，超限则带 "!" 前缀自动上报 ——//
static void check_and_alert(void)
{
  float temp, humi;
  if (!DHT11_read(&temp, &humi))
    return;

  int soil_raw = analogRead(SOIL_PIN);
  int light_raw = analogRead(LIGHT_PIN);

  int soil_pct = 100 - (soil_raw * 100 / 4095);
  int light_pct = 100 - (light_raw * 100 / 4095);

  bool alert = false;
  if (temp > TEMP_HIGH || temp < TEMP_LOW) alert = true;
  if (humi > HUMI_HIGH || humi < HUMI_LOW) alert = true;
  if (soil_pct < SOIL_DRY || soil_pct > SOIL_WET) alert = true;
  if (light_pct < LIGHT_DARK || light_pct > LIGHT_BRIGHT) alert = true;

  if (!alert) return;

  // 超限数据带 "!" 前缀，PC 侧可据此区分
  Serial.print("!");
  Serial.print(temp, 1);
  Serial.print(" ");
  Serial.print(humi, 1);
  Serial.print(" ");
  Serial.print(soil_pct);
  Serial.print(" ");
  Serial.println(light_pct);
  digitalWrite(BUZZER_PIN, HIGH); // 光照异常指示灯常亮
}

void setup()
{
  Serial.begin(115200);

  pinMode(EXT_LED_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(EXT_LED_PIN, LOW);//使用继电器时注意更改
  digitalWrite(PUMP_PIN, HIGH);    // 水泵默认关闭（高电平）
  digitalWrite(BUZZER_PIN, LOW);

  Led_start();
  DHT11_start();
  blink_led(LED_G, 200, 200, 3);  // 启动成功提示
}

void loop()
{
  //—— 串口命令接收 ——//
  while (Serial.available() > 0)
  {
    char cmd = Serial.read();
    switch (cmd)
    {
      case '0':
        // 读取一次传感器数据
        read_sensors();
        break;

      case '1':
        digitalWrite(EXT_LED_PIN, HIGH);
        blink_led(LED_R, 300, 300, 3);//开灯
        break;

      case '2':
        digitalWrite(PUMP_PIN, LOW);
        blink_led(LED_B, 300, 300, 3);//开水泵
        break;

      case '3':
        digitalWrite(BUZZER_PIN, HIGH);
        blink_led(LED_R, 100, 100, 5);
        break;

      case '4':
        digitalWrite(PUMP_PIN, LOW);
        blink_led(LED_R, 100, 400, 3);//开水泵
        break;
      case '5':
        digitalWrite(EXT_LED_PIN, LOW);
        blink_led(LED_R, 300, 300, 3);//关灯
        break;

      case '6':
        digitalWrite(PUMP_PIN, HIGH);
        blink_led(LED_G, 500, 500, 3);

      case '9':
        // 关闭所有执行器
        digitalWrite(EXT_LED_PIN, LOW);
        digitalWrite(PUMP_PIN, HIGH);
        digitalWrite(BUZZER_PIN, LOW);
        break;
    }
  }
  
  //—— 定时自我判断：超限则自动上报 PC ——//
  unsigned long now = millis();
  if (now - s_lastAlertCheck >= ALERT_INTERVAL)
  {
    s_lastAlertCheck = now;
    check_and_alert();
  }

  delay(100);
}
