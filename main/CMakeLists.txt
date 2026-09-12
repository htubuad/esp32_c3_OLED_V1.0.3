idf_component_register(SRCS "main.c"
                    "../components/switch.c"
                    "../components/led.c"
                    "../components/temp_sensor.c"
                    "../components/wifi_manager.c"
                    "../components/dns_server.c"
                    "../components/sntp_sync.c"
                    "../components/mqtt_aliyun.c"
                    "../components/web_server.c"
                    "../components/delay.c"
                    INCLUDE_DIRS "." "../components"
                    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_driver_gpio esp_driver_ledc esp_http_server esp_timer mqtt mbedtls esp_driver_tsens heap lwip cjson)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LWIP_MDNS_RESPONDER=1 MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)

target_sources(${COMPONENT_LIB} PRIVATE
  $ENV{IDF_PATH}/components/lwip/lwip/src/apps/mdns/mdns.c
  $ENV{IDF_PATH}/components/lwip/lwip/src/apps/mdns/mdns_out.c
  $ENV{IDF_PATH}/components/lwip/lwip/src/apps/mdns/mdns_domain.c
)