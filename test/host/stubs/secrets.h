// FAKE test values for the host simulation (not real credentials). The
// variant (TOKEN / SECRET / NOSECRETS) is chosen by run.sh with -DVARIANT_...
// This file is committed on purpose; real secrets belong in include/secrets.h
// (gitignored).
#ifndef VARIANT_NOSECRETS
#define WIFI_SSID "homewifi"
#define WIFI_PW "x"
#define WEB_PASSWORD "pw"
#ifdef VARIANT_SECRET
#define TWITCH_CLIENT_ID "myid"
#define TWITCH_CLIENT_SECRET "mysecret"
#else
#define TWITCH_TOKEN "statictoken"
#endif
#endif
