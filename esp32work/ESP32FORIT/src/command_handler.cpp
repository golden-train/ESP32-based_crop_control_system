#include "command_handler.h"
#include "LED.h"
#include "config.h"
#include <Arduino.h>

void executeCommand(char cmd)
{
    switch (cmd)
    {
    case '1':
        digitalWrite(EXT_LED_PIN, LOW);
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
        digitalWrite(EXT_LED_PIN, HIGH);
        digitalWrite(PUMP_PIN, HIGH);
        digitalWrite(BUZZER_PIN, LOW);
        break;
    }
}
