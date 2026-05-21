#ifndef _DHT20_H
#define _DHT20_H

#define DHT20_SDA 26   //I2C 数据线 
#define DHT20_SCL 27   // I2C 时钟线 


void DHT20_start(void);

bool DHT20_read(float *temperature, float *humidity);

#endif
