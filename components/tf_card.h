#ifndef TF_CARD_H
#define TF_CARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TF_CS_GPIO      10
#define TF_CLK_GPIO     6
#define TF_CMD_GPIO     7
#define TF_D0_GPIO      5

#define TF_MOUNT_POINT  "/tf"

bool tf_card_init(void);
void tf_card_deinit(void);
bool tf_card_is_mounted(void);
bool tf_card_reinit(void);
int  tf_card_get_state(void);

#define TF_CHECK_INTERVAL_MS   30000
void tf_card_periodic_check(void);

bool tf_card_read_file(const char *path, char *buf, size_t buf_size, size_t *bytes_read);
bool tf_card_write_file(const char *path, const char *data, size_t data_len);
bool tf_card_append_file(const char *path, const char *data, size_t data_len);
bool tf_card_flush(void);
bool tf_card_flush_all(void);
bool tf_card_emergency_flush(void);
bool tf_card_list_dir(const char *dir_path);

bool tf_card_get_space(uint64_t *total_bytes, uint64_t *free_bytes);

bool tf_card_append_csv(float ntc1, float ntc2);
int tf_card_read_csv_by_date(const char *date_str, char *buf, size_t buf_size);

#endif