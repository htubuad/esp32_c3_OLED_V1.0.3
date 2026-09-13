# ESP32-C3 固件架构说明

> 版本: V1.3.3 | 芯片: ESP32-C3 | IDF: v6.1-rc1 / v5.5.5

---

## 1. 目录结构

```
esp32_c3_V1.3.1/
├── main/                      # 主程序入口
│   ├── main.c                 # app_main() 启动流程
│   ├── version.h              # 版本号 / 芯片信息
│   └── CMakeLists.txt
├── components/                # 组件库（驱动统一放这里）
│   ├── wifi_manager.c/h       # ★ WiFi 状态机（STA+AP 共存）
│   ├── mqtt_aliyun.c/h        # ★ 阿里云 MQTT 状态机
│   ├── web_server.c/h         # HTTP 服务器（配置/状态查看）
│   ├── dns_server.c/h         # DNS 服务器（AP 模式 CNAME 劫持）
│   ├── sntp_sync.c/h          # SNTP 时间同步
│   ├── led.c/h                # 状态 LED 驱动
│   ├── switch.c/h             # 开关 / 电源 GPIO 驱动
│   └── temp_sensor.c/h        # 内部温度传感器驱动
├── partitions.csv             # 分区表
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
  │     ├─ led_init()          # 状态 LED
  │     └─ temp_sensor_init()  # 内部温度传感器
  │
  ├─ 3. 启动 WiFi 管理器（自动连接或进 AP 配网）
  │     └─ wifi_manager_start()  → 创建 fsm_task 状态机
  │
  ├─ 4. 若启动时已连上 WiFi → 立即启动 SNTP + MQTT
  │
  ├─ 5. 启动 Web 服务器（即使 WiFi 没连上也能访问 AP 页面）
  │     └─ start_webserver()
  │
  └─ 6. 主循环（5秒轮询）
        └─ 检测到 WiFi 连上但服务未启动 → 补启动 SNTP + MQTT
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

### 3.3 开机 AP 自动关闭节能机制

```
开机路径（有凭证）:
  do_wifi_start_once() → AP 全开（给手机 60s 配置窗口）
  start_ap_auto_close_timer()  ← 启动 60s 定时器
       │
       ├─ 60s 后回调 ap_auto_close_cb():
       │    ├─ s_wifi_connected == true → do_close_ap() 节能 ✅
       │    │   s_state 从 STA_CONNECTED → STA_ONLY
       │    │
       │    └─ s_wifi_connected == false → AP 保持开启
       │        （用户可能还在等配置 / 密码错了正在 retry_timer 重试）
       │
       └─ 无凭证开机 → 不启动此定时器（用户需要 AP 配置）

定时器停止条件:
  - do_close_ap() 里 stop_ap_auto_close_timer()
  - 定时器自然到期（单次触发）
```

**设计意图**：开机给 1 分钟窗口让手机能连 AP 进配置页，之后自动关 AP 省电。如果 1 分钟到了 STA 还没连上（密码错误等），AP 保持开启方便用户排查。

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
static esp_timer_handle_t s_ap_auto_close_timer;  // 开机 AP 自动关闭定时器（60秒）
static esp_timer_handle_t s_ap_idle_timer;       // AP 配网超时定时器（300秒，纯配网场景）
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

## 5. Web Server

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

## 6. DNS Server

- **协议**：UDP 53
- **作用**：AP 模式下把所有域名解析到 `192.168.4.1`（即设备自身），实现 **Captive Portal**（手机连上热点自动跳转到配置页）
- **生命周期**：`dns_server_start()` 在 `do_wifi_start_once()` 和 `do_open_ap()` 里调用；`dns_server_stop()` 在 `do_close_ap()` 里调用

---

## 7. SNTP 时间同步

- **触发**：WiFi 连接成功后（main.c 里检测）
- **NTP 服务器**：默认 pool.ntp.org
- **MQTT 依赖**：MQTT 状态机 WAIT_TIME 状态等 SNTP 完成后才继续（TLS 需要有效时间戳）

---

## 8. LED 驱动

### 8.1 硬件配置

| 项目 | 值 |
|------|-----|
| GPIO | **13** |
| 驱动方式 | **LEDC (PWM)** |
| PWM 频率 | 5 kHz |
| 分辨率 | 10 bit（占空比 0~1023） |
| 定时器 / 通道 | LEDC_TIMER_0 / LEDC_CHANNEL_0 |

### 8.2 模式

| 模式 | 说明 | 周期 | PWM 占空比 |
|------|------|------|-----------|
| LED_MODE_OFF | 灭 | - | 0 |
| LED_MODE_ON | 常亮（MQTT 连上） | - | **500 (49%)** |
| LED_MODE_BLINK_SLOW | 慢闪 | 300ms | 1023 |
| LED_MODE_BLINK_FAST | 快闪 | 90ms | 1023 |
| LED_MODE_BLINK_IDLE | 超慢闪（节能态） | 2000ms | 1023 |

### 8.3 状态优先级（resolve_status_mode）

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
```

