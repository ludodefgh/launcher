#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal NimBLE GATT peripheral exposing the slot list (read) and a way
 * to trigger boot_into() remotely (write) -- see README.md for the exact
 * wire format. No pairing/bonding: the optional PIN (CONFIG_LAUNCHER_NET_REMOTE_PIN)
 * is checked at the application layer inside the write payload, mirroring
 * net_remote_http.c's approach, to keep both transports symmetric and
 * avoid NimBLE's much larger security-manager surface for a "trusted home
 * network" threat model (see spec). No-op if already started.
 */
void net_remote_ble_start(void);

#ifdef __cplusplus
}
#endif
