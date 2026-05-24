#include "BH1750.h"
#include <Wire.h>
#include <Arduino.h>

static bool    s_ok       = false;
static bool    s_first    = true;      /* 首次读取时需要等第一个转换周期 */
static float   s_lastLux  = 0.0f;
static uint8_t s_addr     = 0;         /* 探测到的 I2C 地址 */

/* 尝试探测 BH1750 的 I2C 地址（0x23 或 0x5C） */
static uint8_t probeAddress(void)
{
    uint8_t candidates[] = {0x23, 0x5C};
    for (int i = 0; i < 2; i++)
    {
        Wire.beginTransmission(candidates[i]);
        if (Wire.endTransmission() == 0)
        {
            Serial.print("[BH1750] 探测到地址: 0x");
            Serial.println(candidates[i], HEX);
            return candidates[i];
        }
    }
    return 0;
}

void BH1750_start(void)
{
    s_addr = probeAddress();
    if (s_addr == 0)
    {
        Serial.println("[BH1750] 未探测到设备！请检查接线");
        s_ok = false;
        return;
    }

    Wire.beginTransmission(s_addr);
    Wire.write(BH1750_POWER_ON);
    Wire.endTransmission();
    delay(15);

    Wire.beginTransmission(s_addr);
    Wire.write(BH1750_CONT_HR2);   /* H2 模式：0.5 lux，量程 120000 */
    if (Wire.endTransmission() != 0)
    {
        Serial.println("[BH1750] 设置模式失败");
        s_ok = false;
        return;
    }

    s_ok    = true;
    s_first = true;
    Serial.println("[BH1750] 已初始化（H2 模式，0.5 lux，量程 120000）");
}

bool BH1750_read(float *lux)
{
    if (!s_ok)
    {
        *lux = s_lastLux;
        return false;
    }

    /* 首次读取：等第一个转换周期完成（高分辨率模式最大 180ms） */
    if (s_first)
    {
        delay(180);
        s_first = false;
    }

    Wire.requestFrom(s_addr, (uint8_t)2);
    if (Wire.available() < 2)
    {
        *lux = s_lastLux;
        return false;
    }

    uint16_t raw = (Wire.read() << 8) | Wire.read();
    float v = raw / 1.2f;

    /* 合理性校验：H2 模式量程 0 ~ 120000 lux */
    if (v >= 0.0f && v <= 120000.0f)
    {
        s_lastLux = v;
        *lux = v;
        return true;
    }

    *lux = s_lastLux;
    return false;
}
