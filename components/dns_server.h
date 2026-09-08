#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"

esp_err_t dns_server_start(void);
void dns_server_stop(void);

#endif