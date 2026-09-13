# ESP32-C3 固件架构说明

> 版本: V1.3.4 | 芯片: ESP32-C3 | IDF: v6.1-rc1 / v5.5.5

---

## 1. 目录结构

```
esp32_c3_V1.3.3/
├── main/                      # 主程序入口
│   ├── main.c                 # app_main() 启动流程
│   ├── version.h              # 版本号 / 芯片信息
│   └── CMakeLists.txt
├── components/                # 组件库（驱动统一放这里）
│   ├── wifi_manager.c/h       # ★ WiFi 状态机（STA+AP 共存）
│   ├── mqtt_aliyun.c/h        # ★ 阿里云 MQTT 状态机
│   ├── ota_manager.c/h        # ★ OTA 升级管理器（MQTT 触发 + HTTP 下载）
│   ├── web_server.c/h         # HTTP 服务器（配置/状态查看）
│   ├── dns_server.c/h         # DNS 服务器（AP 模式 CNAME 劫持）
│   ├── sntp_sync.c/h          # SNTP 时间同步
│   ├── led.c/h                # 状态 LED 驱动（含 OTA 指示模式）
│   ├── switch.c/h             # 开关 / 电源 GPIO 驱动
│   └── temp_sensor.c/h        # 内部温度传感器驱动
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
  ├─ 2. 硬件初始化
  │     ├─ switch_init()       # GPIO 开关驱动
  │     ├─ led_init()          # 状态 LED + PWM 定时器
  │     └─ temp_sensor_init()  # 内部温度传感器
  │
  ├─ 3. 启动 WiFi 管理器（自动连接或进 AP 配网）
  │     └─ wifi_manager_start()  → 创建 fsm_task 状态机
  │
  ├─ 4. 若启动时已连上 WiFi → 立即启动 SNTP + MQTT + OTA
  │     ├─ sntp_init_and_sync()
  │     ├─ mqtt_init()
  │     └─ ota_init()          # 检查上次 OTA 状态（pending verify → mark valid）
  │
  ├─ 5. 启动 Web 服务器（即使 WiFi 没连上也能访问 AP 页面）
  │     └─ start_webserver()
  │
  └─ 6. 主循环（5秒轮询）
        └─ 检测到 WiFi 连上但服务未启动 → 补启动 SNTP + MQTT + OTA
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
模式 A：开机有凭证
  do_wifi_start_once() → AP 全开（给手机 60s 配置窗口）
  s_ap_close_wait_mqtt = false（默认）
  start_ap_auto_close_timer()  ← 启动 60s 定时器
       │
       ├─ 60s 后回调 ap_auto_close_cb():
       │    ├─ s_wifi_connected == true → do_close_ap() 节能 ✅
       │    │   （不等 MQTT，开机场景 MQTT 慢但不需要等）
       │    │
       │    └─ s_wifi_connected == false → AP 保持开启
       │        （用户可能还在等配置 / 密码错了正在 retry_timer 重试）
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
       │    └─ mqtt_is_connected() == true
       │         → 延迟 10s → do_close_ap() ✅
       │         （给手机留够时间看到"配网+云连接"全部成功）
       │
       └─ WiFi 在等待期间断开 → reset 标志 → AP 保持开启
          （重连后不再触发自动关闭，避免干扰）

定时器停止条件:
  - do_close_ap() 里 stop_ap_auto_close_timer()
  - 定时器自然到期（单次触发 / 循环重 schedule）
```

