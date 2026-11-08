#ifndef WIFI_PROBE_H
#define WIFI_PROBE_H

#include <stdint.h>

void wifi_probe_start(void);
int wifi_probe_recover(void);
int wifi_probe_is_ready(void);
const uint8_t *wifi_probe_server_ip(void);
uint16_t wifi_probe_server_port(void);

#endif