### 8.4 WiFi/MQTT 状态联动（被调用方 → LED）

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
```

### 8.5 RX/TX 通知脉冲（notify_blink_once）

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

### 8.6 共用定时器设计

`s_status_timer` 同时承担两种职责：
- **正常模式**：`status_timer_cb` → 驱动状态闪烁（根据 `s_blink_step` 奇偶翻亮灭）
- **通知模式**：`s_notify_active=true` → 转发到 `notify_timer_cb` → 执行 160ms 脉冲

这样省下了创建第二个 esp_timer 的开销。

### 8.7 关键变量

```c
static led_mode_t s_status_mode;     // 当前 LED 模式
static led_mode_t s_saved_mode;      // 通知脉冲期间保存的模式
static bool s_notify_active;         // 是否正在执行 RX/TX 通知脉冲
static int s_notify_step;            // 通知脉冲步骤（0→ON, 1→OFF, 2→结束）
static int s_blink_step;             // 闪烁计数器（奇偶决定亮灭）

static bool s_ap_active;             // AP 状态输入
static bool s_sta_connected;         // STA 状态输入
static bool s_mqtt_connected;        // MQTT 状态输入
static bool s_idle_energy_save;      // 配网超时节能态（→ BLINK_IDLE 超慢闪）

static esp_timer_handle_t s_status_timer;  // 唯一定时器（闪烁 + 通知复用）
```

---

## 9. Switch 驱动

| GPIO | 名称 | 说明 |
|------|------|------|
| 5 | SWITCH0 | 开关 0 |
| 12 | SWITCH1 | 开关 1 |
| 8 | SWITCH2 / bit1 | 开关 2 |
| 4 | POWER | 电源控制 |

---

## 10. 温度传感器

- 使用 ESP32-C3 内置温度传感器（ADC 通道）
- `temp_sensor_init()` → 校准
- `temp_sensor_get()` → 返回 float 摄氏度

---

## 11. GPIO 分配总表

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

## 12. 事件总线（跨组件协作）

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
│       ├─ main.c 循环检测到 → 启动 SNTP + MQTT                │
│       │                                                     │
│       └─ MQTT 状态机检测到 → 进入 WAIT_TIME                 │
│                                                             │
│  Web Server 提交新 WiFi                                     │
│       │                                                     │
│       └─ wifi_manager_request_connect() → CMD_CONNECT_NEW   │
│              │                                              │
│              └─ do_runtime_switch() 热切换                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 13. 关键 Timer

| Timer | 周期 | 用途 |
|-------|------|------|
| s_rssi_timer | 5s | WiFi RSSI 周期更新 |
| s_retry_timer | 15s→30s | AP 模式后台重连 WiFi |
| s_ap_auto_close_timer | 60s | 开机 AP 自动关闭节能（有凭证时启动） |
| s_ap_idle_timer | 300s | 配网模式超时关闭（无凭证/恢复出厂时启动） |
| MQTT tick | 任务内 100ms | MQTT 状态机轮询 |

---

## 14. WiFi 连接参数

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

## 15. 已知行为 & 设计决策

1. **AP 常开策略**：即使 STA 成功连上，AP 也不关（APSTA 共存模式），手机随时能连 AP 进配置页
2. **运行时掉线永不放弃**：`s_sta_ever_connected` 标志确保连上过后掉线会无限重试
3. **do_runtime_standalone_ap 不清空 STA 凭证**：只 disconnect，不 `set_config(empty)`，让 retry_timer 能直接用 NVS 里的凭证
4. **热切换不 stop WiFi**：`do_runtime_switch()` 只 disconnect + set_config + connect，AP 和 Web Server 全程不掉线
5. **retry_timer 防护逻辑**：如果 `s_sta_connect_allowed && s_sta_ever_connected` 已经成立（即时重连在工作），定时器只重新 schedule，不重复触发 connect
6. **CMD_OPEN_AP 恢复 STA**：从 STA_ONLY 状态打开 AP 时，自动从 NVS 加载凭证并发起 STA 连接

---

## 16. Flash 分区表（partitions.csv）

芯片：ESP32-C3 | Flash：4 MB

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| nvs | 0x9000 | 24 KB | WiFi 凭证等非易失存储 |
| phy_init | 0xF000 | 4 KB | RF 校准数据 |
| ota_0 | 0x10000 | **1984 KB** | 固件槽位 0（主运行） |
| ota_1 | 0x200000 | **1984 KB** | 固件槽位 1（OTA 备用） |
| ota_data | 0x3F0000 | 64 KB | OTA 状态（双缓冲防掉电） |

### 说明

- 固件实际大小约 1067 KB，每个 OTA 分区 1984 KB，剩余 **917 KB** 给后续功能扩展
- ota_data 从 8 KB 扩大到 64 KB，避免 OTA 过程掉电损坏
- Flash 空间 100% 利用，无浪费
- ⚠️ 改分区表需 `idf.py erase-flash` 全片擦除，NVS 清空