**设计意图**：
- 开机场景：给 1 分钟配置窗口，之后不管 MQTT 连没连都关 AP（开机时用户可能不需要手机干预）
- 配网场景：等整个流程（WiFi + MQTT）都成功后再关 AP，让用户通过 LED 状态确认"配网完成"后自动收网
- MQTT 等待超时保护：避免因 MQTT 故障导致 AP 永不关闭

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
- **RX 环形缓冲区**：8 条 `mqtt_rx_entry_t`，保存最近收到的消息
- **阿里云参数上报**：`mqtt_publish_aliyun_params(json)` 用 `/sys/{device}/thing/event/property/post` 主题
- **OTA 消息分发**：RX 事件检测到 topic 含 `/ota/upgrade` → 直接调用 `ota_handle_mqtt_msg()` 进入 OTA 流程（跳过常规 JSON 解析）
- **MQTT 缓冲区**：`.buffer.size` / `.buffer.out_size` = **2048**（原为 512，防止 OTA 推送 JSON 被内部截断）
- **内部任务栈**：`.task.stack_size` = **8192**（显式设置，防止事件回调内 subscribe + publish 栈溢出）
- **事件回调内执行 subscribe**：MQTT_EVENT_CONNECTED 里直接调 esp_mqtt_subscribe 三个 OTA 主题，返回值检查 + 错误日志

### 4.3 对外 API

```c
void mqtt_init(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_publish_custom(topic, data, qos);
esp_err_t mqtt_publish_aliyun_params(params_json);
int mqtt_get_rx_entries(out, max_count);
const char *mqtt_get_last_rx_topic(void);
const char *mqtt_get_last_rx_data(void);
```

---

## 5. OTA 升级（ota_manager.c）

### 5.1 触发流程

```
阿里云控制台推送 /ota/upgrade
  │
  ▼
mqtt_aliyun.c RX 事件检测到 topic 含 "/ota/upgrade"
  │
  ▼
ota_handle_mqtt_msg()
  ├─ 解析 JSON（cJSON）→ ota_notify_t
  ├─ 版本号校验 is_newer_version()（语义版本比较 x.y.z）
  │     └─ 非 forceUpgrade 且版本相同或更旧 → 回复 200 忽略
  ├─ MQTT 连接检查
  └─ 创建 ota_task（优先级 5，栈 **12288** 字节）
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
| `/sys/{pk}/{dn}/ota/post` | 进度上报（step="2" + percent），最终结果（step="0" 成功 / step="3" 失败） |

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

#### 版本比较防御

`is_newer_version()` 调用 `parse_version_ints()` 封装 sscanf，只有 sscanf 匹配到 ≥ 2 个字段才返回 true，防止异常版本字符串导致 sscanf 返回 0 比较出错。

### 5.5 MD5 校验

- 使用 mbedtls v3 API：`mbedtls_md_init()` + `mbedtls_md_setup()` + `mbedtls_md_starts()`
- 下载过程中持续 `mbedtls_md_update()` 累积哈希
- 下载完成后 `mbedtls_md_finish()` → 32 位小写十六进制字符串
- 与 `fwSign` 忽略大小写比对（`strcasecmp`）
- **降级策略**：MD5 初始化失败不中止 OTA，继续写入（仅在 signMethod=MD5 且 fwSign 非空时才比对）

### 5.6 分区切换与重启

1. `esp_ota_end(ota_handle)` → 提交固件到目标分区
2. `esp_ota_set_boot_partition(target)` → 设下次启动用新分区
3. 成功时 `led_notify_ota_success()` → LED 3 快闪 + 常亮
4. 等待 **3s**（`OTA_REBOOT_DELAY_MS`）→ `esp_restart()`

### 5.7 开机恢复（ota_init）

```c
const esp_partition_t *running = esp_ota_get_running_partition();
esp_ota_img_states_t img_state;
esp_ota_get_state_partition(running, &img_state);

if (img_state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();  // 新固件启动成功 → 标记有效
}
if (img_state == ESP_OTA_IMG_ABORTED) {
    ESP_LOGW("Previous OTA aborted, device may have rolled back");
}
```

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
```

### 5.10 对外 API

