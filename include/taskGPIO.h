#pragma once
void taskGPIO(void* pvParameters);
void gpioSetRelay(bool state);
void gpioSetPin(int index, bool state);  // index 0–3
bool gpioGetPin(int index);
bool gpioGetRelay();
