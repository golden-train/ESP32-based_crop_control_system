#ifndef _COMMAND_HANDLER_H
#define _COMMAND_HANDLER_H

/*
 * 指令协议：
 *  '1'  开补光        → LED 亮 + 红灯慢闪 3 次
 *  '2'  加湿          → 水泵开 + 蓝灯闪 3 次
 *  '3'  蜂鸣器(温度异常) → 蜂鸣器响 + 红灯快闪 5 次
 *  '4'  开灌溉        → 水泵开 + 红灯急促闪 3 次
 *  '5'  关补光        → LED 灭 + 红灯慢闪 3 次
 *  '6'  关灌溉        → 水泵关 + 绿灯慢闪 3 次
 *  '8'  清除温度报警   → 关蜂鸣器 + 绿灯闪 2 次
 *  '9'  关闭所有执行器 → 关灯 + 关水泵 + 关蜂鸣器
 */

void executeCommand(char cmd);

#endif
