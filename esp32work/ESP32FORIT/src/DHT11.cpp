#include "DHT11.h"
#include <DHTesp.h>

static DHTesp dht;

void DHT11_start(void)
{
  dht.setup(DHT_PIN, DHTesp::DHT11);
}

bool DHT11_read(float *temperature, float *humidity)
{
  TempAndHumidity values = dht.getTempAndHumidity();

  if (dht.getStatus() != 0)
    return false;

  *temperature = values.temperature;
  *humidity = values.humidity;
  return true;
}
