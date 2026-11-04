#pragma once
#include "WiFi.h"
extern int g_dnsStarted;
struct DNSServer { bool start(int, const char*, IPStub){ g_dnsStarted++; return true; } void processNextRequest(){} };
