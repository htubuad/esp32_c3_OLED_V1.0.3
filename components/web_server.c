#include "web_server.h"
#include "wifi_manager.h"
#include "led.h"
#include "temp_sensor.h"
#include "mqtt_aliyun.h"
#include "version.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HTTP";
static int64_t s_start_time_ms = 0;
static httpd_handle_t s_server = NULL;

#define HTML_BUF_SIZE  16384

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char uptime_str[12];
    int64_t elapsed_ms = esp_timer_get_time() / 1000 - s_start_time_ms;
    int h = elapsed_ms / 3600000, m = (elapsed_ms % 3600000) / 60000, s = (elapsed_ms % 60000) / 1000;
    snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", h, m, s);

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    const char *device_status = "ESP32-C3 已连接";
    const char *device_state_class = "ok";
    const char *device_state_text = "在线";

    if (wifi_is_ap_active()) {
        device_status = "WiFi 配网模式";
        device_state_class = "warn";
        device_state_text = "配网中";
    } else if (!wifi_is_connected()) {
        device_status = "ESP32-C3 离线";
        device_state_class = "bad";
        device_state_text = "已断开";
    }

    const char *mqtt_text = mqtt_is_connected() ? "正常" : "未连接";
    const char *mqtt_cls = mqtt_is_connected() ? "ok" : "warn";
    const char *led_text = led_get() ? "开" : "关";
    const char *led_cls = led_get() ? "ok" : "bad";
    const char *ip_text = wifi_get_ip()[0] ? wifi_get_ip() : "无";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    char *chunk = (char *)malloc(HTML_BUF_SIZE);
    if (!chunk) return ESP_ERR_NO_MEM;

    int n = snprintf(chunk, HTML_BUF_SIZE,
        "<!DOCTYPE html><html lang='zh-CN'>"
        "<head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32-C3 物联网控制面板</title>"
        "<style>"
        "body{font-family:-apple-system,Segoe UI,sans-serif;margin:0;padding:20px;background:#f0f2f5;color:#333}"
        ".card{max-width:620px;margin:0 auto;background:#fff;border-radius:12px;padding:24px;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
        "h1{margin:0 0 4px;font-size:22px;color:#1a73e8}"
        ".sub{color:#888;font-size:14px;margin-bottom:20px}"
        ".row{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid #eee}"
        ".row:last-child{border:none}"
        ".label{color:#666}.val{font-weight:600}"
        ".ok{color:#2e7d32}.bad{color:#c62828}.warn{color:#ef6c00}"
        ".btn{display:inline-block;padding:12px 28px;margin:8px 8px 0 0;border:none;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;text-decoration:none;color:#fff}"
        ".btn-on{background:#2e7d32}.btn-off{background:#c62828}.btn-reboot{background:#e65100}.btn-wifi{background:#1565c0}"
        ".tab{display:none}.tab.active{display:block}"
        ".tabs{display:flex;gap:4px;margin-bottom:16px;border-bottom:2px solid #eee}"
        ".tab-btn{padding:10px 18px;cursor:pointer;border:none;background:none;font-size:15px;color:#666;border-bottom:2px solid transparent}"
        ".tab-btn.active{color:#1a73e8;border-bottom-color:#1a73e8;font-weight:600}"
        "input,select{width:100%%;padding:10px 12px;margin:6px 0;border:1px solid #ddd;border-radius:6px;font-size:14px;box-sizing:border-box}"
        ".scan-btn{background:#7b1fa2;margin-top:6px}"
        ".wifi-list{max-height:220px;overflow-y:auto;border:1px solid #eee;border-radius:6px;margin-top:8px}"
        ".wifi-item{padding:12px 14px;border-bottom:1px solid #f5f5f5;cursor:pointer;display:flex;justify-content:space-between;font-size:14px;align-items:center}"
        ".wifi-item:hover{background:#f0f7ff}.wifi-item:last-child{border:none}"
        ".wifi-item.selected{background:#e3f2fd;border-left:3px solid #1a73e8}"
        ".loading{color:#999;font-size:14px;padding:16px;text-align:center}"
        ".message{padding:12px 16px;border-radius:8px;margin-top:12px;font-size:14px;display:none}"
        ".message.success{background:#e8f5e9;color:#2e7d32;display:block}"
        ".message.error{background:#ffebee;color:#c62828;display:block}"
        "</style></head><body>"

        "<div class='card'>"
        "<h1>ESP32-C3 物联网控制台</h1>"
        "<div class='sub'>%s</div>"

        "<div class='tabs'>"
        "<button class='tab-btn active' onclick='switchTab(this,\"status\")'>状态</button>"
        "<button class='tab-btn' onclick='switchTab(this,\"wifi\")'>WiFi设置</button>"
        "<button class='tab-btn' onclick='switchTab(this,\"mqtt\")'>MQTT数据</button>"
        "</div>"

        "<div id='tab-status' class='tab active'>"
        "<div class='row'><span class='label'>WiFi</span><span class='val %s'>%s</span></div>"
        "<div class='row'><span class='label'>IP</span><span class='val'>%s</span></div>"
        "<div class='row'><span class='label'>RSSI</span><span class='val'>%d dBm</span></div>"
        "<div class='row'><span class='label'>温度</span><span class='val'>%.1f C</span></div>"
        "<div class='row'><span class='label'>MQTT</span><span class='val %s'>%s</span></div>"
        "<div class='row'><span class='label'>运行时间</span><span class='val'>%s</span></div>"
        "<div class='row'><span class='label'>内存</span><span class='val'>%lu KB</span></div>"
        "<div class='row'><span class='label'>版本</span><span class='val'>%s</span></div>"
        "<div style='margin-top:20px'>"
        "<a class='btn btn-reboot' href='/reboot'>重启</a>"
        "</div>"
        "</div>"

        "<div id='tab-wifi' class='tab'>"
        "<div style='margin-bottom:16px'>"
        "<button class='btn scan-btn' onclick='scanWifi()'>扫描</button>"
        "</div>"
        "<div id='wifi-scan-result' class='wifi-list'></div>"
        "<div><label style='color:#666;font-size:13px'>SSID</label>"
        "<input type='text' id='wifi-ssid' placeholder='选择或输入'></div>"
        "<div><label style='color:#666;font-size:13px'>密码</label>"
        "<input type='password' id='wifi-pass'></div>"
        "<div style='margin-top:16px'>"
        "<button class='btn btn-wifi' onclick='submitWifi()'>保存连接</button>"
        "<button class='btn btn-reboot' onclick='clearWifi()'>清除配网</button>"
        "</div>"
        "<div id='wifi-message' class='message'></div>"
        "</div>"

        "<div id='tab-mqtt' class='tab'>"
        "<div class='row'><span class='label'>温度</span><span class='val' id='val-temp'>--</span></div>"
        "<div class='row'><span class='label'>LED</span><span class='val' id='val-led'>--</span></div>"

        "<h3 style='margin:16px 0 8px;color:#1a73e8;font-size:15px'>最新接收数据</h3>"
        "<div id='msg-lines-box' style='background:#1e1e2e;color:#cdd6f4;border-radius:8px;padding:10px 12px;font-family:monospace;font-size:13px;min-height:60px;max-height:200px;overflow-y:auto;word-break:break-all;white-space:pre-wrap'>暂无数据</div>"

        "<h3 style='margin:16px 0 8px;color:#1a73e8;font-size:15px'>发送 (params)</h3>"
        "<textarea id='mqtt-send-data' rows='3' placeholder='JSON params' style='width:100%%;box-sizing:border-box;font-family:monospace;font-size:13px;padding:8px;border:1px solid #ccc;border-radius:4px;resize:vertical'>{\"LedSwitch\":true,\"temperature\":25.0}</textarea>"
        "<div style='margin-top:12px'>"
        "<button class='btn btn-on' onclick='mqttSend()'>发送到阿里云</button>"
        "<button class='btn' style='background:#888' onclick='resetSendData()'>恢复默认</button>"
        "</div>"
        "<div id='mqtt-send-msg' class='message'></div>"
        "</div>"

        "<script>"
        "var t=null;var _lastSig='';"
        "function switchTab(btn,name){"
        "document.querySelectorAll('.tab').forEach(function(x){x.classList.remove('active')});"
        "document.querySelectorAll('.tab-btn').forEach(function(x){x.classList.remove('active')});"
        "document.getElementById('tab-'+name).classList.add('active');"
        "if(btn)btn.classList.add('active');"
        "location.hash=name;"
        "if(name==='mqtt'){startRefresh();}else{stopRefresh();}"
        "}"
        "function startRefresh(){if(t)return;t=setInterval(refresh,1000);refresh();}"
        "function stopRefresh(){if(t){clearInterval(t);t=null;}}"
        "function refresh(){fetch('/api/mqtt/data',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){var t=document.getElementById('val-temp');var l=document.getElementById('val-led');if(t)t.textContent=j.temp!=null?j.temp.toFixed(1)+' C':'--';if(l){l.textContent=j.led?'开':'关';l.className='val '+(j.led?'ok':'bad');}var box=document.getElementById('msg-lines-box');if(box){var h=j.history||[];var html='';if(!h.length){html='暂无数据';}else{var e=h[0];var tp=(e.topic||'').split('/');var name=tp[tp.length-1]||'topic';html+='<div style=\"background:#313244;border-radius:4px;padding:6px 8px;margin-bottom:6px\"><div style=\"color:#89b4fa;font-weight:700;font-size:14px\">'+name+'</div><div style=\"color:#f9e2af;word-break:break-all\">'+(e.data||'')+'</div></div>';for(var i=1;i<h.length;i++){var x=h[i];var tp2=(x.topic||'').split('/');var n2=tp2[tp2.length-1]||'topic';html+='<div style=\"padding:2px 0;color:#6c7086;font-size:12px;border-bottom:1px dashed #45475a\"><span>'+n2+'</span> <span style=\"color:#a6adc8\">'+(x.data||'')+'</span></div>';}}var curSig=h.length?(h[0].topic||'')+'|'+(h[0].data||''):'';if(curSig!==_lastSig){box.innerHTML=html;box.scrollTop=0;}_lastSig=curSig;}}).catch(function(){});}"
        "(function(){var h=location.hash.replace('#','');if(h){var b=document.querySelector('.tab-btn[onclick*=\"'+h+'\"]');switchTab(b,h);}})();"
        "function resetSendData(){document.getElementById('mqtt-send-data').value='{\"LedSwitch\":true,\"temperature\":25.0}';}"
        "async function mqttSend(){"
        "var d=document.getElementById('mqtt-send-data').value.trim();"
        "var m=document.getElementById('mqtt-send-msg');"
        "if(!d){m.className='message error';m.textContent='请输入数据';return;}"
        "m.className='message';m.textContent='发送中...';m.style.display='block';"
        "try{"
        "var r=await fetch('/api/mqtt/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'data='+encodeURIComponent(d)});"
        "var j=await r.json();"
        "if(j.success){m.className='message success';m.textContent='发送成功';}"
        "else{m.className='message error';m.textContent='失败: '+j.error;}"
        "}catch(e){m.className='message error';m.textContent='网络错误';}"
        "}"
        "async function scanWifi(){"
        "var l=document.getElementById('wifi-scan-result');"
        "l.innerHTML='<div class=\"loading\">扫描中...</div>';"
        "try{var r=await fetch('/api/wifi/scan');var j=await r.json();"
        "var h='';"
        "if(!j.aps||j.aps.length===0){h='<div class=\"loading\">未发现</div>';}"
        "else{j.aps.sort(function(a,b){return b.rssi-a.rssi;});"
        "j.aps.forEach(function(a){var s=a.rssi>-60?'强':a.rssi>-75?'中':'弱';"
        "h+='<div class=\"wifi-item\" onclick=\"selWifi(this,\\''+a.ssid+'\\')\">';"
        "h+='<span>'+(a.auth>0?'&#128274;':'&#128275;')+' '+a.ssid+'</span><span style=\"color:#999;font-size:12px\">'+s+'</span></div>';});}"
        "l.innerHTML=h;"
        "}catch(e){l.innerHTML='<div class=\"loading\">失败</div>';}}"
        "function selWifi(el,s){document.getElementById('wifi-ssid').value=s;document.querySelectorAll('.wifi-item').forEach(function(i){i.classList.remove('selected')});el.classList.add('selected');}"
        "async function submitWifi(){"
        "var s=document.getElementById('wifi-ssid').value.trim();"
        "var p=document.getElementById('wifi-pass').value;"
        "var m=document.getElementById('wifi-message');"
        "if(!s){m.className='message error';m.textContent='请输入SSID';return;}"
        "m.className='message';m.textContent='连接中...';m.style.display='block';"
        "try{"
        "var r=await fetch('/api/wifi/configure',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent(p)});"
        "var j=await r.json();"
        "if(j.success&&j.connecting){m.className='message success';m.innerHTML='连接中...<br>新IP: '+j.ip+'<br>请切换到路由器WiFi';}"
        "else if(j.success){m.className='message success';m.innerHTML='已保存';}"
        "else{m.className='message error';m.textContent='失败: '+j.error;}"
        "}catch(e){m.className='message error';m.textContent='网络错误';}"
        "}"
        "async function clearWifi(){"
        "if(!confirm('确定清除WiFi？'))return;"
        "try{var r=await fetch('/api/wifi/clear',{method:'POST'});var j=await r.json();"
        "if(j.success){alert('已清除！\\n热点: '+j.ap_ssid+'\\nIP: '+j.ap_ip);location.reload();}}"
        "catch(e){alert('网络错误');}}"
        "</script>"
        "</div></body></html>",

        device_status,
        device_state_class, device_state_text,
        ip_text,
        wifi_is_connected() ? wifi_get_rssi() : 0,
        temp_sensor_get(),
        mqtt_cls, mqtt_text,
        uptime_str,
        (unsigned long)(free_heap / 1024),
        APP_VERSION
    );

    if (n < 0) { free(chunk); return ESP_FAIL; }
    if (n >= HTML_BUF_SIZE) n = HTML_BUF_SIZE - 1;

    httpd_resp_send(req, chunk, n);
    free(chunk);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_get_ip());
    cJSON_AddBoolToObject(root, "mqtt_connected", mqtt_is_connected());
    cJSON_AddNumberToObject(root, "temp", temp_sensor_get());
    cJSON_AddBoolToObject(root, "led", led_get());
    cJSON_AddNumberToObject(root, "rssi", wifi_get_rssi());
    cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, -1);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t led_post_handler(httpd_req_t *req)
{
    char buf[32] = {0};
    httpd_req_get_url_query_str(req, buf, sizeof(buf));

    bool on;
    if (strstr(buf, "action=on")) on = true;
    else if (strstr(buf, "action=off")) on = false;
    else return ESP_OK;

    led_set(on);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "led", led_get());

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, -1);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t reboot_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<!DOCTYPE html><html><body><h2>重启中...</h2><script>setTimeout(function(){location.href='/';},3000);</script></body></html>", -1);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    static wifi_ap_info_t aps[WIFI_MAX_AP_COUNT];
    uint16_t count = WIFI_MAX_AP_COUNT;
    esp_err_t err = wifi_scan_aps(aps, &count);

    cJSON *root = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < count; i++) {
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
            cJSON_AddNumberToObject(ap, "auth", aps[i].auth);
            cJSON_AddItemToArray(arr, ap);
        }
        cJSON_AddItemToObject(root, "aps", arr);
    } else {
        cJSON_AddArrayToObject(root, "aps");
    }

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, -1);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t wifi_configure_handler(httpd_req_t *req)
{
    char buf[256];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) return ESP_FAIL;
    buf[n] = '\0';

    char *ssid = NULL, *password = NULL;
    char *tok = strtok(buf, "&");
    while (tok) {
        if (strncmp(tok, "ssid=", 5) == 0) ssid = tok + 5;
        else if (strncmp(tok, "password=", 9) == 0) password = tok + 9;
        tok = strtok(NULL, "&");
    }
    if (!password) password = "";

    wifi_cred_t cred;
    snprintf(cred.ssid, sizeof(cred.ssid), "%s", ssid);
    snprintf(cred.password, sizeof(cred.password), "%s", password);
    wifi_cred_save(&cred);

    esp_err_t err = wifi_try_connect(ssid, password);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    cJSON_AddBoolToObject(root, "connecting", err == ESP_OK);
    cJSON_AddStringToObject(root, "ip", wifi_get_ip());
    if (err != ESP_OK) cJSON_AddStringToObject(root, "error", "connect failed");

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, -1);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t wifi_clear_handler(httpd_req_t *req)
{
    wifi_cred_clear();
    wifi_start_ap("ESP32-C3-Setup");

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "ap_ssid", "ESP32-C3-Setup");
    cJSON_AddStringToObject(root, "ap_ip", "192.168.4.1");

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, -1);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t mqtt_data_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temp", temp_sensor_get());
    cJSON_AddBoolToObject(root, "led", led_get());
    cJSON_AddStringToObject(root, "last_topic", mqtt_get_last_rx_topic());
    cJSON_AddStringToObject(root, "last_data", mqtt_get_last_rx_data());

    mqtt_rx_entry_t history[5];
    int hcnt = mqtt_get_rx_entries(history, 5);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < hcnt; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "topic", history[i].topic);
        cJSON_AddStringToObject(item, "data", history[i].data);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "history", arr);

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, -1);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t err_404_handler(httpd_req_t *req, httpd_err_code_t error)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t mqtt_send_handler(httpd_req_t *req)
{
    char buf[512];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) return ESP_FAIL;
    buf[n] = '\0';

    char *data = NULL;
    if (strncmp(buf, "data=", 5) == 0) data = buf + 5;
    else data = buf;

    esp_err_t err = mqtt_publish_aliyun_params(data);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    if (err != ESP_OK) cJSON_AddStringToObject(root, "error", "send failed");

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, -1);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

