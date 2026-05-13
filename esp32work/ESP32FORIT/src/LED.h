#ifndef _LED_H
#define _LED_H

#define LED_R 21
#define LED_G 19
#define LED_B 18

void Led_start(void);
void blink_led(int led, unsigned long on_ms, unsigned long off_ms, int count);

#endif
