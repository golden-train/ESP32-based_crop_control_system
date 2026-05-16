#include "LED.h"
#include "DHT11.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// —— WiFi 配置 —— //
const char* WIFI_SSID     = "MJ7";
const char* WIFI_PASSWORD = "zjy20061001";

// —— MQTT 服务器配置 —— //
const char* MQTT_HOST     = "8.137.48.225";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT_ID = "ESP32_Sensor";
const char* MQTT_USER      = "esp32";
const char* MQTT_PASSWORD  = "20061001";

#define SOIL_PIN   34
#define LIGHT_PIN  35 

#define EXT_LED_PIN   23
#define PUMP_PIN      22
#define BUZZER_PIN    26

//—— 自我判断阈值 ——//
#define TEMP_HIGH      40.0f
#define TEMP_LOW        0.0f
#define HUMI_HIGH      1000.0f
#define HUMI_LOW       0.0f
#define SOIL_DRY        0
#define SOIL_WET        100
#define LIGHT_DARK      10
#define LIGHT_BRIGHT  1500

//—— 上报模式 ——//
enum ReportMode { NORMAL, REALTIME };
static ReportMode s_reportMode = NORMAL;
static unsigned long s_publishInterval = 300000;  // 默认5分钟（毫秒）
static const unsigned long NORMAL_INTERVAL   = 300000;  // 5分钟
static const unsigned long REALTIME_INTERVAL = 2000;    // 2秒

#define ALERT_INTERVAL 3000

static unsigned long s_lastAlertCheck = 0;
static unsigned long s_lastPublish = 0;

WiFiClient    wifiClient;
PubSubClient  mqttClient(wifiClient);

//=====================================================================//
//                        串口命令协议                                  //
//                                                                     //
//  '1'  开补光          → 开 EXT_LED(23) + 红灯慢闪 3 次              //
//  '2'  加湿            → 开 水泵(22)    + 蓝灯闪 3 次                //
//  '3'  蜂鸣器(温度异常) → 开 蜂鸣器(26)  + 红灯快闪 5 次            //
//  '4'  开灌溉          → 开 水泵(22)    + 红灯急促闪 3 次            //
//  '5'  关补光          → 关 EXT_LED(23) + 红灯慢闪 3 次              //
//  '6'  关灌溉          → 关 水泵(22)    + 绿灯慢闪 3 次              //
//  '8'  清除温度报警    → 仅关 蜂鸣器(26)                             //
//  '9'  关闭所有执行器   → 关灯+关水泵+关蜂鸣器                        //
//=====================================================================//

//—— 执行命令 ——//
static void executeCommand(char cmd)
{
  switch (cmd)
  {
    case '1':
      digitalWrite(EXT_LED_PIN, HIGH);
      blink_led(LED_R, 300, 300, 3);
      break;
    case '2':
      digitalWrite(PUMP_PIN, LOW);
      blink_led(LED_B, 300, 300, 3);
      break;
    case '3':
      digitalWrite(BUZZER_PIN, HIGH);
      blink_led(LED_R, 100, 100, 5);
      break;
    case '4':
      digitalWrite(PUMP_PIN, LOW);
      blink_led(LED_R, 100, 400, 3);
      break;
    case '5':
      digitalWrite(EXT_LED_PIN, LOW);
      blink_led(LED_R, 300, 300, 3);
      break;
    case '6':
      digitalWrite(PUMP_PIN, HIGH);
      blink_led(LED_G, 500, 500, 3);
      break;
    case '8':
      digitalWrite(BUZZER_PIN, LOW);
      blink_led(LED_G, 200, 200, 2);
      break;
    case '9':
      digitalWrite(EXT_LED_PIN, LOW);
      digitalWrite(PUMP_PIN, HIGH);
      digitalWrite(BUZZER_PIN, LOW);
      break;
  }
}

//—— ADC → lux 近似转换 ——//
static float adc_to_lux(int raw)
{
  if (raw <= 0) return 0.0f;
  float voltage = raw * 3.3f / 4095.0f;
  if (voltage < 0.01f) return 0.0f;
  float diff = 3.3f - voltage;
  if (diff < 0.01f) diff = 0.01f;
  return (voltage / diff) * 500.0f;
}

