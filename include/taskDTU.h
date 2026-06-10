#pragma once
#include "systemState.h"
void taskDTU(void* pvParameters);
void dtuRequestReboot();
void dtuRequestInverterReboot();
void dtuSetPowerLimit(int percent);
void dtuSetInverterOn(bool on);
