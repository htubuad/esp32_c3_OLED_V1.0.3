#include "dns_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <arpa/inet.h>

static const char *TAG = "DNS";

#define DNS_PORT 53
#define DNS_MAX_PACKET 512
#define DNS_STACK_SIZE  2048
#define DNS_PRIO        5
#define DNS_CORE        0

static int s_sock = -1;
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;

static uint8_t s_pkt[DNS_MAX_PACKET];

static void dns_task(void *arg)
{
    struct sockaddr_in from = {0};
    socklen_t from_len = sizeof(from);

    ESP_LOGI(TAG, "DNS server task started on port %d (core %d)", DNS_PORT, DNS_CORE);

    while (s_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s_sock, &readfds);

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        int ret = select(s_sock + 1, &readfds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        from_len = sizeof(from);
        ssize_t len = recvfrom(s_sock, s_pkt, sizeof(s_pkt), 0,
                               (struct sockaddr *)&from, &from_len);
        if (len < 12) continue;

        int q_offset = 12;
        while (q_offset < len) {
            if (s_pkt[q_offset] == 0) { q_offset++; break; }
            q_offset += s_pkt[q_offset] + 1;
        }
        if (q_offset + 4 > len) continue;

        uint16_t qtype = (s_pkt[q_offset] << 8) | s_pkt[q_offset + 1];
        uint16_t qclass = (s_pkt[q_offset + 2] << 8) | s_pkt[q_offset + 3];

        int question_end = q_offset + 4;

        s_pkt[2] = 0x80; s_pkt[3] = 0x80;
        s_pkt[6] = 0x00; s_pkt[7] = 0x01;
        s_pkt[8] = 0x00; s_pkt[9] = 0x00;
        s_pkt[10] = 0x00; s_pkt[11] = 0x00;

        int out_offset = question_end;

        if (qtype == 1 && qclass == 1) {
            s_pkt[out_offset++] = 0xC0; s_pkt[out_offset++] = 0x0C;
            s_pkt[out_offset++] = 0x00; s_pkt[out_offset++] = 0x01;
            s_pkt[out_offset++] = 0x00; s_pkt[out_offset++] = 0x01;
            s_pkt[out_offset++] = 0x00; s_pkt[out_offset++] = 0x00;
            s_pkt[out_offset++] = 0x00; s_pkt[out_offset++] = 0x3C;
            s_pkt[out_offset++] = 0x00; s_pkt[out_offset++] = 0x04;
            s_pkt[out_offset++] = 192; s_pkt[out_offset++] = 168;
            s_pkt[out_offset++] = 4; s_pkt[out_offset++] = 1;
        }

        sendto(s_sock, s_pkt, out_offset, 0, (struct sockaddr *)&from, from_len);
    }

    ESP_LOGI(TAG, "DNS task exiting");
    vTaskDelete(NULL);
    s_task = NULL;
}

esp_err_t dns_server_start(void)
{
    if (s_running) return ESP_OK;

    ESP_LOGI(TAG, "Starting DNS server...");

    int retry;
    for (retry = 0; retry < 5; retry++) {
        vTaskDelay(pdMS_TO_TICKS(200));

        s_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (s_sock < 0) {
            ESP_LOGW(TAG, "socket() failed (attempt %d/5), errno=%d", retry + 1, errno);
            continue;
        }

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(DNS_PORT);

        if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGW(TAG, "bind() failed port %d (attempt %d/5), errno=%d",
                     DNS_PORT, retry + 1, errno);
            close(s_sock);
            s_sock = -1;
            continue;
        }

        ESP_LOGI(TAG, "UDP socket bound on port %d", DNS_PORT);
        break;
    }

    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create/bind UDP socket after 5 attempts");
        return ESP_FAIL;
    }

    s_running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(dns_task, "dns_srv", DNS_STACK_SIZE,
                                             NULL, DNS_PRIO, &s_task, DNS_CORE);
    if (!ok) {
        ESP_LOGE(TAG, "Failed to create DNS task");
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DNS server started (all queries -> 192.168.4.1)");
    return ESP_OK;
}

void dns_server_stop(void)
{
    if (!s_running) return;
    ESP_LOGI(TAG, "Stopping DNS server...");
    s_running = false;
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
    }
    if (s_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "DNS server stopped");
}