#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connects to WiFi (station mode) using credentials from NVS
 * (CONFIG_LAUNCHER_NVS_NAMESPACE, keys "wifi_ssid"/"wifi_pass"), falling
 * back to CONFIG_LAUNCHER_NET_WIFI_SSID/PASSWORD if NVS has none yet.
 *
 * Blocks for at most ~10s, then gives up. Never blocks boot indefinitely --
 * a WiFi failure just means the 3 network features stay inactive for this
 * boot cycle (see spec "WiFi -- config partagee"). Safe to call multiple
 * times (e.g. lazily, only when a network feature is actually used); a
 * second call while already connected returns true immediately.
 */
bool net_wifi_connect(void);

bool net_wifi_is_connected(void);

/* Fills buf with the last assigned IPv4 address as a string (e.g.
 * "192.168.1.42") and returns true, or returns false without touching buf
 * if not currently connected. Useful for surfacing the address a remote
 * control feature needs to be reached at without a serial cable attached
 * -- see main.c. */
bool net_wifi_get_ip_string(char *buf, size_t buf_len);

/* Persists SSID/password to NVS for future boots (e.g. after a successful
 * connect using the Kconfig dev fallback, or from a future provisioning UI). */
void net_wifi_save_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
