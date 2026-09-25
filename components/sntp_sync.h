#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include <stdbool.h>

void sntp_init_and_sync(void);
bool sntp_is_synced(void);

#endif