#ifndef _BH1750_H
#define _BH1750_H

#include <stdint.h>

#define BH1750_ADDR     0x23    /* ADDR 引脚接 GND */
#define BH1750_POWER_ON 0x01
#define BH1750_RESET    0x07
#define BH1750_CONT_HR  0x10    /* 连续高分辨率模式，1 lux 精度 */

void BH1750_start(void);
bool BH1750_read(float *lux);

#endif