void start_webserver(int64_t start_time_ms)
{
    s_start_time_ms = start_time_ms;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 16;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start webserver");
        return;
    }

    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t uri_status = {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t uri_led = {.uri = "/led", .method = HTTP_ANY, .handler = led_post_handler};
    httpd_uri_t uri_reboot = {.uri = "/reboot", .method = HTTP_GET, .handler = reboot_get_handler};

    httpd_uri_t uri_wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler};
    httpd_uri_t uri_wifi_cfg = {.uri = "/api/wifi/configure", .method = HTTP_POST, .handler = wifi_configure_handler};
    httpd_uri_t uri_wifi_clr = {.uri = "/api/wifi/clear", .method = HTTP_POST, .handler = wifi_clear_handler};

    httpd_uri_t uri_mqtt_data = {.uri = "/api/mqtt/data", .method = HTTP_GET, .handler = mqtt_data_handler};
    httpd_uri_t uri_mqtt_send = {.uri = "/api/mqtt/send", .method = HTTP_POST, .handler = mqtt_send_handler};

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_led);
    httpd_register_uri_handler(s_server, &uri_reboot);
    httpd_register_uri_handler(s_server, &uri_wifi_scan);
    httpd_register_uri_handler(s_server, &uri_wifi_cfg);
    httpd_register_uri_handler(s_server, &uri_wifi_clr);
    httpd_register_uri_handler(s_server, &uri_mqtt_data);
    httpd_register_uri_handler(s_server, &uri_mqtt_send);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, err_404_handler);

    ESP_LOGI(TAG, "Web server started");
}

void stop_webserver(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}