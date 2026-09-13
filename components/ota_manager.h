#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_SWITCHING,
    OTA_STATE_REBOOT_WAIT,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int         progress;
    char        version[32];
    char        error[128];
} ota_status_t;

void ota_init(void);
bool ota_is_in_progress(void);
void ota_handle_mqtt_msg(const char *topic, int topic_len,
                         const char *data, int data_len);
void ota_get_status(ota_status_t *out);
void ota_abort(void);

#endif