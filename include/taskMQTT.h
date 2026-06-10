#pragma once
#include "systemState.h"
void taskMQTT(void* pvParameters);
void mqttPublishGpioState();
void mqttSendHaDiscovery();
