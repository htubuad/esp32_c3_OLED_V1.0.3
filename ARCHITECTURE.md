﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿﻿# ESP32-C3 固件架构说明

> 版本: V1.7.1 | 芯片: ESP32-C3 | IDF: v6.1.0

---

## 1. 目录结构

```
esp32_c3_V1.5.1/
├── main/                      # 主程序入口
│   ├── main.c                 # app_main() 启动流程
│   ├── version.h.in           # ★ 版本号模板（configure_file 从顶层 CMake VERSION 自动注入，唯一源头）
│   ├── version_bump.py        # ★ v1.7.1: 版本号升级工具（改顶层 CMakeLists.txt VERSION，唯一入口）
│   └── CMakeLists.txt         # ★ 末尾 configure_file()，让顶层 VERSION 成为唯一源头
├── components/                # 组件库（驱动统一放这里）
│   ├── wifi_manager.c/h       # ★ WiFi 状态机（STA+AP 共存）
│   ├── mqtt_aliyun.c/h        # ★ 阿里云 MQTT 状态机（上报同步写 TF 卡）
│   ├── ota_manager.c/h        # ★ OTA 升级管理器（MQTT 触发 + HTTP 下载 / TF 卡触发 + fread）
│   ├── tf_card.c/h            # TF 卡驱动（SPI2 + FATFS + 2KB 追加缓存）
│   ├── ntc_sensor.c/h        # 双路 NTC 热敏电阻（ADC1，共享 unit）
│   ├── power_monitor.c/h      # 电源监测（GPIO2 ADC 电压采样 + esp_timer 欠压保护状态机）
│   ├── web_server.c/h         # HTTP 服务器（配置/状态查看/TF 卡状态）
│   ├── dns_server.c/h         # DNS 服务器（AP 模式 CNAME 劫持）
│   ├── sntp_sync.c/h          # SNTP 时间同步
│   ├── pcf8563.c/h            # PCF8563 外部 RTC（I2C, SCL=GPIO9, SDA=GPIO8）
│   ├── led.c/h                # 状态 LED 驱动（含 OTA 指示模式）
│   ├── switch.c/h             # 开关 / 电源 GPIO 驱动
│   ├── temp_sensor.c/h        # 内部温度传感器驱动
│   └── history_query.c/h     # ★ 历史数据查询 v2（按天聚合 + 单日分页 + QUERY_CANCEL 取消）
├── partitions.csv             # 分区表（双 OTA 分区 + ota_data）
├── sdkconfig                  # SDK 配置
└── CMakeLists.txt
```

---

## 2. 启动流程 (main.c)

```
app_main()
  │
  ├─ 1. NVS 初始化（若损坏则擦除重建）
  ├─ 1.5 ★时区设置（V1.7.1 新增，任何时间操作之前必须先设！）
  │     └─ setenv("TZ", "CST-8", 1) + tzset()
  │        （必须在 pcf8563_read_time() → mktime() → settimeofday() 之前调用，
  │         否则 PCF8563 的北京时间会被当成 UTC 处理，localtime_r 多 +8h 跨天）
  ├─ 1.6 日志级别设置（v1.6.5-2 新增，压掉无害警告；v1.7.1 新增 esp-tls-mbedtls）
  │     ├─ esp_log_level_set("gpio", ESP_LOG_ERROR)          # 消掉 GPIO[10] conflict warning（SDSPI 内部重复配 CS）
  │     ├─ esp_log_level_set("sdspi_transaction", ESP_LOG_ERROR)  # 消掉 CMD5 "command not supported"（普通 SD 卡不支持 SDIO）
  │     └─ esp_log_level_set("esp-tls-mbedtls", ESP_LOG_ERROR)    # 消掉 skip_common_name 警告（OTA HTTPS 用 insecure TLS，MD5 在上层兜着）
  ├─ 2. 硬件初始化
  │     ├─ switch_init()       # GPIO 开关驱动（SWITCH1=GPIO19, SWITCH2=1, POWER=13, LIGHT=18）
  │     ├─ led_init()          # 状态 LED + PWM 定时器（GPIO12）
  │     ├─ temp_sensor_init()  # 内部温度传感器
  │     ├─ ntc_sensor_init()   # 双路 NTC ADC（GPIO3 + GPIO4，ADC1 unit）
  │     ├─ power_monitor_init()# 电源监测（GPIO2 ADC + esp_timer 欠压保护状态机）
  │     ├─ pcf8563_init()      # PCF8563 外部 RTC（I2C0, SCL=GPIO9, SDA=GPIO8）
  │     │                      #   VL=0 读时间 → settimeofday() 写入系统
  │     │                      #   VL=1 或读失败 → 等 SNTP 后再校准
  │     └─ tf_card_init()      # TF 卡初始化（SPI2 + FATFS，可选）
  │
  ├─ 3. 启动 WiFi 管理器（自动连接或进 AP 配网）
  │     └─ wifi_manager_start()  → 创建 fsm_task 状态机
  │
  ├─ 4. ★ 立即启动 Web 服务器（V1.7.0：移到 WiFi 启动后立即调一次，避免主循环重入导致 heap 不足）
  │     └─ start_webserver()   # 有防重入保护：s_server != NULL 直接返回
  │     │                      # 即使 WiFi STA 没连上也能访问 AP 页面
  │
  ├─ 5. 若启动时已连上 WiFi → 立即启动 SNTP + MQTT + OTA
  │     ├─ sntp_init_and_sync()
  │     ├─ mqtt_init()
  │     └─ ota_init()          // 检查上次 OTA 状态（v1.6.3: PENDING_VERIFY 不再立即 mark valid，改为延迟验证）
  │
  └─ 6. 主循环（5秒轮询）
        ├─ 欠压保护中 → 跳过所有服务初始化
        ├─ 检测到 WiFi 连上但服务未启动 → 补启动 SNTP + MQTT + OTA（不再重复调 start_webserver）
        ├─ ota_pending_verify_loop_check()  ← v1.6.3 新增：检查 OTA 延迟验证是否满足 mark valid 条件
        └─ sntp_is_synced() → PCF8563 回写（只触发一次）
```

---

## 3. WiFi Manager（核心组件）

### 3.1 状态机

```
                ┌─────────────┐
                │    IDLE     │
                └──────┬──────┘
                       │ wifi_manager_start()
                       ▼
                ┌─────────────┐    有保存的凭证     ┌───────────────────┐
                │  STA_CONNECT │─────────────────▶ │  STA_CONNECTED    │
                └──────┬──────┘                     └──┬──────────┬───┘
                       │ 无凭证 / 首次连接失败3次        │          │
                       ▼                                │          │ CMD_CLOSE_AP
                ┌─────────────┐                         │          ▼
                │      AP      │◀────────────────────────┘  ┌──────────────┐
                └──────┬──────┘                              │  STA_ONLY    │
                       │ CMD_OPEN_AP (从 STA_ONLY)           └──────┬───────┘
                       │  同时恢复 STA 重连                          │ CMD_OPEN_AP
                       └─────────────────────────────────────────────┘
```

| 状态 | 含义 | WiFi 模式 | AP | STA |
|------|------|-----------|-----|-----|
| IDLE | 未初始化 | - | - | - |
| STA_CONNECT | 首次连接中 | APSTA | ✅ | 连接中 |
| STA_CONNECTED | 运行时已连接 | APSTA | ✅ | ✅ 在线 |
| AP | AP 配网模式 | APSTA | ✅ | 后台重试 |
| STA_ONLY | 纯 STA 模式 | STA | ❌ | ✅/连接中 |

### 3.2 重连策略（两层防护）

```
┌────────────────────────────────────────────────────────────────────┐
│ 第一层：即时重连（DISCONNECTED 事件里）                               │
│                                                                    │
│   运行时掉线（s_sta_ever_connected = true）                        │
│     → 无限重连，永不放弃                                            │
│                                                                    │
│   首次连接（s_sta_ever_connected = false）                          │
│     → 最多 3 次（MAX_RETRY）                                       │
│     → 超过则发 FAIL_BIT → 进 AP 配网模式                            │
├────────────────────────────────────────────────────────────────────┤
│ 第二层：后台重试定时器（retry_timer，ESP Timer 15s/30s）             │
│                                                                    │
│   触发时机：do_runtime_standalone_ap() 结束时                        │
│             （首次连接失败 / 密码错误 / 手动进配网）                  │
│                                                                    │
│   行为：每 30 秒从 NVS 加载凭证 → set_config → connect              │
│   成功：GOT_IP 事件里 stop_retry_timer()                            │
│   停止：连上 / do_close_ap() / CMD_CONNECT_NEW 成功                  │
├────────────────────────────────────────────────────────────────────┤
│ 第三层：IP 丢失处理（IP_EVENT_STA_LOST_IP）                          │
│                                                                    │
│   → DHCP stop → DHCP start（续租）                                  │
└────────────────────────────────────────────────────────────────────┘
```

### 3.3 AP 自动关闭节能机制（双模式）

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
模式 A：开机有凭证（等 MQTT 场景）
  do_wifi_start_once() → AP 全开（给手机 60s 配置窗口）
  s_ap_close_wait_mqtt = true
  start_ap_auto_close_timer()  ← 启动 60s 定时器
       │
       ├─ 60s 后回调 ap_auto_close_cb():
       │    ├─ s_wifi_connected == false → AP 保持开启
       │    │    （用户可能还在等配置 / 密码错了正在 retry_timer 重试）
       │    │
       │    └─ s_wifi_connected == true → 进入 MQTT 等待流程（见模式 C）
       │
       └─ 无凭证开机 → 不启动此定时器（用户需要 AP 配置）

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
模式 B：配网成功后等待 MQTT
  触发场景：CMD_CONNECT_NEW 成功（AP 配网提交新 WiFi 成功）
  s_ap_close_wait_mqtt = true
  start_ap_auto_close_timer()  ← 启动 5s 定时器（立即轮询）
       │
       ├─ 每 5s 回调 ap_auto_close_cb():
       │    ├─ mqtt_is_connected() == false
       │    │    ├─ 累计等待 < 60s → 再等 5s
       │    │    └─ 累计等待 ≥ 60s → 强制关 AP（MQTT 超时保护）
       │    │
       │    └─ mqtt_is_connected() == true → 进入模式 C 延迟关闭
       │
       └─ WiFi 在等待期间断开 → reset 标志 → AP 保持开启
          （重连后不再触发自动关闭，避免干扰）

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
模式 C：MQTT 已连上 → 延迟 60s 关闭 AP（开机 & 配网 共用）
  mqtt_is_connected() == true
       │
       └─ 延迟 AP_CLOSE_DELAY_AFTER_MQTT_SEC = 60s → do_close_ap() ✅
          （给手机留够时间连 AP 查 IP 地址，确认"WiFi+MQTT 全部就绪"）

定时器停止条件:
  - do_close_ap() 里 stop_ap_auto_close_timer()
  - 定时器自然到期（单次触发 / 循环重 schedule）
```

**设计意图**：
- 开机 & 配网 均等待 MQTT 连接成功，让用户通过 LED 状态确认"全部就绪"
- MQTT 连上后再留 60 秒 AP 窗口，方便用户连 AP 查询 STA 的 IP 地址
- MQTT 等待超时保护（60s）：避免因 MQTT 故障导致 AP 永不关闭

### 3.4 AP 配网模式超时节能机制

```
触发场景（仅以下两种纯配网场景）:
  1. 无凭证开机 → do_wifi_start_once(NULL, NULL) 后启动
  2. CMD_CLEAR_AND_AP（恢复出厂）→ 清空凭证后启动

不启动此定时器的场景:
  - 有凭证但首次连接失败 → retry_timer 会后台重试，不是配网等待
  - CMD_CONNECT_NEW 失败 → 凭证已保存，retry_timer 会后台重试

超时逻辑（ap_idle_timeout_cb, 300s 单次触发）:
  ├─ s_wifi_connected == true → stop_ap_idle_timer() 正常退出
  └─ s_wifi_connected == false && s_ap_active == true:
       do_close_ap() → s_state = IDLE  ← 关 AP 节能，WiFi 停转
       用户重启设备 → 恢复正常启动流程（读凭证 / 配网）

停止条件（任一触发即停）:
  - IP_EVENT_STA_GOT_IP 事件（连上了）
  - do_close_ap() 被调用（主动关 / timeout 触发）
  - CMD_CONNECT_NEW 成功
```

**设计意图**：设备长时间配网等待无人操作是极大的功耗浪费。5分钟是合理的配网窗口，超时后彻底关 WiFi（`do_close_ap()` → `esp_wifi_set_mode(STA)` 实际此时没目标网络 → WiFi 基本零功耗）。用户发现配网超时后重启即可恢复。

### 3.5 事件处理

| 事件 | 处理 |
|------|------|
| `WIFI_EVENT_STA_START` | 若允许连接 → 自动 `esp_wifi_connect()` |
| `WIFI_EVENT_STA_DISCONNECTED` | 三段式重连（见上）+ 停 RSSI + 清状态 |
| `IP_EVENT_STA_GOT_IP` | 设 `s_sta_ever_connected=true` + 停 retry_timer + 启动 RSSI + mDNS 注册 |
| `IP_EVENT_STA_LOST_IP` | DHCP 续租 |

### 3.5 命令队列（外部 → 状态机）

| 命令 | 触发场景 | 行为 |
|------|---------|------|
| `CMD_CONNECT_NEW` | Web 配置页提交新 WiFi | `do_runtime_switch()` 热切换 |
| `CMD_CLEAR_AND_AP` | 恢复出厂设置 | 清 NVS + 进 AP 模式 + 启动 retry_timer |
| `CMD_CLOSE_AP` | 关闭热点 | `esp_wifi_set_mode(STA)` + 停 retry_timer + 停 auto_close_timer |
| `CMD_OPEN_AP` | 打开热点 | `esp_wifi_set_mode(APSTA)` + **恢复 STA 重连** |

### 3.6 关键变量

```c
static bool s_sta_ever_connected = false;  // 标记曾连接成功 → 运行时掉线无限重试
static bool s_sta_connect_allowed = false;  // 是否允许 STA 连接
static bool s_wifi_connected = false;      // 当前 STA 是否已连上
static bool s_ap_active = false;           // 当前 AP 是否开启
static esp_timer_handle_t s_retry_timer;   // 后台重试定时器
static esp_timer_handle_t s_rssi_timer;    // RSSI 周期更新定时器（5秒）
static esp_timer_handle_t s_ap_auto_close_timer;  // AP 自动关闭定时器（60秒开机 / 5秒轮询MQTT）
static esp_timer_handle_t s_ap_idle_timer;       // AP 配网超时定时器（300秒，纯配网场景）
static bool s_ap_close_wait_mqtt = false;   // 配网场景：关 AP 前先等 MQTT 连上
static int  s_ap_wait_mqtt_elapsed = 0;     // MQTT 等待累计秒数（超时保护）
static EventGroupHandle_t s_event_group;   // CONNECTED_BIT / FAIL_BIT
static QueueHandle_t s_cmd_queue;          // 外部命令队列（长度 8）
```

### 3.6 NVS 存储（命名空间 `wifi_cfg`）

| Key | 类型 | 内容 |
|-----|------|------|
| `ssid` | str32 | 保存的 WiFi SSID |
| `pass` | str64 | 保存的 WiFi 密码 |

### 3.7 对外 API

```c
// 状态查询
wifi_state_t wifi_get_state(void);
bool wifi_is_connected(void);       // STA 是否已连上网
bool wifi_is_ap_active(void);       // AP 热点是否开启
const char *wifi_get_ip(void);       // 优先 STA IP，否则 AP IP
const char *wifi_get_sta_ip(void);
const char *wifi_get_ap_ip(void);
int wifi_get_rssi(void);
const char *wifi_get_status_text(void);

// 控制
void wifi_manager_start(void);
void wifi_manager_request_connect(ssid, pass);
void wifi_manager_request_clear_and_ap(void);
void wifi_manager_request_close_ap(void);
void wifi_manager_request_open_ap(void);

// 工具
esp_err_t wifi_scan_aps(out, count);
bool wifi_cred_load(cred);
bool wifi_cred_save(cred);
void wifi_cred_clear(void);
void wifi_update_rssi(void);
```

---

## 4. MQTT 阿里云（第二核心状态机）

### 4.1 状态机

```
    IDLE ──mqtt_init()──▶ WAIT_WIFI
        ▲                      │
        │                      │ wifi_is_connected()
        │                      ▼
        │                 WAIT_TIME ──SNTP 同步完成──▶ INIT
        │                      │                          │
        │                      │ 60秒超时                  │ esp_mqtt_client_start()
        │                      ▼                          ▼
        │                 INIT（重试）              CONNECTING
        │                                                  │
        │                                         connect 成功
        │                                                  ▼
        │                                              CONNECTED
        │                                                  │
        └────── wifi 断开 / error ◀────────────────────────┘
