#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small esp_http_server exposing the slot list and a way to trigger
 * boot_into() remotely -- same function the local menu uses, network is
 * just another event source (see spec). Requires WiFi already connected;
 * caller (main.c) is responsible for that. No-op if already started.
 */
void net_remote_http_start(void);

#ifdef __cplusplus
}
#endif