```c
void ota_init(void);                                  // 开机恢复 + 初始化二值信号量
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
- **NTP 服务器**：默认 pool.ntp.org
- **MQTT 依赖**：MQTT 状态机 WAIT_TIME 状态等 SNTP 完成后才继续（TLS 需要有效时间戳）

---

## 9. LED 驱动

### 9.1 硬件配置

| 项目 | 值 |
|------|-----|
| GPIO | **13** |
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
| 5 | SWITCH0 | 开关 0 |
| 12 | SWITCH1 | 开关 1 |
| 8 | SWITCH2 / bit1 | 开关 2 |
| 4 | POWER | 电源控制 |

---

## 11. 温度传感器

- 使用 ESP32-C3 内置温度传感器（ADC 通道）
- `temp_sensor_init()` → 校准
- `temp_sensor_get()` → 返回 float 摄氏度

---

## 12. GPIO 分配总表

| GPIO | 方向 | 功能 | 组件 |
|------|------|------|------|
| 4 | 输出 | POWER | switch |
| 5 | 输出 | SWITCH0 | switch |
| 8 | 输出 | SWITCH2 | switch |
| 12 | 输出 | SWITCH1 | switch |
| 13 | 输出 | 状态 LED | led |
| - | - | WiFi | WiFi 协议栈 |
| - | - | 温度传感器 | ADC 内部 |

---

## 13. 事件总线（跨组件协作）

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
│       │       ├─ 解析 + 版本校验                            │
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

## 14. 关键 Timer

| Timer | 周期 | 用途 |
|-------|------|------|
| s_rssi_timer | 5s | WiFi RSSI 周期更新 |
| s_retry_timer | 15s→30s | AP 模式后台重连 WiFi |
| s_ap_auto_close_timer | 60s / 5s | AP 自动关闭节能（开机有凭证→60s；配网成功→5s轮询MQTT） |
| s_ap_idle_timer | 300s | 配网模式超时关闭（无凭证/恢复出厂时启动） |
| MQTT tick | 任务内 100ms | MQTT 状态机轮询 |

---

## 15. WiFi 连接参数

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

## 16.1 sdkconfig 关键配置

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

## 16.2 已知行为 & 设计决策

1. **AP 常开策略**：即使 STA 成功连上，AP 也不关（APSTA 共存模式），手机随时能连 AP 进配置页
2. **运行时掉线永不放弃**：`s_sta_ever_connected` 标志确保连上过后掉线会无限重试
3. **do_runtime_standalone_ap 不清空 STA 凭证**：只 disconnect，不 `set_config(empty)`，让 retry_timer 能直接用 NVS 里的凭证
4. **热切换不 stop WiFi**：`do_runtime_switch()` 只 disconnect + set_config + connect，AP 和 Web Server 全程不掉线
5. **retry_timer 防护逻辑**：如果 `s_sta_connect_allowed && s_sta_ever_connected` 已经成立（即时重连在工作），定时器只重新 schedule，不重复触发 connect
6. **CMD_OPEN_AP 恢复 STA**：从 STA_ONLY 状态打开 AP 时，自动从 NVS 加载凭证并发起 STA 连接
7. **OTA 与正常状态优先级独立**：OTA 专用 LED 模式（DOUBLE/GOOD/BAD）通过 `led_status_mode_set()` 直接覆盖，不走 resolve_status_mode 优先级链，OTA 期间 LED 完全由 OTA 状态决定
8. **OTA 重入保护**：`s_ota_in_progress` 标志防止 MQTT 连续推送多个 OTA 消息导致并发下载
9. **OTA 版本比较只认 x.y.z**：`is_newer_version()` 用 sscanf 解析三位数字，非标准格式一律认为版本不更新
10. **OTA 无 MD5 校验降级**：MD5 初始化失败不中止 OTA，signMethod≠MD5 或 fwSign 为空时跳过校验直接写入
11. **ESP-IDF v6.x TLS 默认强制证书验证**：必须 `CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`，否则 HTTPS 连接失败。传输层安全由上层 MD5 签名补偿
12. **HTTP status_code=0 必须 fetch_headers 兜底**：ESP-IDF v6.x 的 `esp_http_client_open` 在某些场景返回 OK 但不自动解析响应头，必须主动 `esp_http_client_fetch_headers()`
13. **阿里云 OTA URL 可能带反引号**：推送的 JSON 里 URL 字段有 `` ` `` 前后包裹，HTTP 请求前必须 trim
14. **MQTT 内部任务栈必须显式设 8192**：事件回调内执行 subscribe + publish 需要足够栈空间，默认值可能导致 crash

---

## 17. Flash 分区表（partitions.csv）

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