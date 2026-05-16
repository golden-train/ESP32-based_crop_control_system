#ifndef _DHT11_H
#define _DHT11_H

#define DHT_PIN 32

void DHT11_start(void);
bool DHT11_read(float *temperature, float *humidity);

#endif
