#include "LED.h"
#include <Arduino.h>

//=== 引脚初始化 ===//
void Led_start(void)
{
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
  Serial.println("esp32 has been started");
}

//=== 非阻塞闪烁（后台 FreeRTOS 任务自动处理，无需用户在 loop 中调用）===//
#define LED_NUM 3

static const int led_pins[LED_NUM] = {LED_R, LED_G, LED_B};

typedef struct {
  unsigned long on_ms;
  unsigned long off_ms;
  int count;         // -1 = 无限闪烁
  unsigned long last;
  bool state;
  bool active;
} BlinkLED;

static BlinkLED leds[LED_NUM] = {0};

static int led_idx(int pin)
{
  for (int i = 0; i < LED_NUM; i++)
    if (led_pins[i] == pin) return i;
  return -1;
}

void blink_led(int led, unsigned long on_ms, unsigned long off_ms, int count)
{
  int i = led_idx(led);
  if (i < 0) return;

  leds[i].on_ms = on_ms;
  leds[i].off_ms = off_ms;
  leds[i].count = count;
  leds[i].last = millis();
  leds[i].state = true;
  leds[i].active = true;
  digitalWrite(led, HIGH);
}

static void blink_task(void *)
{
  while (1)
  {
    unsigned long now = millis();
    for (int i = 0; i < LED_NUM; i++)
    {
      if (!leds[i].active) continue;

      unsigned long elapsed = now - leds[i].last;
      unsigned long wait = leds[i].state ? leds[i].on_ms : leds[i].off_ms;

      if (elapsed >= wait)
      {
        leds[i].last = now;
        leds[i].state = !leds[i].state;
        digitalWrite(led_pins[i], leds[i].state ? HIGH : LOW);

        // 一次完整的亮→灭算一次闪烁
        if (!leds[i].state && leds[i].count > 0)
        {
          leds[i].count--;
          if (leds[i].count == 0)
          {
            leds[i].active = false;
            leds[i].state = false;
          }
        }
      }
    }
    delay(10);
  }
}

// 上电时自动创建后台任务
static void auto_setup() __attribute__((constructor));
static void auto_setup()
{
  xTaskCreatePinnedToCore(blink_task, "blink", 2048, NULL, 1, NULL, 0);
}