```

| 状态 | 含义 |
|------|------|
| IDLE | 未初始化 |
| WAIT_WIFI | 等待 WiFi 联网 |
| WAIT_TIME | 等待 SNTP 时间同步（最多 60s） |
| INIT | 创建 MQTT 客户端 |
| CONNECTING | 正在连接阿里云 |
| CONNECTED | 已连接，正常工作 |
| ERROR | 连接出错 → 3秒后回到 INIT |

### 4.2 关键设计

- **WiFi 断了自动回退**：`CONNECTING`/`CONNECTED` 状态下检测到 `!wifi_is_connected()` → 回到 `WAIT_WIFI`
- **重发机制**：PUBREC/PUBREL 未收到 ACK 会重发
- **RX 环形缓冲区**：**4 条** `mqtt_rx_entry_t`（v1.6.3: 从 8 条缩减，节省静态 RAM），每条 data 最大 **512 字节**（v1.6.3: 从 2048 缩减）。静态 RAM 从 ~16.6KB 降到 ~2.3KB
- **阿里云参数上报**：`mqtt_publish_aliyun_params(json)` 用 `/sys/{device}/thing/event/property/post` 主题
- **OTA 消息分发**：RX 事件检测到 topic 含 `/ota/upgrade` → 直接调用 `ota_handle_mqtt_msg()` 进入 OTA 流程（跳过常规 JSON 解析）
- **MQTT 缓冲区**：`.buffer.size` / `.buffer.out_size` = **2048**（原为 512，防止 OTA 推送 JSON 被内部截断）
- **内部任务栈**：`.task.stack_size` = **8192**（显式设置，防止事件回调内 subscribe + publish 栈溢出）；**mqtt_manager_task 栈**：**8192**（V1.7.1 从 4096 升级，原因：上报 snprintf 含 7 个 `%.2f` 浮点格式化，newlib 每个 %f 需 ~300 字节临时栈，4096 不足以覆盖浮点格式化 + 调用链 + 局部变量）
- **事件回调内执行 subscribe**：MQTT_EVENT_CONNECTED 里直接调 esp_mqtt_subscribe 三个 OTA 主题，返回值检查 + 错误日志
- **MQTT 连接成功附加动作**：MQTT_EVENT_CONNECTED 里除 subscribe 外，还会发 OTA inform（上报当前 APP_VERSION）+ 调用 `ota_maybe_post_reboot_ok()`（若上次 OTA 升级后重启，补发 step="0" 二次确认）
- **上报 + 写卡同步**：`MQTT_STATE_CONNECTED` 每 30s 周期里，**先执行 TF 卡巡检**（掉了就 reinit）→ **重连判断**（若 `s_is_reconnect=true` 则跳过本次上报，等满 30s 再进入正式周期，避免重连产生额外 CSV 行）→ 组报文 → publish → publish 成功后立即读取真实 NTC 温度 → 调用 `tf_card_append_csv()` 自动按日期写入 `/tf/logs/YYYY-MM-DD.csv`。上报数据与 CSV 行数据完全一致，时间戳精确对齐
- **TF_state 状态字段**（v1.6.5-2 新增, v1.7.1 优化）：周期上报 JSON 在 RSSI 后插入 `"TF_state":1/0` 字段。值来自 `tf_card_get_state()` — 返回 `s_cached_state` 缓存值（0=异常/未挂载, 1=正常）。缓存更新时机：① `tf_card_periodic_check()` 每次巡检 probe 后更新（MQTT task 30s 驱动）② `tf_card_init()` 成功设 1 / `tf_card_deinit()` 设 0 / `tf_card_reinit()` 失败设 0。卡拔掉后下一个 30s 周期立即变 0。**不再每次 MQTT 组报文时都发 SPI CMD0**，省一次 SPI 事务
- **SNTP 未同步时缓存**：SNTP 未同步时 `time(NULL) < 1704067200`，CSV 行暂存到 s_csv_pending[8] 环形缓冲（最多 4 分钟），下次时间有效后自动刷入对应日期文件
- **MQTT 断开时 flush TF 缓存 + 标记重连**：`MQTT_EVENT_DISCONNECTED` 事件里①调用 `tf_card_flush()` 把内存缓存中的 CSV 行紧急写入 TF 卡防止丢失；②设 `s_is_reconnect = true`，下次重连进入 CONNECTED 状态时跳过首次立即上报，等满 30s 再进入正式周期，避免产生额外 CSV 行（正常启动首次连 MQTT 时不会置此标志，不影响首启立即上报）
- **历史查询暂停主动上报**：`history_query_handle()` 收到 QUERY 命令时调 `mqtt_pause_report_ms(30000)` 暂停 30 秒主动上报，查询完成/取消时立即 `pause(0)` 恢复。MQTT 周期上报循环里检查 `s_pub_pause_until_ms`，暂停期间跳过本次上报。查询优先于主动上报

### 4.3 对外 API

```c
void mqtt_init(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_publish_custom(topic, data, qos);
esp_err_t mqtt_publish_aliyun_params(params_json);
int mqtt_get_rx_entries(out, max_count);
const char *mqtt_get_last_rx_topic(void);
const char *mqtt_get_last_rx_data(void);

// 历史查询暂停主动上报（0=立即恢复，>0=暂停N毫秒）
void mqtt_pause_report_ms(int pause_ms);

float mqtt_get_field_a(void);
float mqtt_get_field_b(void);
float mqtt_get_set_a(void);
float mqtt_get_set_b(void);
float mqtt_get_field1_data(void);   // ← V1.7.1: 返回 float（原 int）
float mqtt_get_field2_data(void);   // ← V1.7.1: 返回 float（原 int）
```

---

## 5. OTA 升级（ota_manager.c）

### 5.1 触发流程

**阿里云 MQTT 触发**（主路径）：

```
阿里云控制台推送 /ota/upgrade
  │
  ▼
mqtt_aliyun.c RX 事件检测到 topic 含 "/ota/upgrade"
  │
  ▼
ota_handle_mqtt_msg()  ← v1.6.3 改了完整协议闭环
  ├─ 解析 JSON（cJSON）→ ota_notify_t
  ├─ MQTT 连接检查
  ├─ OTA 忙中? → reply_upgrade(code=200 "busy") → 跳过（防重发）
  ├─ 版本相同且非强制? → reply_upgrade(code=200 "already latest") + report_result("0") → 跳过
  ├─ ✅ reply_upgrade(code=200 "accepted") ← 关键！告诉阿里云"我收到了"
  └─ 创建 ota_task（优先级 5，栈 **12288** 字节）
```

**⚠️ v1.6.3 阿里云 OTA 协议修复**（之前反复重推的根因）：
1. **必须立即 reply_upgrade(code=200)**：阿里云 OTA 推送后 30s 内必须收到 code=200 确认，否则认为"设备离线/不响应"→ **每次 MQTT 重连都会重发同一条推送**
2. **版本比对 + 跳过**：`fw_version == APP_VERSION && !force_upgrade` → 立即 reply 200 + report_result("0")，避免同版本反复刷写重启
3. **所有路径都 reply**：包括错误路径（parse 失败 code=-1）、OTA 忙中（code=200 "busy"），**不能有任何路径静默 return 不 reply**

**⚠️ v1.6.4 bin header 版本一致性修复**（更隐蔽的反复重推根因）：
- ESP-IDF bin header 的版本号来自 `CMakeLists.txt` 的 `project(... VERSION x.y.z)`，**不是** `main/version.h` 的 `APP_VERSION` 宏
- 不设 VERSION 时 → 自动用 `git describe` → `"V1.1.0-27-ge35bbb0-dirty"` 这种字符串
- 之前只改了 version.h 但 CMakeLists.txt 没设 VERSION → OTA 刷完重启后，o ta inform 上报 `APP_VERSION="1.6.3"`，但 bin header 里是 `"V1.1.0-..."` → 云端 fwVersion="1.6.3" 对不上 → 继续推 → 无限循环
- **三层修复**：① CMakeLists.txt 加 `VERSION 1.6.4` ② version.h APP_VERSION 保持同步 ③ OTA 成功路径加 bin header vs fwVersion 校验（不匹配拒绝 set_boot）
- 见 **5.6 分区切换与重启** 节的详细说明

**TF 卡触发**（开机自动检测）：

```
开机 main.c app_main()
  │
  ├─ tf_card_init() 成功后立即调用
  ▼
ota_check_tf_on_boot()
  ├─ 等 TF mount（最多 5s）
  ├─ stat /tf/firmware.bin（存在且 > 64B）
  ├─ 读 bin 版本号（magic=0xE9 + esp_app_desc_t 二次验证）
  │     └─ 非法 → 删除损坏文件，跳过
  ├─ ⚠️ 跳过版本新旧比较（有合法 bin 就升级）
  └─ 创建 tf_ota_task（同 ota_task 参数）
```

### 5.2 OTA 状态机（ota_state_t）

| 状态 | 含义 |
|------|------|
| OTA_STATE_IDLE | 空闲，等待 OTA 通知 |
| OTA_STATE_DOWNLOADING | HTTP 下载中，每 5% 上报进度 |
| OTA_STATE_VERIFYING | MD5 校验 + esp_ota_end |
| OTA_STATE_SWITCHING | esp_ota_set_boot_partition 切换启动分区 |
| OTA_STATE_REBOOT_WAIT | 成功后 2s 等待 → esp_restart() |
| OTA_STATE_FAILED | 任意步骤失败 → cleanup + LED 错误指示 |

### 5.3 阿里云 OTA 消息格式

**下行推送**（设备收到）：`/sys/{productKey}/{deviceName}/ota/upgrade`

```json
{
  "id": "msg-id-001",
  "version": "1.0",
  "params": {
    "fwId": "firmware-001",
    "fwUrl": "https://xxx.oss-cn-hangzhou.aliyuncs.com/app.bin",
    "fwVersion": "1.3.4",
    "fwSign": "md5hexdigest32chars",
    "signMethod": "MD5",
    "forceUpgrade": false
  }
}
```

**上行回复**（设备发送）：

| 主题 | 用途 |
|------|------|
| `/sys/{pk}/{dn}/ota/upgrade_reply` | 对每次推送的即时回复（code=200 接收 / code=-1 拒绝） |
| `/sys/{pk}/{dn}/ota/post` | 进度上报（step="2" + percent），最终结果（step="0" 成功 / step="3" 失败），**重启后二次确认（step="0" + APP_VERSION）** |

### 5.4 HTTP 下载（含重试 & WiFi 防护 & TLS）

- **缓冲区**：4096 字节（`OTA_BUFFER_SIZE`）
- **超时**：30s（`OTA_HTTP_TIMEOUT_MS`）
- **写入方式**：`esp_ota_write()` 直写目标 OTA 分区，不暂存 RAM
- **进度上报**：每 5% 通过 `/ota/post` 上报一次
- **HTTP 连接重试**：最多 **3 次**（`OTA_HTTP_RETRY_MAX`），间隔 **1500ms**（`OTA_HTTP_RETRY_INTERVAL_MS`）
- **单次读取重试**：HTTP 读失败后最多 **2 次**（`OTA_READ_RETRY_MAX`），间隔 **200ms**（`OTA_READ_RETRY_INTERVAL_MS`）
- **WiFi 断开检测**：每次读取前检查 `wifi_is_connected()`，WiFi 断了立即中止（`OTA_DOWNLOAD_WIFI_LOST`）
- **用户取消**：`ota_abort()` 设 `s_cancel_requested=true`，下载循环检测后返回 `OTA_DOWNLOAD_CANCELLED`
- **分区大小预校验**：下载前比对阿里云推送的 `size` 字段与目标分区 `target->size`，超分区容量则提前中止（避免下载到一半才发现）

#### URL 预处理（trim_url_inplace）

阿里云 OTA 推送的 URL 可能带反引号 `` ` `` 或引号，HTTP 请求前统一 trim：
- 前后的反引号、双引号、单引号、空格、制表符、换行符全部去除
- 原字符串就地 `memmove`，不分配额外内存
- 调用两次：`parse_notify` 里 trim 一次，`do_http_download_once` 里兜底 trim 一次（防御性编程）

#### TLS / HTTPS 配置（sdkconfig 强制）

阿里云 OSS 只支持 HTTPS，HTTP 静默拒绝连接。ESP-IDF v6.x 默认强制 TLS 证书验证，必须显式关闭：

```ini
# sdkconfig
CONFIG_ESP_TLS_INSECURE=y                  # 允许 insecure TLS 模式
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y   # 跳过服务器证书验证
```

HTTP 客户端配置：
```c
.skip_cert_common_name_check = true,       // 同时跳过 CN/SAN 主机名校验
```

**安全补偿**：传输层不验证，固件完整性由上层 **MD5 签名校验**（下载过程中累积哈希）保证。

#### HTTP 响应头 fetch_headers 兜底

ESP-IDF v6.x 的 `esp_http_client_open` 在某些场景下返回 OK 但 `status_code=0`（响应头未自动解析），此时主动调 `esp_http_client_fetch_headers()` 显式读取响应头，确保拿到正确的 status 和 content_len。

#### HTTP URL scheme 校验

下载前检查 URL 是否以 `https://` 或 `http://` 开头，避免脏数据导致 HTTP 客户端内部 panic。

#### 重启后二次确认（Reboot OK Confirm）

OTA 升级成功后（重启前）会发送 `report_result("0", version)`，但如果设备在重启过程中掉电，云端会误认为升级成功而实际未生效。因此增加重启后的二次确认机制：

```
ota_init() 检测到 ESP_OTA_IMG_PENDING_VERIFY
  └─ s_ota_pending_verify = true  ← v1.6.3: 不再立即 mark！

...WiFi 连上 → SNTP → MQTT init → MQTT 连接成功...
mqtt_event_handler() MQTT_EVENT_CONNECTED:
  └─ ota_maybe_post_reboot_ok()  ← 此时 s_ota_need_post_reboot_ok = false → 跳过（正常）

...主循环每 5s 调 ota_pending_verify_loop_check()...
  └─ MQTT 已连 + 跑满 30s
       ├─ esp_ota_mark_app_valid_cancel_rollback()
       ├─ s_ota_need_post_reboot_ok = true
       └─ ota_maybe_post_reboot_ok()   ← ⚠️ 必须主动调！否则没人再触发 → 阿里云收不到 reboot_ok → 无限重推！
            ├─ s_ota_need_post_reboot_ok = true ✅
            ├─ 清标志
            └─ report_result("0", APP_VERSION)  ← 重启后 step="0" 确认，闭环完成
```

- **标志存储**：`static volatile bool s_ota_need_post_reboot_ok`（volatile 保证跨任务可见性）
- **触发时机**：两条路径都可能触发
  1. **主循环主动调**（关键路径）：`ota_pending_verify_loop_check()` 设完标志后立即主动调用，确保 reboot_ok 在 30s 验证通过时就发出
  2. **MQTT 重连兜底**：每次 MQTT 连接成功时 `mqtt_aliyun.c` 也会调一次（幂等，标志已清则跳过）
- **幂等安全**：标志在发送一次后立即清零，即使 MQTT 多次重连也不会重复上报
- **v1.6.3 改变**：设标志时机从 `ota_init()`（启动立即）延后到 `ota_pending_verify_loop_check()`（MQTT 连上 + 跑满 30s），与 mark valid 同步，防止未验证通过就上报"升级成功"
- **⚠️ v1.6.3.1 关键修复**：延迟验证后 `ota_pending_verify_loop_check()` 设完标志必须**主动调一次** `ota_maybe_post_reboot_ok()`。之前只设了标志没调函数，MQTT 后续也不会重连 → reboot_ok 永远发不出去 → 阿里云认为升级没完成 → 无限重推同版本
- **正常重启无影响**：非 OTA 场景下 `img_state` 不是 `PENDING_VERIFY`，标志始终为 false

### 5.5 MD5 校验

- 使用 mbedtls v3 API：`mbedtls_md_init()` + `mbedtls_md_setup()` + `mbedtls_md_starts()`
- 下载过程中持续 `mbedtls_md_update()` 累积哈希
- 下载完成后 `mbedtls_md_finish()` → 32 位小写十六进制字符串
- 与 `fwSign` 忽略大小写比对（`strcasecmp`）
- **降级策略**：MD5 初始化失败不中止 OTA，继续写入（仅在 signMethod=MD5 且 fwSign 非空时才比对）

### 5.6 分区切换与重启

1. `esp_ota_end(ota_handle)` → 提交固件到目标分区
2. **v1.6.4 新增：bin header 版本一致性校验**（防止 bin 内 APP_VERSION / CMake VERSION 和云端 fwVersion 不匹配）
   - `esp_ota_get_partition_description(target, &new_desc)` → 读取刚写入的 bin header 版本
   - `strcmp(new_desc.version, notify->fw_version)` → 两者不一致 → report_result("3") → 拒绝 set_boot → 不重启
   - **根因背景**：ESP-IDF 的 bin header 版本由 `CMakeLists.txt` 的 `project(... VERSION x.y.z)` 决定（不设则用 `git describe` 生成乱七八糟的字符串如 `V1.1.0-27-ge35bbb0-dirty`），和 `main/version.h` 的 `APP_VERSION` 宏是两套独立系统。之前 version.h 改了但 CMakeLists.txt 没设 VERSION → OTA 刷完重启后 ota inform 上报的 APP_VERSION 和云端推的 fwVersion 对不上 → 云端认为升级没生效 → 无限重推同版本
3. `esp_ota_set_boot_partition(target)` → 设下次启动用新分区
4. 成功时 `led_notify_ota_success()` → LED 3 快闪 + 常亮
5. 等待 **3s**（`OTA_REBOOT_DELAY_MS`）→ `esp_restart()`

**⚠️ v1.6.5 版本号单一源头改造**（彻底消除双源不同步风险）：

| 位置 | 作用 | 谁改 |
|------|------|------|
| `CMakeLists.txt` `project(... VERSION x.y.z)` | **唯一源头**，写入 bin header | **只改这一处** |
| `main/version.h` `APP_VERSION` | 代码里用的宏（自动生成） | **configure_file 自动生成，不手动改** |
| 阿里云控制台推送 `fwVersion` | OTA 云端期望的目标版本 | 控制台手动填 |

**原理**：`main/version.h.in` 模板里写 `#define APP_VERSION "@CMAKE_PROJECT_VERSION@"`，`main/CMakeLists.txt` 末尾 `configure_file(version.h.in version.h @ONLY)`，每次构建自动把顶层 VERSION 注入 version.h。ESP-IDF component 作用域隔离，用 `@CMAKE_PROJECT_VERSION@`（CMake 全局内置变量）才能穿透。

**v1.6.4 双源 bug 回顾**：之前只改了 version.h 但 CMakeLists.txt 没设 VERSION → bin header 里是 `git describe` 生成的乱字符串 → OTA 刷完重启后 bin header vs fwVersion 校验失败 → 无限重推。v1.6.5 从根源上解决。

#### v1.7.1 版本管理工具链

**版本号只在一处改**：顶层 `CMakeLists.txt` 的 `project(... VERSION x.y.z)`，其他全自动同步。v1.7.1 新增 `version_bump.py` 作为唯一入口工具，取代之前改错文件的旧脚本（旧脚本改的是 version.h 生成文件，根本不碰 CMakeLists.txt 的 VERSION，等于废的）。

**同步链路**：

```
唯一源头：CMakeLists.txt  project(ESP32_C3_OLED VERSION 1.7.1)
              │
              ├── configure_file → main/version.h  自动生成 APP_VERSION="1.7.1"
              │
              ├── 编译时写入 → bin header esp_app_desc_t.version = "1.7.1"
              │
              └── CMake 配置时 message → 终端打印当前版本（盲发前可一眼看到）
```

**version_bump.py 用法**：

```powershell
python main/version_bump.py patch      # 1.7.1 → 1.7.2
python main/version_bump.py minor      # 1.7.1 → 1.8.0
python main/version_bump.py major      # 1.7.1 → 2.0.0
python main/version_bump.py set 1.8.3  # 直接指定目标版本

# 或者用 idf.py alias（顶层 CMakeLists.txt 已定义 custom target）
idf.py bump-version           # patch +1
idf.py bump-version-minor     # minor +1
idf.py bump-version-major     # major +1
```

**顶层 CMakeLists.txt 构建时版本自显**（v1.7.1 新增）：

```
-- ========== ESP32-C3 OLED ==========
--   Project version : 1.7.1
--   bin header ver  : 1.7.1  (written by CMake, single source)
-- ====================================
```

**⚠️ v1.7.1 OTA 失败根因复盘**（2026-09-25 触发）：

改了 CMakeLists.txt VERSION 1.7.0 → 1.7.1，但**忘了 clean rebuild**（configure_file 不重跑），导致 bin header 里仍是旧的 1.7.0，和阿里云控制台填的 fwVersion=1.7.1 对不上 → bin header vs fwVersion 校验失败 → 拒绝 set_boot。

**这个 OTA 版本校验不能跳过**——它是 v1.6.4 加的安全网，防止云端和设备版本号对不上时设备瞎重启。根因永远在"某处没改对"，而不是要关校验。

#### 发版 checklist

每次发布 OTA 固件时按此执行：

- [ ] `version_bump.py` 改版本号（脚本只动 CMakeLists.txt 一处，其他自动同步）
- [ ] `idf.py fullclean && idf.py build`（configure_file 必须重跑，确保 version.h 重新生成）
- [ ] 看构建日志里的版本自显，确认 Project version = 你期望的值
- [ ] 新 bin 上传阿里云 OTA 控制台，**fwVersion 填的和 CMakeLists.txt VERSION 一模一样**
- [ ] OTA 失败报 `fw version mismatch` 时 → 两边对一下：CMakeLists.txt VERSION / bin header version（可 `esptool.py image_info build/xxx.bin` 看）/ 阿里云控制台 fwVersion，三者必须对齐

### 5.7 开机恢复（ota_init）

**⚠️ 延迟验证机制（v1.6.3 新增）**：新固件启动后**不立即** `mark_app_valid`，而是等主循环确认系统完全跑起来后再标记。如果新固件在验证前 crash，bootloader 会自动回滚旧分区。

```
                    固件 A (稳定版，当前运行)
                          │
                    OTA 刷入固件 B
                          │
                    esp_restart() ↓
            bootloader 启动固件 B (状态 = PENDING_VERIFY)
                          │
                ota_init() ──► s_ota_pending_verify = true
                          │    (不 mark valid，不重启二确认)
                          │
              ┌───────────┴───────────┐
              │                       │
         主循环每 5s 检查:          crash / 重启
         mqtt_is_connected()?        (跑不到 30s)
         运行满 30s?                  │
              │                  bootloader 检测到
              │                  "B 没被验证通过"
         两个条件都满足                │
              │                 自动回滚到 A ✅
     esp_ota_mark_app_valid_cancel_rollback()
     s_ota_need_post_reboot_ok = true
              │
       B 永久有效 + MQTT 连上后补发 reboot_ok 确认
```

```c
void ota_init(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state;
    esp_ota_get_state_partition(running, &img_state);

    if (img_state == ESP_OTA_IMG_PENDING_VERIFY) {
        // ⚠️ 不再立即 mark_app_valid！改为延迟验证
        s_ota_pending_verify = true;
        s_pending_verify_boot_ms = esp_log_timestamp();
        ESP_LOGI(TAG, "OTA PENDING VERIFY - 等 MQTT 连上 + 跑满 30s 后才 mark valid");
    }
    if (img_state == ESP_OTA_IMG_ABORTED) {
        ESP_LOGW(TAG, "Previous OTA aborted, device may have rolled back");
    }
}

// 主循环每 5s 调用一次
void ota_pending_verify_loop_check(void) {
    if (!s_ota_pending_verify) return;

    uint32_t elapsed = (esp_log_timestamp() - s_pending_verify_boot_ms) / 1000;
    bool mqtt_ok = mqtt_is_connected();

    if (mqtt_ok && elapsed >= OTA_VERIFY_MIN_RUNTIME_SEC) {
        esp_ota_mark_app_valid_cancel_rollback();
        s_ota_pending_verify = false;
        s_ota_need_post_reboot_ok = true;
        ota_maybe_post_reboot_ok();   // ← 主动上报 reboot_ok 给阿里云，否则云端一直重推
        ESP_LOGI(TAG, "OTA pending verify: MQTT connected + %lus elapsed, marking valid", elapsed);
    }
}
```

| 常量 | 值 | 说明 |
|------|-----|------|
| `OTA_VERIFY_MIN_RUNTIME_SEC` | **30** | 新固件最少稳定运行秒数，过了这个阈值才 mark valid |

**回滚触发条件**（自动）：
- 新固件 crash 在 `ota_pending_verify_loop_check()` 被调用之前 → bootloader 检测到分区仍为 PENDING_VERIFY → 自动回滚旧分区
- 新固件能启动但业务 bug 导致 MQTT 始终连不上 / 30s 内反复重启 → 也会触发回滚

**不触发回滚**的正常路径：新固件 MQTT 正常连上 + 跑满 30s → mark_app_valid 永久生效

### 5.8 OTA LED 联动

| OTA 阶段 | LED 函数 | LED 表现 |
|----------|----------|---------|
| 下载开始 | `led_notify_ota_start()` | **双闪**：短亮→短灭→短亮→长灭（BLINK_DOUBLE）+ 设 `s_ota_active=true` |
| 下载成功 | `led_notify_ota_success()` | **3 次快闪 → 常亮**（BLINK_GOOD，占空比 500）+ `s_ota_active=false` |
| 失败 | `led_notify_ota_fail()` | **快速闪烁**（BLINK_BAD，75ms 周期）+ `s_ota_active=false` |

**防覆盖机制**：`resolve_status_mode()` 里第一行检查 `if (s_ota_active) return;`，OTA 期间 LED 完全由 OTA 状态决定，MQTT/WiFi 状态变化不会覆盖掉下载中双闪效果。

### 5.9 关键变量

```c
static ota_state_t  s_state;           // 当前 OTA 状态
static int          s_progress;        // 下载进度 0-100
static char         s_target_version[32];  // 目标固件版本号
static char         s_error_reason[128];   // 失败原因（128 字节）
static volatile bool s_ota_in_progress;    // OTA 进行中（防止重入）
static volatile bool s_cancel_requested;   // 用户取消请求
static TaskHandle_t s_ota_task_handle;     // OTA 任务句柄
static mbedtls_md_context_t s_md5_ctx;     // MD5 上下文
static bool         s_md5_ctx_ready;       // MD5 ctx 是否初始化成功

// v1.6.3 新增：延迟验证机制
static volatile bool s_ota_pending_verify;       // 新固件未验证（PENDING_VERIFY）
static uint32_t     s_pending_verify_boot_ms;   // 新固件启动时刻（esp_log_timestamp）
static volatile bool s_ota_need_post_reboot_ok; // 验证通过后 MQTT 连上要补发 reboot_ok
```

### 5.10 TF 卡 OTA（本地固件升级）

用户把编译好的固件命名 `firmware.bin` 放到 TF 卡根目录，设备开机自动检测并升级。

#### 核心文件与常量

| 项目 | 值 | 说明 |
|------|-----|------|
| bin 路径 | `/tf/firmware.bin` | TF 卡根目录固定名 |
| 扫描缓冲区 | 4096 字节 | `TF_OTA_SCAN_BUF_SIZE`，流式读取零 RAM 压力 |
| Flash header magic | `0xE9` | ESP32-C3 bin 文件头第一字节 |
| 等待 TF mount | 最多 5s | `ota_check_tf_on_boot()` 循环检测 |

#### bin 版本读取原理（read_version_from_bin_file）

**为什么不手动解析 bin header？**

曾试过 3 种方案，全部翻船：

| 方案 | 死因 |
|------|------|
| 硬编码 8+24=32 字节偏移 | ESP32 segment header 没有 magic（ESP8266 才有 0xE5），偏移全错 |
| C struct 读 16 字节 header | 编译器 struct padding 导致 `tf_ota_image_header_t` 变成 20 字节而非 16 字节 |
| malloc 整个 bin（1.2MB） | ESP32-C3 最大连续 RAM 才 **106KB**，OOM |

**最终方案：流式搜索 magic word**

```
步骤：
1. 读前 16 字节 → 验证 flash magic 0xE9 ✅（文件是否是合法 ESP32 bin）
2. 分配 4KB 缓冲，从 offset 16 开始流式读取
3. 逐字节搜索 ESP_APP_DESC_MAGIC_WORD = 0xABCD5432
   小端字节序 = [32 54 CD AB]
4. 跨块边界状态机匹配（match_state 0→1→2→3→4）
   → 即使 magic 被切在两块之间也能正确命中
5. 命中后 fseek 回该位置，读完整 esp_app_desc_t（约 108 字节）
6. 二次验证 desc.magic_word == ESP_APP_DESC_MAGIC_WORD
   → 过滤掉文件内容中偶然撞对的 magic bytes
7. 提取 desc.version 字符串
```

**优势**：
- ✅ 不依赖 bin 格式解析（永远不会因 padding/偏移错挂）
- ✅ 只占 **4KB RAM**
- ✅ 跨块边界安全（状态机匹配）
- ✅ 完整二次验证

#### TF OTA 触发流程（ota_check_tf_on_boot）

```
ota_check_tf_on_boot()
  │
  ├─ 等 TF mount（最多 5s，每秒检查一次）
  │     └─ 超时 → 日志 "TF card not mounted after 5s, skip" → return
  │
  ├─ stat("/tf/firmware.bin", &st)
  │     └─ 文件不存在 → "no bin at /tf/firmware.bin" → return
  │     └─ st_size < 64 → "bin too small" → unlink 删除 → return
  │
  ├─ read_version_from_bin_file()
  │     └─ 失败 → "failed to parse bin version" → unlink 删除损坏文件 → return
  │     └─ 成功 → 得到 bin_version（可能含 V 前缀或 git describe 后缀）
  │
  ├─ ⚠️ 跳过版本新旧比较（只要 bin 合法就升级！）
  │
  └─ 创建 tf_ota_task → 写入目标 OTA 分区 → 成功删 bin → 重启
```

#### tf_ota_task 执行流程

与阿里云 MQTT OTA 的 `ota_task` 基本一致，区别：

| 环节 | 阿里云 OTA | TF 卡 OTA |
|------|-----------|----------|
| 触发 | MQTT 消息 | 开机自动检测 |
| 下载源 | HTTP(s) 从云端拉 | fread 本地 TF 卡 |
| MD5 校验 | ✅ 有 signMethod/fwSign | ❌ 跳过（bin 完整性由 esp_ota_end 自身校验保证） |
| 进度上报 | ✅ 每 5% 通过 MQTT 上报 | ❌ 跳过（无 MQTT 连接时不需要） |
| 成功后 | report_result("0") + LED + 3s 延迟 + restart | **unlink 删除 bin** + LED + 3s 延迟 + restart |
| 失败后 | report_result("3") + LED | bin 保留（方便用户排查/重试）+ LED |

#### bin 文件命名规范

固件编译产物 `build/ESP32_C3_OLED.bin`，手动操作：
- 改名为 `firmware.bin` → 复制到 TF 卡根目录 → 重启设备
- 升级成功后 `firmware.bin` 自动被删除
- 升级失败（bin 损坏）时文件也被删除（避免每次开机重复报错）

#### TF OTA 关键代码位置

| 函数/宏 | 位置 | 作用 |
|---------|------|------|
| `ota_check_tf_on_boot()` | ota_manager.c | 开机入口，等 TF mount + 检查 bin + 启动 tf_ota_task |
| `read_version_from_bin_file()` | ota_manager.c | 流式扫描 magic word 读 bin 版本号 |
| `tf_ota_task()` | ota_manager.c | TF OTA 主任务（读 bin → 写 OTA 分区 → 删 bin → 重启） |
| `TF_OTA_BIN_PATH` | ota_manager.c | `/tf/firmware.bin` |
| `TF_OTA_SCAN_BUF_SIZE` | ota_manager.c | 4096 字节扫描缓冲 |

### 5.11 对外 API

```c
void ota_init(void);                                  // 开机恢复 + 初始化二值信号量（v1.6.3: 不再立即 mark valid）
void ota_pending_verify_loop_check(void);            // v1.6.3 新增：主循环每 5s 调用，检查是否满足 mark valid 条件
bool ota_is_in_progress(void);                        // OTA 是否正在进行
void ota_handle_mqtt_msg(topic, topic_len, data, data_len);  // MQTT 消息入口（在 mqtt_aliyun.c 中被调用）
void ota_get_status(ota_status_t *out);              // 查询当前状态、进度、版本、错误信息
void ota_abort(void);                                 // 请求中止正在进行的 OTA（设 s_cancel_requested）
```

---

## 6. Web Server

- **底层**：`esp_http_server`
- **线程**：HTTP 内置任务（与 WiFi 同任务优先级）
- **作用**：
  - 展示设备状态页面（WiFi 状态 / MQTT 状态 / 温度 / RSSI / 运行时长）
  - WiFi 配网页面（扫描 + 提交 SSID/密码 → `wifi_manager_request_connect()`）
  - REST API（JSON 接口查询）
  - 恢复出厂设置 → `wifi_manager_request_clear_and_ap()`

### v1.6.3 Web 页面优化

| 项目 | 之前 | 之后 | 收益 |
|------|------|------|------|
| HTML 生成方式 | `malloc(12288)` 一次性生成整个页面 → `httpd_resp_send()` 一次发送 | 栈上 `vb[2048]` + 4 次 `httpd_resp_send_chunk()` 分块发送 | **省 ~10KB heap 峰值**，Web 访问时不再 OOM 风险 |
| Web 页面 MQTT Tab | 有"发送 (标准报文)"区域（textarea + 发送/恢复按钮 + JS `mqttSend()` / `resetSendData()`） | **已删除** | 页面更简洁，减少 HTML 字节数 |

### v1.6.5-2 Web Server TF 状态实时同步

| 问题 | 根因 | 修复 |
|------|------|------|
| web 页 TF 状态拔卡不更新 | ① `tf_card_is_mounted()` 只看 `s_mounted` 内存标志，从不碰卡 ② Status tab 没有 JS 自动刷新（只有 mqtt/wifi tab 才 startRefresh） | ① `tf_card_is_mounted()` 内部也调 `tf_card_probe()` 发 SPI CMD0 做真实硬件探测 ② Status tab 也调用 `startRefresh()`，JS 每秒 fetch `/status` API 更新 TF 状态、RSSI、温度、MQTT 状态 DOM 元素 ③ HTML 给 TF、RSSI、温度、MQTT 元素加了 id（`id='val-tf'` / `id='val-rssi'` / `id='val-temp2'` / `id='val-mqtt'` / `id='val-tf-space'`） |

**效果**：拔掉 TF 卡后 web 页 Status tab 在 **1-2 秒内**自动从"已挂载 (ok)"变"未挂载 (bad)"，剩余容量变"无"。插回卡后自动恢复。

### V1.7.0 Web Server 防重入 + heap 优化

| 项目 | 之前 | 之后 | 原因 |
|------|------|------|------|
| 启动时机 | WiFi 启动后调一次 + 主循环 WiFi 连上后又调一次 | **只在 app_main 里调一次**，主循环不再重复调 | 第二次调用时 WiFi STA 已连，heap 被 TCP/IP 协议栈吃了 ~14KB → `listen(112) ENOBUFS` |
| 防重入 | 无 | `start_webserver()` 入口 `if (s_server != NULL) return` | 欠压恢复时 `on_power_recovery()` 也调一次，和冷启动互斥安全 |
| stack_size | 8192 | **4096 → 回退 8192** | v1.7.0 砍到 4096 导致 `root_get_handler`（栈上 `vb[2048]`）栈溢出崩溃，主页面访问不到。captive portal handler 因代码简单未触发。V1.7.1 回归修复 |
| backlog_conn | 默认 5 | **2** | 同时省 heap |
| max_open_sockets | 默认 | **4** | 设备端不需要同时服务太多连接 |
| ctrl_port | 默认随机 | **32768** 固定端口 | 避免端口随机冲突 |

**根因分析日志**（修复前）：
```
I (876) HTTP: === start_webserver ===    ← 第一次 heap=151KB，WiFi AP 模式 → 成功 ✓
I (2436) WIFI: WiFi Connected!           ← STA 连上，heap 被 WiFi TCP/IP 吃了 ~14KB
I (5886) HTTP: === start_webserver ===   ← 主循环又调了一次！heap 只剩 137KB ✗
E (5886) httpd: httpd_server_init: error in listen (112)  ← ENOBUFS
```

### V1.7.1 Web Server 删除 MQTT Tab

| 项目 | 之前 | 之后 |
|------|------|------|
| 前端 Tab 按钮 | 状态 / WiFi设置 / **MQTT数据** | 状态 / WiFi设置（**只留 2 个 Tab**） |
| 前端 tab-mqtt div | 温度/LED/Field1/Field2/Set1/Set2 + 历史 RX 数据区 | **已删除** |
| JS refresh() mqtt 分支 | `fetch('/api/mqtt/data')` + 大量 DOM 更新 | **已删除** |
| `/api/mqtt/data` GET | `mqtt_data_handler`（返回 cJSON 温度/LED/字段/历史 RX） | **已删除** |
| `/api/mqtt/send` POST | `mqtt_send_handler`（url_decode → `mqtt_publish_aliyun_params`） | **已删除** |
| `url_decode()` 工具函数 | 仅被 mqtt_send_handler 用 | **已删除**（随 handler 一起） |
| max_uri_handlers | 16 | **14** |

### 状态页面颜色规则（web_server.c）

| 条件 | device_state |
|------|-------------|
| AP 活跃但未连 STA | warn / "配网中" |
| WiFi 断开 | bad / "已断开" |
| 正常 | ok / "在线" |

---

## 7. DNS Server

- **协议**：UDP 53
- **作用**：AP 模式下把所有域名解析到 `192.168.4.1`（即设备自身），实现 **Captive Portal**（手机连上热点自动跳转到配置页）
- **生命周期**：`dns_server_start()` 在 `do_wifi_start_once()` 和 `do_open_ap()` 里调用；`dns_server_stop()` 在 `do_close_ap()` 里调用

---

## 8. SNTP 时间同步

- **触发**：WiFi 连接成功后（main.c 里检测）
- **NTP 服务器**：默认 pool.ntp.org（代码实际用 ntp.aliyun.com）
- **MQTT 依赖**：MQTT 状态机 WAIT_TIME 状态等 SNTP 完成后才继续（TLS 需要有效时间戳）
- **SNTP 主时钟 / PCF8563 做备份**：SNTP 同步成功后回写 PCF8563，设备断电后靠 PCF8563 保持时间
- **sntp_is_synced()**：暴露 SNTP 是否已同步的查询接口，main.c 主循环用它触发 PCF8563 回写
- **★时区设置位置（V1.7.1 修复）**：`setenv("TZ", "CST-8")` + `tzset()` 从 `sntp_init_and_sync()` 移到 `app_main()` 最开头（NVS 之后、任何硬件 init 之前）。**根因**：之前时区在 sntp_init 里设，但 PCF8563 的 `mktime()` → `settimeofday()` 在 sntp_init 之前就执行了，导致系统时间在错误时区（UTC）下被写入，后续 `localtime_r()` 就会多出 8 小时，CSV 文件名跨天

---

## 8.1 PCF8563 外部 RTC（pcf8563.c）

### 硬件配置

| 项目 | 值 |
|------|-----|
| I2C 端口 | I2C_NUM_0 |
| SCL GPIO | **9** |
| SDA GPIO | **8** |
| I2C 地址 | 0x51 (7-bit) |
| 波特率 | 100kHz（标准模式） |
| 内部上拉 | 启用 |
| 芯片 | PCF8563（掉电不丢失，±10ppm ≈ 每月 ±2.5s） |

### 时钟架构（模式 A：PCF8563 做主，SNTP 校准）

```
设备上电
  │
  ├─ ★ 先设时区 CST-8（app_main 最开头，任何时间操作之前）
  │
  ├─ pcf8563_init()
  │     ├─ ★ 读 Control_status1(0x00) 检查 STOP 位（bit5）（V1.7.1 新增）
  │     │     └─ STOP=1 → 清除该位，重启振荡器
  │     ├─ 读 VL 位
  │     │     ├─ VL=0（时间有效）→ 读 PCF8563 时间 → settimeofday() 写入系统
  │     │     └─ VL=1（掉过电）→ 等待 SNTP 校准
  │
  ├─ WiFi 连上 → SNTP 同步
  │
  └─ main.c 主循环检测 sntp_is_synced()
        └─ 只触发一次 → pcf8563_set_time() 回写 PCF8563
           ★ set_time 内部：tm_year >= 100 时月份寄存器 bit7(Century) 设 1（V1.7.1 新增）
           ★ 写回后立即 read_time 读回验证，diff ≤ 2s 才算成功（V1.7.1 新增）
```

### 对外 API

```c
bool pcf8563_init(void);                        // 初始化 I2C + 检测芯片 + STOP位清除
bool pcf8563_read_time(struct tm *out);         // 读时间（BCD → tm）
bool pcf8563_set_time(const struct tm *in);     // 写时间（tm → BCD，含 Century bit）
bool pcf8563_is_running(void);                  // VL=0 表示未掉电，时间可信
```

### CSV 时间有效性

PCF8563 初始化成功后，系统 `time()` 立刻有效，`csv_time_valid()` 阈值判断自然通过。只有 PCF8563 不可用或 VL=1 时才退化为等 SNTP + 环形缓冲。

---

## 9. LED 驱动

### 9.1 硬件配置

| 项目 | 值 |
|------|-----|
| GPIO | **12** |
| 驱动方式 | **LEDC (PWM)** |
| PWM 频率 | 5 kHz |
| 分辨率 | 10 bit（占空比 0~1023） |
| 定时器 / 通道 | LEDC_TIMER_0 / LEDC_CHANNEL_0 |

### 9.2 模式

| 模式 | 说明 | 周期 / 时序 | PWM 占空比 |
|------|------|------------|-----------|
| LED_MODE_OFF | 灭 | - | 0 |
| LED_MODE_ON | 常亮（MQTT 连上 / OTA 成功收尾） | - | **500 (49%)** |
| LED_MODE_BLINK_SLOW | 慢闪 | 300ms | 1023 |
| LED_MODE_BLINK_FAST | 快闪 | 90ms | 1023 |
| LED_MODE_BLINK_IDLE | 超慢闪（节能态） | 2000ms | 1023 |
| **LED_MODE_BLINK_DOUBLE** | **OTA 下载中：双闪** | 亮100ms→灭60ms→亮100ms→灭600ms | 1023 |
| **LED_MODE_BLINK_GOOD** | **OTA 成功：3 次快闪→常亮** | 快闪 6 次×100ms→常亮 | 1023→500 |
| **LED_MODE_BLINK_BAD** | **OTA 失败：快闪** | 75ms | 1023 |
| **LED_MODE_BLINK_UNDERVOLT** | **欠压保护：极速闪** | **50ms** | 1023 |

### 9.3 状态优先级（resolve_status_mode）

```
MQTT 连上 ───────▶ LED_MODE_ON  （常亮，最高优先级）
    │
    ▼
STA 连上 ───────▶ LED_MODE_BLINK_SLOW  （300ms 慢闪）
    │
    ▼
AP 开启 ───────▶ LED_MODE_BLINK_FAST   （90ms 快闪）
    │
    ▼
IDLE 节能 ──────▶ LED_MODE_BLINK_IDLE  （2000ms 超慢闪）
    │
    ▼
都没有 ───────▶ LED_MODE_BLINK_SLOW  （兜底）

⚠️ OTA 专用模式（DOUBLE / GOOD / BAD）直接通过 led_status_mode_set() 强制覆盖，
   不走 resolve_status_mode 优先级链

⚠️ 欠压保护模式（UNDERVOLT）最高优先级锁死：
   resolve_status_mode() 入口处 if (s_status_mode == LED_MODE_BLINK_UNDERVOLT) return;
   欠压保护触发后，任何 WiFi/MQTT/OTA 状态变化都不能覆盖 LED 指示
   恢复由 on_power_recovery() 显式调 led_status_mode_set(BLINK_SLOW) 解除锁
```

### 9.4 WiFi/MQTT/OTA 状态联动（被调用方 → LED）

```c
// wifi_manager.c
led_notify_wifi_ap(true);      // do_wifi_start_once / do_open_ap
led_notify_wifi_ap(false);     // do_close_ap
led_notify_wifi_sta(true);     // GOT_IP
led_notify_wifi_sta(false);    // DISCONNECTED
led_notify_wifi_idle(true);    // ap_idle_timeout_cb（配网超时→节能态）
led_notify_wifi_idle(false);   // do_wifi_init / do_wifi_start_once / do_open_ap（恢复使用）

// mqtt_aliyun.c
led_notify_mqtt(true);         // MQTT CONNECTED
led_notify_mqtt(false);        // DISCONNECTED / ERROR

// ota_manager.c
led_notify_ota_start();        // 开始下载 → 强制 BLINK_DOUBLE
led_notify_ota_success();      // 下载成功 → 强制 BLINK_GOOD（3闪→常亮）
led_notify_ota_fail();         // 下载失败 → 强制 BLINK_BAD
```

### 9.5 RX/TX 通知脉冲（notify_blink_once）

收到 MQTT 消息或发送消息时，LED 闪烁 160ms 提示：

```
正常运行:  ┌─┐ ┌─┐ ┌─┐ ┌─┐         (300ms 周期)
           └─┘ └─┘ └─┘ └─┘

RX 触发: ┌─────┐ ┌───────────┐ ┌─┐ ┌─┐ ┌─┐
         │ ON  │ │   OFF     │ │ │ │ │
         │80ms │ │   80ms    │ │ │ │ │
         └─────┘ └───────────┘ └─┘ └─┘ └─┘
           └──── 160ms 通知 ───┘   └── 恢复正常闪烁
```

实现机制：
1. 保存当前模式 → `s_saved_mode`
2. `s_notify_active = true`
3. 停掉状态闪烁定时器 → 复用同一个 `s_status_timer` 执行通知脉冲
4. 160ms 后 `resume_status_mode()` 恢复

### 9.6 共用定时器设计

`s_status_timer` 同时承担两种职责：
- **正常模式**：`status_timer_cb` → 驱动状态闪烁（根据 `s_blink_step` 奇偶翻亮灭）
- **通知模式**：`s_notify_active=true` → 转发到 `notify_timer_cb` → 执行 160ms 脉冲

这样省下了创建第二个 esp_timer 的开销。

### 9.7 关键变量

```c
static led_mode_t s_status_mode;     // 当前 LED 模式
static led_mode_t s_saved_mode;      // 通知脉冲期间保存的模式
static bool s_notify_active;         // 是否正在执行 RX/TX 通知脉冲
static int s_notify_step;            // 通知脉冲步骤（0→ON, 1→OFF, 2→结束）
static int s_blink_step;             // 闪烁计数器（奇偶决定亮灭）
static int s_good_step;              // BLINK_GOOD 专用计数器（快闪 6 次后转常亮）

static bool s_ap_active;             // AP 状态输入
static bool s_sta_connected;         // STA 状态输入
static bool s_mqtt_connected;        // MQTT 状态输入
static bool s_idle_energy_save;      // 配网超时节能态（→ BLINK_IDLE 超慢闪）

static esp_timer_handle_t s_status_timer;  // 唯一定时器（闪烁 + 通知 + OTA 模式 复用）
```

---

## 10. Switch 驱动

| GPIO | 名称 | 说明 |
|------|------|------|
| **19** | **SWITCH1** | **继电器1（Switches bit0）** |
| **1** | **SWITCH2** | **继电器2（Switches bit1）** |
| **13** | **POWER** | **电源开关** |
| **18** | **LIGHT** | **独立灯控制（light 字段）** |

---

## 11. 温度传感器

- 使用 ESP32-C3 内置温度传感器（ADC 通道）
- `temp_sensor_init()` → 校准
- `temp_sensor_get()` → 返回 float 摄氏度

---

## 12. GPIO 分配总表

| GPIO | 方向 | 功能 | 组件 | ADC 通道 | 备注 |
|------|------|------|------|---------|------|
| **2** | **输入** | **电源电压 ADC** | **power_monitor** | **ADC1_CH2** | **150K/22K 分压，电池电压采样** |
| **3** | **输入** | **NTC1 ADC (Field1_data)** | **ntc_sensor** | **ADC1_CH3** | 热敏电阻分压 |
| **4** | **输入** | **NTC2 ADC (Field2_data)** | **ntc_sensor** | **ADC1_CH4** | |
| 5 | 输入 | TF MISO (DATA0) | tf_card | - | SPI2 MISO |
| 6 | 输出 | TF SCK (CLK) | tf_card | - | SPI2 SCK |
| 7 | 输出 | TF MOSI (CMD) | tf_card | - | SPI2 MOSI |
| **8** | **双向** | **PCF8563 I2C SDA** | **pcf8563** | - | **SPIWP，DIO 模式空闲，内部上拉** |
| **9** | **输出** | **PCF8563 I2C SCL** | **pcf8563** | - | **SPIHD/ROM PRINT，内部上拉** |
| 10 | 输出 | TF CS（片选） | tf_card | - | strapping，启动后可用 |
| **12** | **输出** | **状态 LED** | **led** | - | **PWM 输出** |
| **13** | **输出** | **POWER 电源开关** | **switch** | - | SPIWP，DIO 模式空闲 |
| **1** | **输出** | **SWITCH2 继电器2** | **switch** | - | USB Serial JTAG 已关闭，释放为普通 GPIO |
| 18 | 输出 | LIGHT 灯控制 | switch | - | 原 SWITCH2，现为独立灯 |
| **19** | **输出** | **SWITCH1 继电器1** | **switch** | - | |
| 14 | - | SPI Flash CS0 | - | - | ⚠️ 绝对不可用作 GPIO |
| - | - | WiFi | WiFi 协议栈 | - | |
| - | - | 内部温度传感器 | temp_sensor | ADC 内部 | |

### ESP32-C3 Flash 引脚保护（重要！）

ESP32-C3 以下引脚与 SPI Flash 共用，**绝对不能配成 GPIO 输出**：

| GPIO | Flash 功能 | 当前 Flash 模式 | GPIO 可用？ |
|------|-----------|----------------|------------|
| **14** | **SPICS0 (Flash CS)** | **始终使用** | **❌ 绝对不可！** 配输出会切断 Flash 通信 → crash |
| 8 | SPIWP | DIO 模式不使用 | ✅ 可用（PCF8563 SDA） |
| 9 | SPIHD / ROM PRINT | DIO 模式不使用 | ✅ 可用（PCF8563 SCL） |
| 12 | SPIHD | DIO 模式不使用 | ✅ 可用（SWITCH1） |
| 13 | SPIWP | DIO 模式不使用 | ✅ 可用（POWER） |
| 15 | SPID | Flash D0 | ❌ 严禁 |
| 16/17 | SPICLK | Flash CLK+/- | ❌ 严禁 |

### GPIO 冲突解决历史

1. **SWITCH0** 原分配 GPIO 5 → TF 卡 DATA0 占用 → 移至 **GPIO 18 (SWITCH2)**
2. **POWER** 原分配 GPIO 4 → NTC2 ADC 占用 → 移至 **GPIO 13**
3. TF 卡 CS 从 GPIO11 → **GPIO10**（用户指定不可改 TF 卡引脚）
4. SPI2 总线专用，不与其他外设共享
5. **V1.5.1 互换**：LED 从 GPIO12 → **GPIO1**；SWITCH1 从 GPIO1 → **GPIO12**
6. **GPIO1 是 strapping pin**，用作 PWM 输出，启动后可用
7. **V1.5.1 新增** PCF8563 RTC：I2C0 SCL=GPIO9, SDA=GPIO8（均为 DIO 空闲脚）
8. **V1.7.0 GPIO 重分配**：LED 从 GPIO1 → **GPIO12**；SWITCH1 从 GPIO12 → **GPIO19**；电源监测从 GPIO19 下降沿中断 → **GPIO2 ADC 电压采样**（ESP32-C3 ADC1 不与 WiFi 冲突）
9. **V1.7.0 SWITCH2 迁移**：曾临时从 GPIO18 → GPIO1 → 撤回恢复 **GPIO18**
10. **V1.7.1 SWITCH2 最终迁移**：SWITCH2 从 GPIO18 → **GPIO1**（关闭 `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG` 释放 GPIO1）；**LIGHT 独立**：原 `light` 字段与 SWITCH2 共用，现拆为独立 GPIO18 (`LIGHT_GPIO`)，新增 `light_set/get` 接口；MQTT 上报 `pub_light` 改读 `light_get()`；web_server `/led` 接口改调 `light_set/get()`
11. **V1.7.1 Field1_data / Field2_data 改 float**：原 `int s_field1_data/s_field2_data` 强转 `(int)ntc1/ntc2` 截断温度小数 → 改 `float`，去掉 `(int)` 强转，上报格式 `%d` → `%.2f`，ACK 解析 `valueint` → `valuedouble`，getter 返回类型 `int` → `float`。mqtt_manager_task 栈从 4096 → 8192（7 个 %.2f 浮点格式化吃掉太多栈）

### strapping 引脚说明

| GPIO | strapping 功能 | 启动后可用？ |
|------|---------------|------------|
| 0 | 启动模式（下载/Flash） | ✅ 启动后可用 |
| 1 | JTAG 信号源选择 | ✅ **V1.7.1 关闭 USB Serial JTAG 后可用**（SWITCH2 GPIO） |
| 3 | JTAG 选择 | ✅ 启动后可用 |
| 4 | VDD_SPI 电压 | ❌ 不可改 |
| 8 | ROM 消息打印 | ✅ 启动后可用（PCF8563 SDA） |
| 10 | JTAG 信号源选择 | ❌ JTAG 引脚，跟随 GPIO1 |

---

## 13. TF 卡驱动（tf_card.c）

### 13.1 硬件配置

| 项目 | 值 |
|------|-----|
| 总线 | **SPI2_HOST**（独占，不共享） |
| CS | **GPIO 10** |
| SCK | **GPIO 6** |
| MOSI (CMD) | **GPIO 7** |
| MISO (DATA0) | **GPIO 5** |
| 挂载点 | `/tf` |
| 文件系统 | FAT32（电脑可直接读写） |
| 分区大小 | 16KB allocation unit |

### 13.2 SPI 模式原因

ESP32-C3 无 SDMMC 硬件，必须用 SPI 模式（`esp_vfs_fat_sdspi_mount`）。

### 13.3 追加缓存机制（磨损保护 + 掉电安全）

TF 卡擦写寿命有限（SLC ~10 万次，MLC ~1 万次）。引入 **2KB 追加缓存** + **递归互斥锁** + **5分钟定时 flush timer** + **CSV 独立直写** 的混合策略：

| 项目 | 值 |
|------|-----|
| 缓存大小 | 2048 字节 |
| 缓存路径 | 只缓存单个文件路径（`s_append_cache_path`） |
| 触发 flush | 缓存满 / 切换文件路径 / 手动调用 / MQTT 断开 / deinit / **5分钟定时器** |
| 数据丢失窗口 | cache 路径：最多 5 分钟（定时 flush 保底）；CSV 路径：**最多 30 秒**（每次直接 fopen+fwrite+fclose） |
| ★ 互斥锁（V1.7.1） | 所有公共函数用 `xSemaphoreCreateRecursiveMutex` 保护，防止 MQTT task 和 esp_timer（欠压回调）并发访问 |
| ★ CSV 直写（V1.7.1） | `tf_card_append_csv()` **不走** cache，每次直接 fopen("a") + fwrite + fclose，掉电最多丢 1 行 |
| ★ 去掉 fsync（V1.7.1） | fclose 已经触发 FatFs 目录更新，额外 fsync 每次省几十 ms |

```
cache 路径（Web 端写历史数据等）:
  append_file() → 进 2KB cache
       ↓
  触发 flush（满/5min timer/手动）→ fopen + fwrite + fclose

CSV 路径（MQTT 上报后写温度）:
  append_csv() → ★ 直接 fopen + fwrite + fclose（不走 cache）
       ↓
  每次上报（30s）立即落盘，掉电最多丢 1 行
```

### 13.4 TF 卡自愈机制（v1.6.5 新增）

开机只 init 一次，运行中卡被拔掉/接触不良会导致所有 `tf_card_*` 函数静默失败。v1.6.5 新增**自愈机制**，与 MQTT 上报完全同步（MQTT task 每 30s 组报文前先巡检），连续 2 次失败即触发彻底重启。

| 项目 | 值 |
|------|-----|
| 巡检时机 | **MQTT 组报文之前**（与业务写卡同 task、同周期，完全同步） |
| 失败门槛 | 连续 **2 次**（SPI 毛刺防抖） |
| 彻底重启 | flush 缓存 → unmount → spi_bus_free → 重新 full init |
| 未 mounted 时 | 每次巡检都直接尝试 `tf_card_init()`（插卡自动恢复） |
| 自愈守卫 | `s_reinit_in_progress` 互斥标志防止 reinit 递归死循环 |

**MQTT task 30s 周期流程**：

```
MQTT_STATE_CONNECTED 每 30s:
  1. tf_card_periodic_check()   ← 先查卡，掉了就 reinit
  2. s_is_reconnect?            ← 重连跳过首次上报
     → YES: 清标志 + delay 30s + break
     → NO:  继续正常流程
  3. s_pub_pause_until_ms?      ← 历史查询暂停期间跳过
     → YES: break（不报也不写卡）
     → NO:  继续
  4. 组 JSON 报文（读温度/RSSI/开关/NTC/TF_state）
  5. mqtt_publish()
  6. publish 成功 + TF 卡 mounted → tf_card_append_csv()
```

**自愈计数逻辑**（v1.7.1 分离为两个独立计数器，避免 probe 成功冲掉 I/O fail 记录）：

```
之前（BUG：共享 s_check_fail_count）：
  probe OK → count 清零 → I/O fail 记录被冲掉！
  I/O fail → count = 1
  probe OK → count 清零 → 永远在 1 徘徊，永远到不了 2，永远不会 reinit！

现在（正确：分离 s_probe_fail_count / s_io_fail_count）：

巡检路径（每 30s，MQTT task 驱动）：
  tf_card_periodic_check() → tf_card_probe()（直连 SPI 发 CMD0 GO_IDLE_STATE）
    → probe 成功 → s_probe_fail_count = 0, s_cached_state = 1（不影响 s_io_fail_count！）
    → probe 失败 → s_probe_fail_count++, s_cached_state = 0
      → s_probe_fail_count >= 2 || s_io_fail_count >= 2 → tf_card_reinit()

写路径失败（业务驱动，辅助）：
  业务写卡 fopen/fwrite 失败 → tf_card_notify_io_fail()
    → s_io_fail_count++（独立累加，probe 成功不会清零它！）
    → s_probe_fail_count >= 2 || s_io_fail_count >= 2 → tf_card_reinit()
    → 写卡成功 → s_io_fail_count = 0
```

**巡检状态机**（v1.7.1 更新：分离计数器 + s_cached_state 更新）：

```
tf_card_periodic_check()  （由 MQTT task 每 30s 调一次，与写卡同 task）
  │
  ├─ s_mounted == true:
  │     ├─ tf_card_probe() → SPI2 临时挂载: GPIO10 手动控 CS → 发 CMD0 → 读 R1
  │     │     ├─ 成功 → s_probe_fail_count = 0
  │     │     │         s_cached_state = 1（状态变化时打 W 日志）
  │     │     │         不影响 s_io_fail_count ✅（分离计数器，这是关键修复）
  │     │     └─ 失败 → s_probe_fail_count++, s_cached_state = 0
  │     │           ├─ count == 1 → ESP_LOGW("probe fail (1/2)")（防抖）
  │     │           └─ s_probe_fail_count >= 2 || s_io_fail_count >= 2 → tf_card_reinit()：
  │     │                 1. s_reinit_in_progress = true（防重入）
  │     │                 2. append_cache_flush()  ← 先把 CSV 缓存刷盘
  │     │                 3. esp_vfs_fat_sdcard_unmount() + spi_bus_free()
  │     │                 4. tf_card_init() 重新 full init（bus_init + mount）
  │     │                 5. 成功 → 两个计数器都清零 + s_cached_state = 1 → "re-mounted OK"
  │     │                    失败 → s_cached_state = 0 → "re-init failed"
  │     │                 6. s_reinit_in_progress = false
  │
  └─ s_mounted == false（开机没插 / 之前 reinit 失败）:
        s_cached_state = 0
        └─ 直接调 tf_card_init()  ← 插卡自动恢复
```

**写路径失败点**（均调用 `tf_card_notify_io_fail()`，辅助自愈）：

| 函数 | 失败点 | 成功时 |
|---|---|---|
| `append_cache_flush` | fopen 失败 + fwrite 不完整 | s_io_fail_count = 0 |
| `tf_card_write_file` | fopen 失败 + fwrite 不完整 | s_io_fail_count = 0 |
| `tf_card_append_csv` | fopen 失败 + fwrite 不完整 + ENOSPC 重试 | s_io_fail_count = 0 |

**probe 方式选择**（v1.6.5-2 最终方案）：
- **最终方案：直连 SPI 发 CMD0 (GO_IDLE_STATE)** — 绕过 VFS 层缓存，直接操作 SPI2_HOST 硬件。手动控制 GPIO10 (CS) 拉低 → 发 CMD0 → 读 R1 响应。物理拔掉卡后 MISO 浮空读到全 0xFF，必定失败，**零缓存、零延迟**
- 放弃 `opendir("/tf")`：只查 VFS 挂载链表（内存状态），卡拔掉后记录还在 → 假阳性
- 放弃 `stat("/tf")`：比 opendir 好但仍走 FatFs 内部缓存路径，且需要 `#include <sys/stat.h>`
- 放弃 `sdmmc_card_read_csd()`：需要 `sdmmc_cmd.h` 且依赖已挂载的 sdspi host，probe 在异常时 host 可能已无效
- v1.7.1 修复：**业务写卡成功清零 s_io_fail_count**（probe 成功清零 s_probe_fail_count），两个计数器独立运作，互不干扰。解决了卡接触不良时 probe 能过但文件操作失败 → probe 成功把 I/O fail 记录冲掉 → 永远到不了 2 → 永远不会 reinit 的 BUG

**安全性**：
- SPI CMD0 是纯只读命令，**对 TF 卡零磨损**
- 彻底重启（unmount + bus_free + reinit）同样无额外写操作
- 先 flush 再 deinit → 断电/掉卡不丢 CSV 数据

**日志策略**（v1.7.1 优化：减少正常巡检日志，状态变化才打印）：

| 场景 | 级别 |
|---|---|
| probe 成功 + 状态未变 | **不打印**（静默，避免每 30s 刷一条 "TF card OK"） |
| probe 成功 + 状态变化（0→1） | `W` "TF state changed: 0 -> 1 (probe OK)" |
| probe 失败 / I/O 失败 | `W` "probe fail (x/2)" / "I/O fail (x/2)" |
| reinit 成功 | `I` "TF card re-mounted OK" + "两个计数器清零" |
| reinit 失败 | `W` |
| fopen / fwrite / mount 错误 | `E` |
| init / deinit / mount 成功 / 正常写读 | `D`（Debug，默认静默） |
| sdmmc_card_print_info | **已删除**（那一大串 Name/Type/Size 太吵） |
| **v1.7.1 新增 ENOSPC** | `W` "Low space: xxx KB (< 1024 KB), triggering cleanup..." |
| **v1.7.1 新增 清理** | `I` "Deleted oldest log: 2026-09-10.csv (free now 5120 KB)" |

### 13.5 对外 API

```c
// 生命周期
bool tf_card_init(void);
void tf_card_deinit(void);
bool tf_card_is_mounted(void);   // v1.6.5-2: 内部调用 tf_card_probe() 做真实硬件检测，不再只看 s_mounted
int  tf_card_get_state(void);    // v1.6.5-2 新增, v1.7.1 优化: 返回 s_cached_state 缓存值（0/1），缓存由 periodic_check 更新，不再每次实时 probe

// ★ v1.6.5 自愈
#define TF_CHECK_INTERVAL_MS   30000
void tf_card_periodic_check(void);     // MQTT task 每 30s 组报文前调一次

// 文件操作
bool tf_card_read_file(path, buf, buf_size, bytes_read);
bool tf_card_write_file(path, data, data_len);     // 覆盖写
bool tf_card_append_file(path, data, data_len);    // 追加（带缓存）
bool tf_card_flush(void);                          // 强制 flush 缓存
bool tf_card_list_dir(dir_path);

// 磁盘信息
bool tf_card_get_space(total_bytes, free_bytes);
```

### 13.6 CSV 记录（按天分文件）

- **存储路径**：`/tf/logs/YYYY-MM-DD.csv`（每天自动创建新文件）
- **一行格式**：`2026-09-19,14:30:00,25.30,0.00`（date,time,Field1_data,Field2_data）
- **写入接口**：`tf_card_append_csv(ntc1, ntc2)` — 内部自动处理日期切换
- **查询接口**：`tf_card_read_csv_by_date("2026-09-19", buf, size)` — 返回当天行数，手机端 HTTP 可直接调用
- **SNTP 未同步**：环形缓冲 s_csv_pending[8] 暂存（最多 4 分钟），时间有效后自动刷入正确日期文件
- **旧日志**：不自动清理，写满 TF 卡为止
- **电脑打开**：FAT32 + 纯 CSV，Excel/WPS/记事本直接可用

### 13.7 Mock 数据自动生成（首次开机）

TF 卡 mount 后自动检测 `/tf/mock_done` 标记文件，不存在则在 `/tf/logs/` 下生成当天日期的 mock CSV（50 行），方便手机端查询测试。生成后写入 marker 文件，后续开机不再重复。

---

## 13.8 历史数据查询 v2（history_query.c）

> 版本：V2.0 | 核心变化：按天聚合 + QueryDate 标记 + 单日独立分页（替代旧的全局 RangeStartDate/RangeEndDate + 全局分页）

### 13.8.1 设计动机（为什么 V2 要重写）

| 旧实现（v1） | 问题 |
|-------------|------|
| 全局预扫一遍算总数（Phase 1） | TF 卡每个 CSV 被读两次（Phase 1 扫 + Phase 2 再扫），I/O 翻倍 |
| QUERY_DATA 用 `RangeStartDate/RangeEndDate` 全局标记 | APP 端还得从每条 record 的 `date` 字段推断属于哪天 |
| Page/TotalPages 是全局统一分页 | 某天数据特别多（如一天 500 条）也被混在全局 3000 条的分页里，APP 分组难 |
| `json_buf[20000]` 在 4KB 栈上 | **栈溢出 → 设备重启**（ESP32-C3 任务栈默认 4KB，json_buf 就 20KB，一进门就踩穿栈底 CANARY） |

V2 设计原则：**TF 卡 I/O 最少 + 协议最清晰 + 栈安全**。

### 13.8.2 入参格式（兼容两种写法）

**格式 A：范围查询（推荐，新格式）**

```json
{"DeviceID":"26001_V1.7.0","Dir":"C>D","Cmd":"QUERY","StartDate":"2026-09-15","EndDate":"2026-09-21","StartTime":"00:00:00","EndTime":"23:59:59"}
```

**格式 B：单日查询（旧格式仍兼容）**

```json
{"DeviceID":"26001_V1.7.0","Dir":"C>D","Cmd":"QUERY","QueryDate":"2026-09-21"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `StartDate` / `EndDate` | string | 条件必填 | 范围两端，格式 `YYYY-MM-DD`，与 QueryDate 互斥 |
| `StartTime` / `EndTime` | string | 否 | 默认 `"00:00:00"` / `"23:59:59"`，格式 `HH:MM:SS` |
| `QueryDate` | string | 条件必填 | 单日快捷写法，固件内部自动映射为 StartDate=EndDate=QueryDate |

### 13.8.3 上行回复协议（关键变化）

**QUERY_DATA（每页最多 300 条，按天独立分页）**

```json
{"Dir":"D>C","Cmd":"QUERY_DATA","QueryDate":"2026-09-20","Page":0,"TotalPages":2,"Records":[
  {"date":"2026-09-20","t":"08:00:00","f1":23.50,"f2":24.10},
  {"date":"2026-09-20","t":"08:30:00","f1":23.60,"f2":24.20}
]}
```

| 字段 | 说明 | 与 v1 对比 |
|------|------|-----------|
| **`QueryDate`** | **本页数据对应的日期（YYYY-MM-DD）** | ⭐ 新增，替代 v1 的 `RangeStartDate`/`RangeEndDate` |
| **`Page`** | **该天内部页码，0 起始** | ⭐ 从"全局页码"改为"该天内部分页" |
| **`TotalPages`** | **该天内部总页数（精准）** | ⭐ 从"全局总页数"改为"该天内部总页数" |
| `Records[]` | 本页数据，最多 300 条 | 不变 |

**QUERY_END（简化）**

```json
{"Dir":"D>C","Cmd":"QUERY_END","RecordCount":2130}
```

| 字段 | 说明 |
|------|------|
| `RecordCount` | 所有天累加的总条数 |

**QUERY_ERROR**

```json
{"Dir":"D>C","Cmd":"QUERY_ERROR","Reason":"RANGE_TOO_LARGE"}
```

Reason 枚举：`INVALID_DATE` / `INVALID_TIME` / `INVALID_RANGE` / `RANGE_TOO_LARGE` / `INVALID_FIELD` / `NO_MEM`

**QUERY_CANCEL**

```json
{"Dir":"D>C","Cmd":"QUERY_CANCEL"}
```

取消后固件**不发 QUERY_END**，APP 端自行超时退出。

### 13.8.4 两阶段执行流程（Phase 1 逐日计数 + Phase 2 逐日发送）

```
history_query_handle()
  ├─ JSON 解析 → query_params_t（start_date / end_date / start_time / end_time）
  ├─ 校验：范围 ≤ 7 天、日期格式、时间逻辑
  ├─ malloc 一份 params（传给子任务）
  └─ xTaskCreate(history_query_task, "query", 24576, ...)   ← 栈 24KB（关键！v1 只有 4KB 导致栈溢出）

history_query_task()
  ├─ mqtt_pause_report_ms(30000)          ← 暂停 30s 主动上报，查询优先
  ├─ s_query_cancelled = false

  ├─ ===== Phase 1: 逐日计数 =====
  │     for day_offset = 0 .. total_days-1:
  │       cur_date = start_date + day_offset
  │       per_day_count[day_offset] = count_day_records(cur_date, ...)
  │         └─ fopen /tf/logs/{cur_date}.csv
  │         └─ 逐行 parse_csv_line + record_time_in_range → count++
  │         └─ fclose
  │       （取消检查点：每日期循环入口）
  │
  │     → 此阶段后每个 CSV 文件只读一遍，得到精准的 per_day_count[]

  ├─ ===== Phase 2: 逐日 rewind 发送 =====
  │     for day_offset = 0 .. total_days-1:
  │       if per_day_count[day_offset] == 0: continue
  │       day_total_pages = ceil(per_day_count / 300)
  │       send_day_data(cur_date, day_total_pages, ...)
  │         └─ fopen /tf/logs/{cur_date}.csv  ← 第二次打开（Phase 1 已关闭）
  │         └─ 逐行 parse_csv_line + record_time_in_range
  │         └─ 累满 300 条 → 发 QUERY_DATA（带 QueryDate + 该天 Page + 该天 TotalPages）
  │         └─ fclose
  │       （取消检查点：每页发送后 + 每行循环入口）
  │
  │     → 每个 CSV 文件第二次打开是 rewind 回文件头，SPI 开销极小

  └─ 取消 ?
       ├─ YES → 跳过 QUERY_END → pause(0) → vTaskDelete
       └─ NO  → send_query_end(total_sent) → pause(0) → vTaskDelete
```

**为什么是"两阶段 + 每文件两次打开"而不是"全局预扫 + 全局重扫"？**

```
                    v1（旧）                        v2（新）
  ┌─────────────────────────────────────────┐  ┌─────────────────────────────────┐
  │ 全局 Phase 1: 全部天扫一遍 → 总条数    │  │ Phase 1: 每天各扫一遍 → 每天条数 │
  │ 全局 Phase 2: 全部天再扫一遍 → 发数据  │  │ Phase 2: 每天 rewind → 发该天数据 │
  │                                         │  │                                  │
  │ CSV 读取次数 = total_days × 2           │  │ CSV 读取次数 = total_days × 2    │
  │ 区别：Phase 1 和 Phase 2 各自独立开文件  │  │ 区别：Phase 1 已结束再开 Phase 2    │
  │ （旧实现也是每文件两次 fopen，不是真全局） │  │ （和 v1 读取次数一样，但分页粒度变细了）│
  └─────────────────────────────────────────┘  └─────────────────────────────────┘

  结论：每文件两次 fopen 是"准 TotalPages"必须付出的代价。
       如果接受不准 TotalPages，可以真只读一遍（边扫边发，TotalPages 传 0，等 QUERY_END 再知道）。
       当前选择：准 TotalPages + 每文件两次打开（SPI rewind 开销极小）。
```

### 13.8.5 单日分页示例

```
请求：{"Cmd":"QUERY","StartDate":"2026-09-19","EndDate":"2026-09-21"}

设备侧 CSV 数据：
  /tf/logs/2026-09-19.csv → 120 条符合条件
  /tf/logs/2026-09-20.csv → 450 条符合条件
  /tf/logs/2026-09-21.csv → 文件不存在 → 跳过

上行回复时序：
  QUERY_DATA QueryDate=2026-09-19 Page=0 TotalPages=1  (120 条)
  QUERY_DATA QueryDate=2026-09-20 Page=0 TotalPages=2  (300 条)
  QUERY_DATA QueryDate=2026-09-20 Page=1 TotalPages=2  (150 条)
  QUERY_END RecordCount=570
```

APP 端按 QueryDate 自动分组建 Tab，每天独立翻页。

### 13.8.6 关键函数

| 函数 | 作用 |
|------|------|
| `history_query_handle(payload, len)` | 入口：JSON 解析 + 校验 + 创建任务 |
| `count_day_records(query_date, ...)` | **Phase 1**：单日纯计数（CSV 只读一遍） |
| `send_day_data(query_date, day_total_pages, ...)` | **Phase 2**：单日分页发送（fopen → 组 JSON → 按 300 条拆页 → fclose） |
| `history_query_task(arg)` | 主任务：Phase 1 → Phase 2 → QUERY_END |
| `record_time_in_range()` | 判断单条记录是否落在全局 [start_date+start_sec, end_date+end_sec] 范围内 |
| `parse_csv_line()` | 从 CSV 行提取 date/time/f1/f2 |

### 13.8.7 关键常量与变量

```c
#define QUERY_PAGE_SIZE     300
#define QUERY_JSON_BUF_SIZE 20000
#define QUERY_MAX_DAYS       7

static volatile bool s_query_cancelled;    // QUERY_CANCEL 跨任务标志

// 任务创建栈大小：24576 = json_buf[20000] + 局部变量余量 4KB
xTaskCreate(history_query_task, "query", 24576, dup, 5, NULL);
```

### 13.8.8 取消机制

```
App 任意时刻发 QUERY_CANCEL → s_query_cancelled = true
  │
  ├─ cancel 检查点 ①：history_query_task() 的 Phase 1 / Phase 2 外层循环入口
  ├─ cancel 检查点 ②：count_day_records() 内部（Phase 1 单日扫描入口）
  ├─ cancel 检查点 ③：send_day_data() 的 fgets 循环入口（Phase 2 每行解析前）
  └─ cancel 检查点 ④：每页发送后

  取消后跳过 QUERY_END 发送 → pause(0) → vTaskDelete
```

### 13.8.9 错误处理

| 场景 | 行为 |
|------|------|
| 日期范围超过 7 天 | QUERY_ERROR("RANGE_TOO_LARGE") → return |
| EndDate < StartDate | QUERY_ERROR("INVALID_RANGE") → return |
| 日期/时间格式非法 | QUERY_ERROR("INVALID_DATE") / QUERY_ERROR("INVALID_TIME") → return |
| 入参缺少 QueryDate 和 StartDate/EndDate | QUERY_ERROR("INVALID_FIELD") → return |
| ~~单日查询 EndTime < StartTime~~ | ~~QUERY_ERROR("INVALID_TIME")~~ → **已移除**（V1.7.0 修复：`start_sec > end_sec` 在跨天查询时合法，单日查询如 StartTime=23:59:59, EndTime=00:00:00 也合法，表示跨午夜） |
| CSV 文件 ENOENT | Phase 1 跳过计数，Phase 2 跳过发送（**不报错**，无数据很正常） |
| CSV 文件其他 I/O 错误 | tf_card_reinit() → 重试一次 → 仍失败则跳过该日期 |
| malloc / xTaskCreate 失败 | QUERY_ERROR("NO_MEM") → return |

**注意**：`ENOENT`（无对应日期的 CSV 文件）在 v2 里**不再发错误**，因为正常查询跨越多天时，某天没有数据是常态（设备那天没启动 / TF 卡那天没插），应该静默跳过。

### 13.8.10 任务栈安全（v1 踩过的坑）

```
v1 bug：send_day_data() 里 char json_buf[20000] 在栈上
        任务创建时栈只给了 4096
        函数调用 → SP - 20000 → 踩穿 CANARY → 设备重启

v2 修复：xTaskCreate(..., 24576, ...) 栈大小从 4KB → 24KB
        20KB json_buf + 2KB 局部 + 2KB FreeRTOS 上下文 = 24KB 刚好
        任务是短生命周期（发完数据立即 vTaskDelete），RAM 临时占用，不影响常态
```

### 13.8.11 对外 API

```c
void history_query_handle(const char *payload, int len);   // 被 mqtt_aliyun.c RX 回调调用
```

MQTT 侧调用点（mqtt_aliyun.c 的 MQTT_EVENT_DATA 回调，在 /user/get 主题）：
```c
history_query_handle(data_buf, dlen);   // 所有下行消息都过一遍，非 QUERY/QUERY_CANCEL 自动忽略
```

### 13.8.12 v1 → v2 协议变更总结

| 项目 | v1（旧） | v2（新） |
|------|---------|---------|
| QUERY_DATA 日期标识 | `RangeStartDate` + `RangeEndDate`（全局范围） | **`QueryDate`（单日标记）** |
| Page / TotalPages | 全局统一分页 | **该天内部独立分页** |
| QUERY_END 字段 | 带 RangeStartDate / RangeEndDate | **只带 RecordCount**（简化） |
| CSV ENOENT | 发 QUERY_ERROR（错） | **静默跳过**（无数据是常态） |
| 任务栈 | 4KB（**踩穿栈 → 设备重启**） | **24KB（安全）** |
| 入参格式 | 只支持 QueryDate（单日） | **同时兼容 QueryDate 和 StartDate/EndDate + StartTime/EndTime** |

---

## 14. NTC 热敏电阻（ntc_sensor.c）

### 14.1 硬件配置

| 项目 | 值 |
|------|-----|
| NTC1 ADC GPIO | **3**（Field1_data） |
| NTC2 ADC GPIO | **4**（Field2_data） |
| ADC unit | **ADC1**（WiFi 无冲突） |
| 衰减 | ADC_ATTEN_DB_12 |
| 采样 | 10 次平均（前加 1 次丢弃采样 + 500us 延迟，清通道切换残留） |
| 运放缓冲 | 每通道独立运放（电压跟随器，高阻输入 → 低阻输出） |

### 14.2 NTC 参数（ntc_sensor.h）

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 分压电阻（上拉固定） | **47 kΩ** | V1.7.3 从 10kΩ 改为 47kΩ，避免 DB12 高电压非线性 |
| NTC 标称电阻 R0 | **10700 Ω** @ 25℃ | V1.7.3 实测校准（原 10615Ω） |
| B 值 | **3415** | V1.7.3 实测校准（原 3950） |
| 参考电压 | **3300 mV** | |
| 故障检测 | **已移除** | V1.7.3 去掉开路/短路阈值，公式算多少显示多少 |

### 14.3 ADC 校准架构（★ V1.7.2 重写）

**每个通道独立 curve_fitting 校准**，不复用其他通道句柄。

```
ntc_sensor_init():
  s_cali1 = curve_fitting(ADC1_CH3, NTC1)  ✅ 独立校准
  s_cali2 = curve_fitting(ADC1_CH4, NTC2)  ✅ 独立校准

power_monitor_init():
  s_cali_handle = curve_fitting(ADC1_CH2, 电源)  ✅ 独立校准（自己创建 unit）
```

**Sanity check 防坏 efuse**：部分芯片 CH3 的 curve_fitting efuse 数据在满量程（raw=4095）时严重偏离（如给 2855mV 而实际应 ~3300mV）。在正常工作范围（raw 100~4000）内，对比 curve_fitting 结果与 fallback `raw×NTC_VOLTAGE_MV/4095`，偏差 >30% 则标记 bad，后续自动降级用 fallback。**开路/短路时跳过 sanity check**（满量程值不应用来判断校准好坏）。

```
raw_to_voltage_safe(chan, raw):
  cali_mv = curve_fitting(raw)
  fallback_mv = raw * NTC_VOLTAGE_MV / 4095    ← V1.7.3 从硬编码 4400 改为 NTC_VOLTAGE_MV

  if raw in [100, 4000]:        ← 只在正常范围检查
    diff = abs(cali_mv - fallback_mv)
    if diff > fallback_mv / 3:  ← >30% 偏差
      s_cali_bad = true         ← 永久降级
      return fallback_mv

  return cali_mv
```

**实测精度**（47kΩ 电阻后）：CH3 943mV vs 万用表 946mV（0.3%），CH4 1751mV vs 万用表 1708mV（2.5%）。

### 14.4 故障检测

**V1.7.3 已移除开路/短路阈值检测**，公式计算多少就显示多少。v_ratio 温和钳位到 [0.001, 0.999] 仅防止除零，NaN/Inf 兜底返回 -40.0°C。

| 项目 | 说明 |
|------|------|
| 开路检测 | ❌ 已移除（raw ≥ 4000 不再判定开路） |
| 短路检测 | ❌ 已移除（raw ≤ 10 不再判定短路） |
| NaN/Inf | ✅ 保留（兜底返回 -40.0°C） |

### 14.5 计算原理

```
VCC(3.3V) ── R_fixed(47K) ──┬── NTC ── GND    ← V1.7.3 电阻改为 47kΩ
                             │
                          GPIOx (ADC)    ← DB12 衰减，有效线性范围 0~2500mV

raw_to_voltage:
  normal → curve_fitting 校准 → voltage_mv
  bad efuse → fallback raw×NTC_VOLTAGE_MV/4095 → voltage_mv

v_ratio = voltage_mv / 3300
R_ntc = 47000 * v_ratio / (1 - v_ratio)        ← V1.7.3 改为 47000
1/T = 1/298.15 + (1/3415) * ln(R_ntc / 10700)  ← V1.7.3 改为 B=3415, R0=10700
T_celsius = (1/T) - 273.15
```

**硬件改 47kΩ 的原因**：ESP32-C3 ADC 用 `ADC_ATTEN_DB_12` 时，有效线性范围只有 **0~2500mV**。10kΩ 上拉 + NTC 在低温（<-15°C）时分压点超过 2800mV，进入非线性区，curve_fitting 校准也救不了。改 47kΩ 后整个量程（-30°C ~ 100°C）分压都在 0~2200mV，完全落在 ADC 线性区。

### 14.6 ADC 通道切换残留清除（★ V1.7.2 新增）

ESP32 ADC 内部 ~4pF 采样保持电容在通道切换后有上一通道的残留。CH3 开路（3.3V 满量程）采样完切到 CH4 时，残留高压会让 CH4 读数偏高，算出假低温。

**修复**（calc_temp 开头）：
```c
adc_oneshot_read(s_adc_unit, chan, &raw);  // 丢弃第一次采样（清除残留）
esp_rom_delay_us(500);                     // 500us 让保持电容充分稳定
// 然后再采 10 次平均
```

### 14.7 对外 API

```c
esp_err_t ntc_sensor_init(void);
float ntc_sensor_get1(void);     // Field1_data (℃)
float ntc_sensor_get2(void);     // Field2_data (℃)
void ntc_sensor_get_raw(int *raw1, int *raw2, int *mv1, int *mv2);  // 调试用
bool ntc_sensor_is_calibrated(void);  // 两个通道校准都 OK 才返回 true
```

---

## 15. 电源监测（power_monitor.c）— 欠压保护状态机

### 15.1 方案选择：为什么用 ADC 而不是比较器中断？

| 方案 | 优点 | 缺点 |
|------|------|------|
| ❌ GPIO 下降沿中断（旧方案） | 硬件简单 | 阈值固定（比较器分压定死），无法区分"轻微欠压"和"严重掉电"，只能开关式触发 |
| ✅ **ADC 采样（当前方案）** | **实时读电压值，可设多级阈值，支持滞回防抖，能上报精确电压到 MQTT Field1** | 多占一个 ADC 通道（但 GPIO2 ADC1 不与 WiFi 冲突） |

### 15.2 硬件设计（12V → 3.3V ADC）

| 项目 | 值 |
|------|-----|
| 监测 GPIO | **2**（ADC1_CH2，WiFi 无冲突） |
| 分压电阻 R5 | **150 kΩ**（上拉，接电源正极） |
| 分压电阻 R6 | **22 kΩ**（下拉，接 GND） |
| 分压比 | V_gpio = V_bat × R6 / (R5 + R6) = V_bat × 22/172 ≈ V_bat × 0.1279 |
| 校准系数 | **0.9986**（实测 11.734V 时 ADC 算得 11.75V，微调对齐） |

```
12V 主电源
  │
  ├── R5 (150K) ──┬── R6 (22K) ── GND
                  │
                  ├── GPIO2 (ADC1_CH2)
                  │
               C (100nF) ── GND  ← 滤纹波
```

**分压计算（ADC 直读 → 电池电压）：**

| 电池电压 | GPIO2 电压 (×0.1279) | ADC 值 (12bit) | 说明 |
|----------|---------------------|---------------|------|
| 14V（过压阈值） | 1.79V | ~2220 | OVERVOLTAGE |
| 12V（正常） | 1.53V | ~1903 | ✅ |
| **11V（恢复阈值 V1.7.1）** | 1.41V | ~1757 | RECOVERY（从 9V 调高） |
| **9V（欠压阈值 V1.7.1）** | 1.15V | ~1430 | ⚠️ UNDER_VOLT（从 7V 调高） |
| 7V（V1.7.0 旧阈值） | 0.895V | ~1113 | 旧欠压电平 |
| 6V（严重掉电） | 0.767V | ~953 | 触发保护 |

### 15.3 软件架构 — esp_timer 状态机（与主循环解耦）

```
power_monitor_init()
  ├─ ADC1 unit + CH2 配置（复用 ntc_sensor 的 unit？❌ 独立 unit！因为 ntc_sensor 先 init 后 power_monitor 会冲突）
  │   实际：ntc_sensor 用独立 adc_oneshot_unit，power_monitor 也创建自己的 adc_oneshot_unit
  │   → 两个独立 unit_id，互不干扰
  ├─ 创建 esp_timer (PERIODIC, 500ms)
  └─ esp_timer_start → poll_cb() 每 500ms 自动采样

⚠️ 与主循环解耦！主循环不再调 power_monitor_poll()，esp_timer 独立驱动。
   欠压检测速度 = 3 次 × 500ms = 1.5s（之前主循环 5s 轮询要 15s）
```

### 15.4 欠压保护状态机（带滞回 + 防抖 + 冷却）

```
                    ┌─────────────────────────────────────────┐
                    │              正常状态                    │
                    │  s_in_protection = false                │
                    │                                         │
                    │  esp_timer 500ms poll:                  │
                    │    ├─ cooldown 10s 未过？ → return      │
                    │    ├─ V_bat < 9000mV?（★ V1.7.1 从 7000 调高）│
                    │    │    ├─ NO → low_count=0             │
                    │    │    └─ YES → low_count++            │
                    │    │         └─ low_count >= 3?         │
                    │    │              │YES                   │
                    │    │              ▼                     │
                    │    │     ┌───────────────────────────┐ │
                    │    │     │     欠压保护触发           │ │
                    │    │     │  s_in_protection = true   │ │
                    │    │     │  回调 on_power_undervolt() │ │
                    │    │     └───────────────────────────┘ │
                    │    └─ V_bat >= 11000mV?（★ V1.7.1 从 9000 调高）│
                    │         ├─ NO → recovery_count=0        │
                    │         └─ YES → recovery_count++        │
                    │              └─ recovery_count >= 5?    │
                    │                   │YES                  │
                    │                   ▼                     │
                    │            ┌─────────────────────┐      │
                    │            │   恢复退出保护      │      │
                    │            │ s_in_protection=false│     │
                    │            │ s_cooldown += 10s    │     │
                    │            │ 回调 on_power_recovery()    │
                    │            └─────────────────────┘      │
                    └─────────────────────────────────────────┘
```

### 15.5 阈值与防抖参数（power_monitor.h）

| 参数 | 值 | 说明 |
|------|-----|------|
| `POWER_MONITOR_UNDERVOLTAGE_MV` | **9000** ★ V1.7.1 从 7000 调高 | 欠压触发阈值（mV），给 TF 卡 flush 留够电压余量 |
| `POWER_MONITOR_RECOVERY_MV` | **11000** ★ V1.7.1 从 9000 调高 | 恢复退出阈值（mV），保持 2V 滞回 |
| `POWER_MONITOR_OVERVOLTAGE_MV` | 14000 | 过压保护阈值（预留） |
| `POWER_MONITOR_SAMPLE_INTERVAL_MS` | **500** | esp_timer 采样间隔 |
| `POWER_MONITOR_UNDERVOLTAGE_COUNT` | **3** | 欠压连续计数（3×0.5s = 1.5s 触发） |
| `POWER_MONITOR_RECOVERY_COUNT` | **5** | 恢复连续计数（5×0.5s = 2.5s 退出） |
| 冷却时间 | **10s** | 退出保护后 10s 内不许再进（防电压抖动反复触发） |
| `POWER_MONITOR_CAL_FACTOR` | **0.9986f** | 校准系数（实测对齐） |
| 滞回间隔 | **2000mV** | 9V 触发 → 11V 恢复，2V 差值防抖动 |

★ V1.7.1 阈值调整原因：之前 7V 触发时 ESP32 电压已不稳定，SPI 写 TF 卡撑不完。调高到 9V 让 flush 时电压更充足。

### 15.6 on_power_undervolt() 关闭顺序（main.c）

```c
// 第一时间锁死 LED（任何人不能覆盖）
led_status_mode_set(LED_MODE_BLINK_UNDERVOLT);  // 50ms 极速闪

// ★ V1.7.1：先关通信（降低功耗，防 MQTT task 并发访问 TF 卡）
mqtt_force_stop();      // vTaskDelete mqtt_task → disconnect → stop → destroy client
stop_webserver();
wifi_manager_stop();    // CMD_STOP → stop wifi + dns + delete fsm task

// ★ V1.7.1：关完通信再 flush（此时只有 esp_timer 可能并发，靠递归锁保护）
tf_card_emergency_flush();
  ├─ 递归互斥锁加锁（500ms 超时）
  ├─ 若 TF 未 mounted → 尝试重新 init（有电压余量时可恢复）
  ├─ tf_card_flush_all() → flush cache + flush csv_pending（无 fsync，省时间）
  └─ 解锁返回

// 再断继电器（最后硬件动作，确保功耗已降）
switch1_set(false);
switch2_set(false);

// ★ V1.7.1：去掉 power_set(false)（会切 ESP32 自己的电源，导致反复重启）

s_in_protection = true;
```

**★ V1.7.1 emergency_flush 改动**：
- 去掉 marker 文件 `.uv_marker`（少一次 fopen+fwrite+fclose，省 5-10ms）
- mounted=true 时跳过 probe（直接 flush，省一次 SPI 事务）
- 用递归互斥锁保护（mqtt_force_stop 已停 MQTT task，但 esp_timer 回调还可能进来）

### 15.7 on_power_recovery() 恢复顺序（main.c）

```c
s_in_protection = false;
wifi_manager_start();                              // 重新启 WiFi
start_webserver(s_start_time_ms);                  // ★ V1.7.0：补重启 HTTP server（欠压保护里 stop_webserver 清了 s_server=NULL）
led_status_mode_set(LED_MODE_BLINK_SLOW);          // 解除 UNDER_VOLT 锁
// 主循环检测到 wifi_is_connected() 后自动补启动 SNTP + MQTT + OTA
```

### 15.8 主循环保护状态检查

```c
while (1) {
    if (power_monitor_in_protection()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;   // 欠压保护中，跳过所有服务初始化
    }
    // ... 正常业务 ...
}
```

### 15.9 电压上报到 MQTT Field1

ADC 采样值通过 `power_monitor_get_battery_mv()` 暴露，main.c 每 5s 组 MQTT 报文时写入 `Field1`，云端可实时看电池电压。

### 15.10 对外 API

```c
esp_err_t power_monitor_init(void);
bool power_monitor_is_low(void);
bool power_monitor_in_protection(void);           // 查询当前是否在保护状态
int  power_monitor_get_battery_mv(void);          // 读取当前电池电压（mV）
void power_monitor_set_undervolt_cb(cb);          // 设置欠压触发回调
void power_monitor_set_recovery_cb(cb);           // 设置恢复退出回调
void power_monitor_poll(void);                    // 手动 poll（esp_timer 自动驱动，一般不需要调）
```

---

## 16. 事件总线（跨组件协作）

```
┌─────────────────────────────────────────────────────────────┐
│                    事件 / 通知流                              │
│                                                             │
│  WiFi STA_DISCONNECTED                                      │
│       │                                                     │
│       ├─ wifi_is_connected() = false                       │
│       │                                                     │
│       ├─ MQTT 状态机检测到 → 回到 WAIT_WIFI                  │
│       │                                                     │
│       └─ Web Server 状态页自动变"已断开"                      │
│                                                             │
│  WiFi STA GOT_IP                                            │
│       │                                                     │
│       ├─ main.c 循环检测到 → 启动 SNTP + MQTT + OTA          │
│       │                                                     │
│       └─ MQTT 状态机检测到 → 进入 WAIT_TIME                 │
│                                                             │
│  MQTT 收到 /ota/upgrade 消息                                 │
│       │                                                     │
│       ├─ mqtt_aliyun.c → ota_handle_mqtt_msg()              │
│       │       ├─ 解析 JSON                                   │
│       │       └─ 创建 ota_task (优先级 5)                    │
│       │              │                                      │
│       │              ├─ LED → led_notify_ota_start()         │
│       │              │     (强制 BLINK_DOUBLE，覆盖正常状态)   │
│       │              ├─ HTTP 下载 + esp_ota_write()          │
│       │              ├─ MD5 校验                            │
│       │              ├─ esp_ota_set_boot_partition()         │
│       │              │                                      │
│       │              ├─ 成功 → LED_GOOD → 2s → esp_restart() │
│       │              └─ 失败 → LED_BAD → cleanup            │
│                                                             │
│  MQTT 定时上报（30s 周期）                                   │
│       │                                                     │
│       ├─ 读取 NTC1/NTC2 → Field1_data, Field2_data          │
│       ├─ publish 到阿里云                                    │
│       └─ publish 成功 → tf_card_append_csv()                │
│              （自动按日期写 /tf/logs/YYYY-MM-DD.csv）          │
│                                                             │
│  MQTT DISCONNECTED 事件                                     │
│       │                                                     │
│       ├─ s_is_reconnect = true                             │
│       │    （重连后跳过首次上报，等满 30s 再进周期）          │
│       └─ tf_card_flush() ← 紧急写入缓存，防断电丢数据        │
│                                                             │
│  电源监测欠压（esp_timer 500ms 自动 poll）                    │
│       │                                                     │
│       ├─ V_bat < 9000mV 连续 3 次（★ V1.7.1 从 7000mV 调高）   │
│       │    ├─ LED → UNDER_VOLT 极速闪（锁死，不可覆盖）      │
│       │    ├─ ★ V1.7.1：mqtt_force_stop() 先停 MQTT task     │
│       │    ├─ ★ V1.7.1：stop_webserver() + wifi_manager_stop() 降功耗 │
│       │    ├─ ★ V1.7.1：tf_card_emergency_flush() 带递归锁防并发 │
│       │    ├─ switch1/2 全部 OFF（继电器断开）                │
│       │    ├─ ★ V1.7.1：去掉 power_set(false)（切 ESP32 电源会反复重启）│
│       │    └─ s_in_protection = true → 主循环跳过服务启动   │
│       │                                                     │
│       └─ V_bat ≥ 11000mV 连续 5 次（★ V1.7.1 从 9000mV 调高）│
│            ├─ s_in_protection = false + 10s 冷却            │
│            ├─ wifi_manager_start()                          │
│            └─ LED → BLINK_SLOW（解除 UNDER_VOLT 锁）        │
│                                                             │
│  ★ v1.6.5 TF 卡自愈（MQTT task 30s 周期，与写卡同步）         │
│       │                                                     │
│       ├─ MQTT 组报文前 → tf_card_periodic_check()            │
│       │    ├─ tf_card_probe() → 直连 SPI2 发 CMD0 GO_IDLE    │
│       │    │    STATE → 读 R1 响应（绕过 VFS 缓存，真实硬件） │
│       │    ├─ 成功 → 清零 count → "TF card OK"              │
│       │    └─ 失败 → count++ → "probe fail (x/2)"           │
│       │                                                     │
│       ├─ 连续 2 次失败（probe 或写卡）且 !s_reinit_in_progress │
│       │    ├─ s_reinit_in_progress = true（防递归）          │
│       │    ├─ append_cache_flush()  ← 先刷 CSV 缓存          │
│       │    ├─ unmount + spi_bus_free                        │
│       │    └─ tf_card_init() 重新 full init                 │
│       │                                                     │
│       └─ 未 mounted 时 → 每次巡检都尝试 tf_card_init()       │
│              （插卡自动恢复，不用重启）                        │
│                                                             │
│  OTA 启动恢复（ota_init）                                    │
│       │                                                     │
│       └─ 上次固件 ESP_OTA_IMG_PENDING_VERIFY                 │
│             → esp_ota_mark_app_valid_cancel_rollback()       │
│                                                             │
│  Web Server 提交新 WiFi                                     │
│       │                                                     │
│       └─ wifi_manager_request_connect() → CMD_CONNECT_NEW   │
│              │                                              │
│              └─ do_runtime_switch() 热切换                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 17. 关键 Timer

| Timer | 周期 | 用途 |
|-------|------|------|
| s_rssi_timer | 5s | WiFi RSSI 周期更新 |
| s_retry_timer | 15s→30s | AP 模式后台重连 WiFi |
| s_ap_auto_close_timer | 60s / 5s / 60s | AP 自动关闭节能（开机有凭证→60s等WiFi；配网成功→5s轮询MQTT；MQTT连上后→延迟60s给用户查IP） |
| s_ap_idle_timer | 300s | 配网模式超时关闭（无凭证/恢复出厂时启动） |
| MQTT tick | 任务内 100ms | MQTT 状态机轮询 |
| ★ s_cache_flush_timer | **5 min** | **cache 路径定时 flush（V1.7.1 新增，cache 有数据才触发，CSV 直写不走此 timer）** |

---

## 18. WiFi 连接参数

| 参数 | 值 |
|------|-----|
| AP 默认 SSID | `DEV-MGT_26001` |
| AP IP | `192.168.4.1` |
| AP DHCP 网关 | `192.168.4.1` |
| AP 最大连接 | 4 |
| AP 频道 | 1 |
| STA 协议 | 11b/g/n/ax |
| AP 协议 | 11b/g/n |
| STA PMF | capable=true, required=false |
| STA 省电 | PS_NONE |
| 首次连接超时 | 15s |
| 首次重试次数 | 3 |

---

## 19.1 sdkconfig 关键配置

### OTA / TLS 相关

```ini
CONFIG_ESP_TLS_INSECURE=y                  # 允许 insecure TLS（OTA HTTPS 必需）
CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y   # 跳过服务器证书验证（阿里云 OSS CA 不在 Mozilla 包里）
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y        # 编译 Mozilla 根证书包（MQTT 连接阿里云仍需要）
CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y      # crash 时打印 backtrace 后自动重启（方便调试）
```

### Flash / OTA 分区

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

### 日志级别

```ini
CONFIG_LOG_DEFAULT_LEVEL_INFO=y             # 默认 Info
CONFIG_LOG_MAX_LEVEL_VERBOSE=y             # 编译时保留 verbose 日志（runtime 可调）
```

---

## 19.2 已知行为 & 设计决策

1. **AP 常开策略**：即使 STA 成功连上，AP 也不关（APSTA 共存模式），手机随时能连 AP 进配置页
2. **运行时掉线永不放弃**：`s_sta_ever_connected` 标志确保连上过后掉线会无限重试
3. **do_runtime_standalone_ap 不清空 STA 凭证**：只 disconnect，不 `set_config(empty)`，让 retry_timer 能直接用 NVS 里的凭证
4. **热切换不 stop WiFi**：`do_runtime_switch()` 只 disconnect + set_config + connect，AP 和 Web Server 全程不掉线
5. **retry_timer 防护逻辑**：如果 `s_sta_connect_allowed && s_sta_ever_connected` 已经成立（即时重连在工作），定时器只重新 schedule，不重复触发 connect
6. **CMD_OPEN_AP 恢复 STA**：从 STA_ONLY 状态打开 AP 时，自动从 NVS 加载凭证并发起 STA 连接
7. **OTA 与正常状态优先级独立**：OTA 专用 LED 模式（DOUBLE/GOOD/BAD）通过 `led_status_mode_set()` 直接覆盖，不走 resolve_status_mode 优先级链，OTA 期间 LED 完全由 OTA 状态决定
8. **OTA 重入保护**：`s_ota_in_progress` 标志防止 MQTT 连续推送多个 OTA 消息导致并发下载
9. **OTA 不校验版本新旧**：阿里云推送或 TF 卡 bin 只要合法（JSON 解析成功 / bin magic=0xE9）就直接升级，不做版本号比较
10. **OTA 无 MD5 校验降级**：MD5 初始化失败不中止 OTA，signMethod≠MD5 或 fwSign 为空时跳过校验直接写入
11. **ESP-IDF v6.x TLS 默认强制证书验证**：必须 `CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`，否则 HTTPS 连接失败。传输层安全由上层 MD5 签名补偿
12. **HTTP status_code=0 必须 fetch_headers 兜底**：ESP-IDF v6.x 的 `esp_http_client_open` 在某些场景返回 OK 但不自动解析响应头，必须主动 `esp_http_client_fetch_headers()`
13. **阿里云 OTA URL 可能带反引号**：推送的 JSON 里 URL 字段有 `` ` `` 前后包裹，HTTP 请求前必须 trim
14. **MQTT 内部任务栈必须显式设 8192**：事件回调内执行 subscribe + publish 需要足够栈空间，默认值可能导致 crash
15. **mqtt_manager_task 栈 8192 不够会栈溢出 panic**：snprintf 里 7 个 `%.2f` 浮点格式化（newlib 每个 ~300 字节）+ 调用链 + 局部变量 ≈ 4.5KB，4096 不够会在 `malloc` 内部崩溃。此问题直接触发 `Guru Meditation Error: Stack protection fault`（栈越过边界 20 字节）

---

## 20. Flash 分区表（partitions.csv）

芯片：ESP32-C3 | Flash：4 MB

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| nvs | 0x9000 | 24 KB | WiFi 凭证等非易失存储 |
| phy_init | 0xF000 | 4 KB | RF 校准数据 |
| ota_0 | 0x10000 | **1984 KB** | 固件槽位 0（主运行） |
| ota_1 | 0x200000 | **1984 KB** | 固件槽位 1（OTA 备用） |
| ota_data | 0x3F0000 | **8 KB** | OTA 状态（ESP-IDF 强制 0x2000） |

### 说明

- 固件实际大小约 1067 KB，每个 OTA 分区 1984 KB，剩余 **917 KB** 给后续功能扩展
- ota_data 必须是 0x2000（ESP-IDF 硬性要求），不可更改
- Flash 尾部约 896 KB 未使用（ota_0/ota_1 已最大化）
- ⚠️ 改分区表需 `idf.py erase-flash` 全片擦除，NVS 清空

---

## 21. V1.7.0 改动记录

### 21.1 跨天查询 invalid_time 修复（history_query.c）

**问题**：V2.0 历史查询协议支持 StartDate/EndDate 范围查询（跨多天），但单日时间校验逻辑 if (start_sec < 0 || end_sec < 0 || start_sec > end_sec) 中的 start_sec > end_sec 在以下场景会误报 INVALID_TIME：

| 场景 | start_time | end_time | start_sec | end_sec | start_sec > end_sec | 结果 |
|------|-----------|----------|-----------|---------|---------------------|------|
| 正常单日 | 08:00:00 | 23:59:59 | 28800 | 86399 | ❌ 不触发 | ✅ 合法 |
| 正常单日 | 00:00:00 | 00:30:00 | 0 | 1800 | ❌ 不触发 | ✅ 合法 |
| **跨午夜单日** | 23:59:59 | 00:00:00 | 86399 | 0 | ✅ **触发** | ❌ 误报 INVALID_TIME |
| **跨天范围查询** | 23:59:59 | 00:00:00 | 86399 | 0 | ✅ **触发** | ❌ 误报 INVALID_TIME |

**修复**（history_query.c:297）：移除 start_sec > end_sec 条件，改为只校验 start_sec < 0 || end_sec < 0。时间范围合法性由 StartDate/EndDate 日期范围隐式保证，跨午夜场景应被视为合法（例：查询 09-19 23:59:59 到 09-20 00:00:00）。

`c
// 修复前
if (start_sec < 0 || end_sec < 0 || start_sec > end_sec) {
    // 报 INVALID_TIME
}

// 修复后
if (start_sec < 0 || end_sec < 0) {
    // 仅校验时间格式合法性（HH:MM:SS → 必须各段数字且范围合法）
}
`

**连带影响**：此修复同时消除了 App 端在 v2.0→v2.3 升级中遇到的"跨天查询闪退"根因之一（App 端闪退另一原因是 TemperatureChartView.setHistoryData() 未排序 + NaN 过滤缺失，已在 App v2.3 中修复）。

### 21.2 HTTP Server listen(112) ENOBUFS 修复（web_server.c + main.c）

**问题**：系统启动后 HTTP server 反复报 `httpd_server_init: error in listen (112)` 失败，但 WiFi AP 页面实际已能访问。

**根因**：不是 heap 不够，也不是 LwIP MEMP 池不够。是逻辑 bug 导致 `start_webserver()` 被调用了**两次**：

```
app_main():
  wifi_manager_start()  →  start_webserver()   ← 第1次，WiFi AP模式，heap=151KB → 成功 ✓
  wifi_is_connected()=false → s_services_started 未设 true

WiFi STA 连上后主循环检测到 !s_services_started:
  → start_webserver()   ← 第2次，WiFi STA已连，heap被TCP/IP吃了14KB → 失败 ✗
```

第1次 HTTP server 已经成功启动并在运行，但第2次因为 heap 少了 ~14KB，`tcp_pcb_listen` 内部 `sys_mbox_new(acceptmbox)` 分配失败返回 `ENOBUFS (112)`。

**修复**：

| 文件 | 改动 |
|------|------|
| `components/web_server.c` | 入口加防重入：`if (s_server != NULL) { ESP_LOGI("webserver already running"); return; }` |
| `main/main.c` | 主循环里去掉重复的 `start_webserver()` 调用（只补启 SNTP + MQTT + OTA） |
| `main/main.c` | `on_power_recovery()` 里加 `start_webserver()`（欠压保护里 `stop_webserver()` 清了 s_server=NULL，恢复时需要重建） |
| `components/web_server.c` | HTTP server 配置降资源：stack_size 8192→4096，backlog_conn 3→2，max_open_sockets=4，ctrl_port=32768 |

**修复后日志**：
```
I (876) HTTP: === start_webserver ===
I (876) HTTP: Web server started OK, heap now free=143320
I (2436) WIFI: WiFi Connected!
I (5886) OTA: OTA init, version: 1.7.0    ← 不再有第二次 start_webserver，不再有 listen(112)
```
---

## 22. V1.7.1 改动记录

### 22.1 TF 卡：递归互斥锁 + CSV 直写 + 定时 flush

**背景**：掉电检测触发 	f_card_emergency_flush() 时，MQTT task 和 esp_timer 回调可能并发访问 TF 卡，导致 flush 失败丢数据。

**改动**（components/tf_card.c）：

| 改动 | 说明 |
|------|------|
| 新增 s_mutex（递归互斥锁） | 所有公共函数（init/deinit/append_file/append_csv/flush/probe/emergency_flush 等）加锁，防止 MQTT task 与 esp_timer（欠压回调）并发 |
| ppend_csv() 改直写 | 之前走 2KB cache，现在直接 open("a") + fwrite + fclose，掉电最多丢 1 行（30s 周期） |
| 去掉 fsync | fclose 已触发 FatFs 目录更新，额外 fsync 每次省几十 ms |
| 新增 s_cache_flush_timer（esp_timer） | 5 分钟周期，cache 有数据才触发 flush，cache 路径数据丢失窗口从无限缩短到 5 分钟 |
| emergency_flush 去掉 marker 文件 .uv_marker | 少一次 fopen+fwrite+fclose，省 5-10ms 掉电关键时间 |
| emergency_flush mounted 时跳过 probe | mounted=true 直接 flush，省一次 SPI 事务 |

### 22.2 电源监测阈值调高

**背景**：之前欠压阈值 7V，触发时 ESP32 电压已不稳定，SPI 写 TF 卡撑不完一个完整 flush。

**改动**（components/power_monitor.h）：

| 参数 | 修改前 | 修改后 |
|------|--------|--------|
| POWER_MONITOR_UNDERVOLTAGE_MV | 7000 | **9000** |
| POWER_MONITOR_RECOVERY_MV | 9000 | **11000** |

滞回间隔保持 2000mV 不变。

### 22.3 PCF8563：STOP 位检查 + Century bit + 写回验证

**改动**（components/pcf8563.c）：

| 改动 | 说明 |
|------|------|
| init 读 Control_status1(0x00) 检查 STOP 位（bit5） | STOP=1 时清除，重启振荡器 |
| set_time() 加 Century bit | 月份寄存器 bit7，	m_year >= 100 时设 1（PCF8563 year 寄存器是 2 位，Century bit 标识 20xx） |
| main.c SNTP 回写后立即读回验证 | diff ≤ 2 秒才算成功，否则打 WARN |

### 22.4 时区设置移到 app_main 最开头

**背景**：之前 setenv("TZ", "CST-8", 1) + tzset() 在 sntp_init_and_sync() 里调用，但 PCF8563 的 mktime() → settimeofday() 在 sntp_init 之前就执行了，导致系统时间在错误时区（UTC）下被写入，后续 localtime_r() 多 +8 小时。

**改动**：时区设置从 components/sntp_sync.c 移到 main/main.c 的 pp_main() 开头（NVS 之后、任何硬件 init 之前）。同时删除 sntp_sync.c 里重复的时区设置。

### 22.5 欠压回调流程调整

**改动**（main/main.c on_power_undervolt()）：

| 改动 | 说明 |
|------|------|
| 先关通信再 flush | mqtt_force_stop() + stop_webserver() + wifi_manager_stop() 先执行，降功耗 + 停 MQTT task，避免与 flush 并发 |
| 去掉 power_set(false) | 会切 ESP32 自己的电源，导致反复重启 |

### 22.6 Field1_data / Field2_data 从 int 改 float

**背景**：NTC 温度采样返回 float（如 25.63°C），但 `s_field1_data/s_field2_data` 声明为 `int`，赋值时 `(int)ntc1` 强转截断小数，上报格式 `%d` 输出整数。APP 里这两个字段显示温度，没有小数点。

**改动**（components/mqtt_aliyun.c + mqtt_aliyun.h）：

| 位置 | 修改前 | 修改后 |
|------|--------|--------|
| `static s_field1_data/s_field2_data` | `int` | **`float`** |
| 赋值（第 458-459 行） | `(int)ntc1` / `(int)ntc2` | **`ntc1` / `ntc2`**（去掉强转） |
| 周期上报 snprintf | `"Field1_data":%d` | **`"Field1_data":%.2f`** |
| ACK 回包 snprintf | `"Field1_data":%d` | **`"Field1_data":%.2f`** |
| ACK 解析 `j_field1_data` | `valueint` | **`(float)valuedouble`** |
| getter 函数声明 | `int mqtt_get_field1_data()` | **`float mqtt_get_field1_data()`** |

web_server.c 里的 `cJSON_AddNumberToObject(root, "field1_data", ...)` 自动接受 float，无需改动。

### 22.7 mqtt_manager_task 栈 4096 → 8192

**背景**：22.6 把上报 snprintf 里 `Field1_data/Field2_data` 从 `%d` 改成 `%.2f` 后，整行 snprintf 含 **7 个浮点格式化**（`Temp:%.1f, Field1:%.2f, Field1_data:%.2f, Field2:%.2f, Field2_data:%.2f, Set1:%.2f, Set2:%.2f`）。ESP-IDF newlib 处理每个 `%f` 需 ~300 字节临时栈（`%d` 仅需 ~50 字节）。7 × 300 = 2100 字节光格式化就吃掉，加上 snprintf 自身局部变量 + 调用链，4096 不够触发栈溢出。

**症状**：`Guru Meditation Error: Core 0 panic'ed (Stack protection fault)`，栈指针越过栈底 20 字节，崩溃点在 `malloc`（newlib 内部格式化调用）。

**改动**（components/mqtt_aliyun.c 第 517 行）：`xTaskCreate(..., "mqtt_mgr", 8192, ...)`。

---

## 23. V1.7.2 改动记录（NTC 测温修复）

### 23.1 每个 ADC 通道独立 curve_fitting 校准

**问题**：V1.7.0 只创建一个 curve_fitting 校准句柄（`ADC_CHANNEL_0`），被 ntc_sensor 和 power_monitor 共同用来校准 CH3/CH4/CH2。跨通道校准导致 CH3 电压严重不准（万用表 1657mV，ESP32 算出 2855mV → 误差 +72%）。

**改动**（components/ntc_sensor.c + power_monitor.c）：

| 改动 | 说明 |
|------|------|
| ntc_sensor_init 创建 2 个独立 curve_fitting | s_cali1 校 CH3，s_cali2 校 CH4 |
| power_monitor_init 创建自己的 curve_fitting | s_cali_handle 校 CH2，自己创建 adc_oneshot_unit |
| 删除跨通道 `ntc_sensor_get_cali_handle()` | 不再共享句柄 |

### 23.2 Sanity check 防坏 efuse

**问题**：部分 ESP32-C3 芯片 CH3 的 curve_fitting efuse 数据在满量程（raw=4095）时严重偏离（如给 2855mV 而实际应 4400mV），但正常电压范围（raw 2000 左右）校准是准的。之前的 sanity check 在**所有** raw 值下比较，拔掉 NTC（开路 raw=4095）时错误标记 `s_cali_bad=true`，插回 NTC 后一直用不准的 fallback。

**改动**：sanity check 包在 `if (raw > 100 && raw < 4000)` 里，开路/短路时跳过。正常范围偏差 >30% 才标记 bad。

### 23.3 故障检测改用 raw 值

**问题**：之前用校准后的 voltage_mv 判断开路（≥3300mV），但 curve_fitting 在满量程偏了给 2855mV < 3300mV → 漏检。拔掉 NTC 后算出假温度 -10.65°C 而不是 -40°C。

**改动**（components/ntc_sensor.h + ntc_sensor.c）：

| 故障 | 修改前 | 修改后 |
|------|--------|--------|
| 开路 | `voltage_mv >= 3300` | **`raw >= 4000`**（ADC 满量程，校准前先判） |
| 短路 | `raw <= 10` | `raw <= 10`（不变） |
| NaN/Inf | 有 | 有（兜底） |

### 23.4 ADC 通道切换残留清除

**问题**：拔掉 CH3 传感器（开路 3.3V 满量程）后，ESP32 ADC 内部 ~4pF 采样保持电容残留高压，切到 CH4 时读数偏高（正常 1.6V 被读成 ~2.5V），算出假低温 4.89°C。

**改动**（components/ntc_sensor.c calc_temp）：每次切通道后**丢弃第一次采样** + `esp_rom_delay_us(500)` 让保持电容充分稳定，再采 10 次平均。

### 23.5 NTC 参数实测校准

**改动**（components/ntc_sensor.h）：`NTC_NOMINAL_RES_OHM` 从 10000Ω 改为 **10615Ω**（两个通道实测 25°C 下的平均阻值，对齐万用表）。

---

## 24. V1.7.3 改动记录（NTC 测温精度修复）

### 24.1 fallback 电压硬编码 4400 → NTC_VOLTAGE_MV

**问题**：`raw_to_voltage_safe` 里 fallback 公式写死 `raw * 4400 / 4095`，但实际 VCC 是 3300mV。导致 curve_fitting vs fallback 偏差计算在低温高电压段刚好超过 33% 阈值（如 raw=2827 时差 33.6%），错误触发 fallback 降级，用了不准的硬编码值。

**改动**（components/ntc_sensor.c 第 44 行）：`raw * 4400 / 4095` → `raw * NTC_VOLTAGE_MV / 4095`。

### 24.2 硬件改 47kΩ 上拉电阻

**问题**：ESP32-C3 ADC 用 `ADC_ATTEN_DB_12` 时有效线性范围只有 **0~2500mV**。10kΩ 上拉 + NTC 在低温（<-15°C）时分压点超过 2800mV，进入严重非线性区。curve_fitting 校准只能补偿偏移和增益，无法修正硬件级非线性。表现为：CH4 在 -16.5°C 时 ADC raw 已经到 4095（满量程封顶），温度无法继续往下测。

**改动**：硬件上把两路 NTC 的上拉电阻从 **10kΩ 改为 47kΩ**。改后整个量程（-30°C ~ 100°C）分压都在 0~2200mV，完全落在 ADC 线性区。

| 温度 | 10kΩ 上拉分压 | 47kΩ 上拉分压 | 在线性区？ |
|------|--------------|--------------|-----------|
| -30°C | ~3050mV ❌ | ~1700mV ✅ | |
| -15°C | ~2850mV ❌ | ~1400mV ✅ | |
| 25°C | ~1700mV ✅ | ~650mV ✅ | |
| 100°C | ~800mV ✅ | ~220mV ✅ | |

**代码同步**（components/ntc_sensor.h）：`NTC_FIXED_RES_OHM` 从 10000 改为 **47000**。

### 24.3 NTC 参数重新校准

电阻值变了 + curve_fitting 校准现在可靠了（全部在线性区），用两个实测温度点（冷藏室 10.8°C, 冷冻室 -11.1°C）联立 B 值公式重新拟合：

| 参数 | V1.7.2 | V1.7.3 | 校准依据 |
|------|--------|--------|---------|
| R0 @ 25°C | 10615Ω | **10700Ω** | curve_fitting 实测 r_ntc 联立 |
| B 值 | 3950 | **3415** | 同上 |

**校准验证**：

| 通道 | 公式算 | 真实温度 | 误差 |
|------|--------|---------|------|
| CH3 冷藏室 | 10.80°C | 10.8°C | 0.00°C |
| CH4 冷冻室 | -11.10°C | -11.1°C | 0.00°C |

### 24.4 移除开路/短路阈值检测

**问题**：之前用 `raw >= 4000` 判开路、`raw <= 10` 判短路，但改 47kΩ 后 ADC 在线性区不会封顶，阈值判定反而干扰正常测量（如 CH4 在 -16.5°C 时 raw=4095 被判为开路）。

**改动**：
- 移除 `raw >= NTC_OPEN_RAW_THRESHOLD` → 返回 -40°C 的判定
- 移除 `raw <= NTC_SHORT_RAW_THRESHOLD` → 返回 150°C 的判定
- 移除 `v_ratio >= 0.999f` → 返回 -40°C 的判定
- 移除 `v_ratio <= 0.001f` → 返回 150°C 的判定
- 保留 v_ratio 温和钳位到 [0.001, 0.999] 仅防止除零
- 保留 NaN/Inf 兜底返回 -40.0°C

**原则**：公式算多少就显示多少，让数据诚实反映硬件状态。