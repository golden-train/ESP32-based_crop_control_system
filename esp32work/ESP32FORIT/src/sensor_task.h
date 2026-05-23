#ifndef _SENSOR_TASK_H
#define _SENSOR_TASK_H

#include "config.h"

/* 构建传感器数据行（格式 "temp humi soil lux"），返回长度 */
int buildDataLine(char *buf, int bufSize,
                  float *outTemp, float *outHumi,
                  int *outSoil, float *outLight);

/* 读取并发布传感器数据到 MQTT */
void publishSensors(void);

/* 超限检测并发布告警 */
void checkAndAlert(void);

#endif
