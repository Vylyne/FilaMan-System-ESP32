#ifndef WLAN_H
#define WLAN_H

#include <Arduino.h>

void initWiFi();
void checkWiFiConnection();

extern bool ethOn;
bool networkConnected(); // returns ethOn || wifiOn

#endif