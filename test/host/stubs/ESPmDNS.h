#pragma once
struct MDNSStub { void addService(const char*, const char*, int){} };
extern MDNSStub MDNS;