//—— 构建传感器数据行（返回 "温度 湿度 土壤 光照"） ——//
static String buildDataLine(float &outTemp, float &outHumi,
                            int &outSoil, float &outLight)
{
  float temp, humi;
  DHT11_read(&temp, &humi);

  int soil_raw = analogRead(SOIL_PIN);
  int light_raw = analogRead(LIGHT_PIN);

  int soil_pct = 100 - (soil_raw * 100 / 4095);
  float light_lux = adc_to_lux(light_raw);

  outTemp  = temp;
  outHumi  = humi;
  outSoil  = soil_pct;
  outLight = light_lux;

  return String(temp, 1) + " " + String(humi, 1) + " "
         + String(soil_pct) + " " + String(int(light_lux));
}

//—— 读取并发布传感器数据 ——//
static void publishSensors()
{
  float temp, humi;
  int soil;
  float light;
  String line = buildDataLine(temp, humi, soil, light);

  // 串口输出（调试用）
  Serial.println(line);

  // MQTT 发布
  if (mqttClient.connected())
    mqttClient.publish("esp32/data", line.c_str());
}

//—— 超限检测并发布告警 ——//
static void checkAndAlert()
{
  float temp, humi;
  int soil;
  float light;
  String line = buildDataLine(temp, humi, soil, light);

  bool alert = false;
  if (temp > TEMP_HIGH || temp < TEMP_LOW) alert = true;
  if (humi > HUMI_HIGH || humi < HUMI_LOW) alert = true;
  if (soil < SOIL_DRY || soil > SOIL_WET) alert = true;
  if (light < LIGHT_DARK || light > LIGHT_BRIGHT) alert = true;

  if (!alert) return;

  String alertLine = "!" + line;
  Serial.println(alertLine);
  if (mqttClient.connected())
    mqttClient.publish("esp32/alert", alertLine.c_str());

  digitalWrite(BUZZER_PIN, HIGH);
}

//—— MQTT 回调：收到 PC 下发的指令 ——//
static void mqttCallback(char* topic, byte* payload, unsigned int len)
{
  if (len < 1) return;
  char cmd = (char)payload[0];

  Serial.print("[MQTT] 收到指令: ");
  Serial.println(cmd);

  // 'R'/'N': 切换上报模式
  if (cmd == 'R')
  {
    s_reportMode = REALTIME;
    s_publishInterval = REALTIME_INTERVAL;
    Serial.println("[模式] 切换为实时模式（2秒上报）");
    return;
  }
  if (cmd == 'N')
  {
    s_reportMode = NORMAL;
    s_publishInterval = NORMAL_INTERVAL;
    Serial.println("[模式] 切换为普通模式（5分钟上报）");
    return;
  }

  executeCommand(cmd);
}

//—— 重连 MQTT ——//
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
      Serial.println("[模式] 普通模式（5分钟上报），'R'切换实时，'N'切回普通");
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

void setup()
{
  Serial.begin(115200);

  pinMode(EXT_LED_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(EXT_LED_PIN, LOW);
  digitalWrite(PUMP_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);

  Led_start();
  DHT11_start();

  // —— 连接 WiFi —— //
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

  // —— 配置 MQTT —— //
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  blink_led(LED_G, 200, 200, 3);  // 启动成功
}

void loop()
{
  // —— 保持 MQTT 连接 —— //
  if (!mqttClient.connected())
    reconnectMQTT();
  mqttClient.loop();

  // —— 串口命令接收（调试用） —— //
  while (Serial.available() > 0)
  {
    char cmd = Serial.read();
    if (cmd == 'R')
    {
      s_reportMode = REALTIME;
      s_publishInterval = REALTIME_INTERVAL;
      Serial.println("[模式] 实时模式（2秒上报）");
    }
    else if (cmd == 'N')
    {
      s_reportMode = NORMAL;
      s_publishInterval = NORMAL_INTERVAL;
      Serial.println("[模式] 普通模式（5分钟上报）");
    }
    else
    {
      executeCommand(cmd);
    }
  }

  // —— 定时发布传感器数据（动态间隔） —— //
  unsigned long now = millis();
  if (now - s_lastPublish >= s_publishInterval)
  {
    s_lastPublish = now;
    publishSensors();
  }

  // —— 定时自我判断 —— //
  if (now - s_lastAlertCheck >= ALERT_INTERVAL)
  {
    s_lastAlertCheck = now;
    checkAndAlert();
  }

  delay(10);
}
