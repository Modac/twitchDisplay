#pragma once
extern bool g_bundleUsed; extern int g_tlsStops;
struct WiFiClientSecure { void useBuiltinCACertBundle(){ g_bundleUsed = true; } void stop(){ g_tlsStops++; } };
