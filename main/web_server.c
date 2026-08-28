// On-device web UI: Daikin Data, WiFi, Config and OTA tabs.
//
// Not ported from upstream ESPAltherma - it has no web UI. The structure
// follows the BirdBox convention: one page assembled from C string literals, a
// handful of /api endpoints returning JSON, and a raw-body OTA upload.
//
// The page uses single quotes for HTML attributes throughout, so the C string
// literals need no backslash escaping and stay readable.

#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "althermaserial.h"
#include "converters.h"
#include "mqtt.h"
#include "settings.h"
#include "version.h"
#include "wifi.h"

static const char *TAG = "web";

static httpd_handle_t s_server;

// ------------------------------------------------------------------ the page

static const char PAGE[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>AlthermaInterface</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#14161a;color:#e6e6e6}"
"header{padding:14px 18px;background:#1c1f26;border-bottom:1px solid #2a2f39}"
"h1{margin:0;font-size:17px;font-weight:600}"
"h1 span{font-weight:400;color:#8b93a1;font-size:13px;margin-left:8px}"
"nav{display:flex;gap:2px;background:#1c1f26;padding:0 12px;border-bottom:1px solid #2a2f39;flex-wrap:wrap}"
"nav button{background:none;border:0;color:#8b93a1;padding:11px 15px;font-size:14px;cursor:pointer;border-bottom:2px solid transparent}"
"nav button.on{color:#5ec8f2;border-bottom-color:#5ec8f2}"
"main{padding:18px;max-width:900px}"
"section{display:none}section.on{display:block}"
"table{border-collapse:collapse;width:100%;font-size:14px}"
"td,th{padding:7px 10px;border-bottom:1px solid #262b34;text-align:left}"
"th{color:#8b93a1;font-weight:500;font-size:12px;text-transform:uppercase}"
"td.v{text-align:right;font-variant-numeric:tabular-nums;color:#fff}"
"td.r{color:#5c6472;font-size:12px;width:1%;white-space:nowrap}"
"label{display:block;margin:14px 0 4px;font-size:13px;color:#8b93a1}"
"input{width:100%;max-width:380px;padding:8px 10px;background:#1c1f26;border:1px solid #333a46;"
"border-radius:4px;color:#e6e6e6;font-size:14px;box-sizing:border-box}"
"button.go{margin-top:18px;background:#2b6cb0;color:#fff;border:0;padding:9px 18px;"
"border-radius:4px;font-size:14px;cursor:pointer}"
"button.go:hover{background:#3182ce}"
".msg{margin-top:12px;font-size:13px;color:#68d391;min-height:18px}"
".msg.err{color:#fc8181}"
".hint{color:#5c6472;font-size:12px;margin-top:6px}"
"progress{width:100%;max-width:380px;height:6px;margin-top:12px}"
"</style></head><body>"
"<header><h1>AlthermaInterface<span id='hv'></span></h1></header>"
"<nav>"
"<button class='on' onclick='tab(0,this)'>Daikin Data</button>"
"<button onclick='tab(1,this)'>WiFi</button>"
"<button onclick='tab(2,this)'>Config</button>"
"<button onclick='tab(3,this)'>OTA</button>"
"</nav><main>"

"<section class='on'><table><thead><tr><th>Reg</th><th>Value</th><th class='v'>Reading</th></tr>"
"</thead><tbody id='vals'><tr><td colspan='3'>loading...</td></tr></tbody></table>"
"<p class='hint' id='vhint'></p>"
"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>X10A LINK</h3>"
"<p class='hint' id='rxl'></p>"
"<table><thead><tr><th>Reg</th><th>Proto</th><th>Last result</th><th>OK/fail</th><th>Raw bytes</th></tr>"
"</thead><tbody id='diag'><tr><td colspan='5'>no queries yet</td></tr></tbody></table>"
"<button class='go' onclick='rxcheck()'>Re-check RX line</button> "
"<button class='go' onclick='probe()'>Probe both protocols</button>"
"<p class='msg' id='pmsg'></p>"
"<p class='hint'>Re-check RX reads the level on GPIO16 right now, so a wire can "
"be moved and re-tested without power-cycling the pump. An idle transmitter "
"holds its line high; a low reading means nothing is driving it.</p>"
"<p class='hint'>Asks protocol I on 0x10/0x20/0x21/0x60/0x61 and protocol S on "
"0x50/0x53/0x54/0x55/0x56. Whichever dialect the machine speaks will answer. "
"Takes about 25 s; watch the table above.</p>"

"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>"
"REGISTER SCAN</h3>"
"<button class='go' onclick='scan()'>Scan all 256 registers</button>"
"<progress id='sp' value='0' max='256' style='display:none'></progress>"
"<p class='msg' id='smsg'></p>"
"<table><tbody id='shits'></tbody></table>"
"<p class='hint'>Asks every registry ID 0x00-0xFF on the active protocol and "
"reports which ones this machine implements. Protocol S is barely documented - "
"upstream's 40 definition files use only five 0x5x registers between them and "
"no full scan has been published - so anything found here beyond 0x53-0x56 is "
"new. Read-only, and takes about 2 minutes.</p>"
"</section>"

"<section><table id='wifi'></table></section>"

"<section>"
"<label>Broker URI</label><input id='uri' placeholder='mqtt://192.168.1.4:1883'>"
"<p class='hint'>mqtt:// for plain TCP, mqtts:// for TLS.</p>"
"<label>Username</label><input id='usr' autocomplete='off'>"
"<label>Password</label><input id='pwd' type='password' autocomplete='new-password'>"
"<p class='hint' id='pwdh'></p>"
"<button class='go' onclick='save()'>Save and reboot</button>"
"<p class='msg' id='cmsg'></p></section>"

"<section>"
"<label>Firmware image (build/AlthermaInterface.bin)</label>"
"<input id='fw' type='file' accept='.bin'>"
"<button class='go' onclick='ota()'>Upload and reboot</button>"
"<progress id='pg' value='0' max='100' style='display:none'></progress>"
"<p class='msg' id='omsg'></p>"
"<p class='hint'>Dual OTA slots with rollback: an image that fails to boot is "
"reverted automatically on the next start.</p></section>"

"</main><script>"
"function tab(i,b){"
"document.querySelectorAll('section').forEach((s,n)=>s.className=n==i?'on':'');"
"document.querySelectorAll('nav button').forEach(x=>x.className='');b.className='on';}"
"function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}"
"function row(k,v){return '<tr><td>'+esc(k)+'</td><td class=\"v\">'+esc(v)+'</td></tr>';}"
"async function load(){"
"try{"
"var s=await (await fetch('/api/status')).json();"
"document.getElementById('hv').textContent='v'+s.version+' - '+s.ip;"
"document.getElementById('wifi').innerHTML="
"row('SSID',s.ssid)+row('IP address',s.ip)+row('RSSI',s.rssi+' dBm')+"
"row('Channel',s.channel)+row('BSSID',s.bssid)+row('WiFi',s.wifi?'connected':'down')+"
"row('MQTT',s.mqtt?'connected':'down')+row('Broker',s.broker)+"
"row('Protocol',s.protocol)+row('Refrigerant',s.refrigerant)+"
"row('OTA slot',s.partition)+row('Reset reason',s.resetReason)+row('Free heap',s.heap+' bytes')+row('Uptime',s.uptime+' s')+row('Firmware',s.version);"
"var v=await (await fetch('/api/values')).json();"
"document.getElementById('vals').innerHTML=v.values.length?v.values.map(x=>"
"'<tr><td class=\"r\">'+esc(x.reg)+'</td><td>'+esc(x.label)+'</td><td class=\"v\">'+esc(x.value)+'</td></tr>'"
").join(''):'<tr><td colspan=\"3\">no values read yet</td></tr>';"
"document.getElementById('vhint').textContent="
"v.values.length+' of '+v.total+' labels have been read at least once.';"
"var d=await (await fetch('/api/x10a')).json();"
"document.getElementById('rxl').textContent='RX line ('+d.rxWhen+'): '+d.rxIdle;"
"document.getElementById('diag').innerHTML=d.registries.length?d.registries.map(x=>"
"'<tr><td class=\"r\">'+esc(x.reg)+'</td><td>'+esc(x.proto)+'</td><td>'+esc(x.status)+"
"'</td><td>'+x.ok+'/'+x.fail+'</td><td class=\"r\">'+esc(x.raw||'-')+'</td></tr>'"
").join(''):'<tr><td colspan=\"5\">no queries yet</td></tr>';"
"}catch(e){}}"
"async function rxcheck(){var m=document.getElementById('pmsg');m.className='msg';"
"m.textContent='sampling RX...';"
"try{var r=await (await fetch('/api/rxcheck',{method:'POST'})).json();"
"m.textContent='RX line now: '+r.rxIdle;load();}"
"catch(e){m.className='msg err';m.textContent='Failed: '+e;}}"
"async function probe(){var m=document.getElementById('pmsg');m.className='msg';"
"m.textContent='probing both protocols, ~25 s...';"
"try{await fetch('/api/probe',{method:'POST'});setTimeout(load,26000);}"
"catch(e){m.className='msg err';m.textContent='Failed: '+e;}}"
"async function scanpoll(){"
"var r=await (await fetch('/api/scan')).json();"
"var p=document.getElementById('sp');var m=document.getElementById('smsg');"
"p.style.display='block';p.value=r.progress;"
"m.textContent=(r.running?'scanning ':'done: ')+r.progress+'/256 - '+r.ok+' ok, '"
"+r.notImpl+' not implemented, '+r.badCrc+' bad CRC, '+r.silent+' silent';"
"document.getElementById('shits').innerHTML=r.hits.map(x=>"
"'<tr><td class=\"r\">'+esc(x.reg)+'</td><td>'+esc(x.outcome)+'</td><td>'+x.bytes+"
"' B</td><td class=\"r\">'+esc(x.raw)+'</td></tr>').join('');"
"if(r.running)setTimeout(scanpoll,3000);}"
"async function scan(){var m=document.getElementById('smsg');m.className='msg';"
"m.textContent='starting...';"
"try{await fetch('/api/scan',{method:'POST'});setTimeout(scanpoll,1500);}"
"catch(e){m.className='msg err';m.textContent='Failed: '+e;}}"
"async function cfg(){var c=await (await fetch('/api/config')).json();"
"document.getElementById('uri').value=c.uri;document.getElementById('usr').value=c.user;"
"document.getElementById('pwdh').textContent=c.passSet?"
"'A password is stored. Leave blank to keep it.':'No password stored.';}"
"async function save(){var m=document.getElementById('cmsg');m.className='msg';m.textContent='saving...';"
"var b={uri:document.getElementById('uri').value,user:document.getElementById('usr').value,"
"pass:document.getElementById('pwd').value};"
"try{var r=await fetch('/api/config',{method:'POST',body:JSON.stringify(b)});"
"m.textContent=r.ok?'Saved. Rebooting...':'Failed: '+await r.text();"
"if(!r.ok)m.className='msg err';}catch(e){m.className='msg err';m.textContent='Failed: '+e;}}"
"function ota(){var f=document.getElementById('fw').files[0];var m=document.getElementById('omsg');"
"var p=document.getElementById('pg');m.className='msg';"
"if(!f){m.className='msg err';m.textContent='Choose a .bin first.';return;}"
"p.style.display='block';m.textContent='uploading '+f.size+' bytes...';"
"var x=new XMLHttpRequest();x.open('POST','/ota/upload');"
"x.upload.onprogress=e=>{if(e.lengthComputable)p.value=100*e.loaded/e.total;};"
"x.onload=()=>{if(x.status==200){m.textContent='Flashed. Rebooting...';}"
"else{m.className='msg err';m.textContent='Failed: '+x.responseText;}};"
"x.onerror=()=>{m.className='msg err';m.textContent='Upload failed.';};"
"x.setRequestHeader('Content-Type','application/octet-stream');x.send(f);}"
"load();cfg();setInterval(load,5000);"
"</script></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

// ------------------------------------------------------------------- /api

static esp_err_t status_get(httpd_req_t *req)
{
    char ip[16];
    char bssid[20];
    alt_wifi_ip(ip, sizeof(ip));
    alt_wifi_bssid(bssid, sizeof(bssid));

    // Which OTA slot is running. The bootloader logs this too, but
    // CONFIG_BOOTLOADER_LOG_LEVEL_WARN suppresses that line, and after an OTA
    // "did the slot actually change" is exactly the question worth answering.
    const esp_partition_t *run = esp_ota_get_running_partition();

    char body[700];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"channel\":%d,\"bssid\":\"%s\",\"wifi\":%s,\"mqtt\":%s,"
             "\"broker\":\"%s\",\"protocol\":\"%c\",\"refrigerant\":%d,"
             "\"partition\":\"%s\",\"resetReason\":%d,\"heap\":%u,\"uptime\":%lld}",
             FIRMWARE_VERSION, alt_wifi_ssid(), ip, alt_wifi_rssi(),
             alt_wifi_channel(), bssid,
             alt_wifi_is_connected() ? "true" : "false",
             alt_mqtt_is_connected() ? "true" : "false",
             alt_settings_mqtt_uri(), converter_protocol(), converter_refrigerant(),
             run ? run->label : "?",
             (int)esp_reset_reason(),
             (unsigned)esp_get_free_heap_size(),
             esp_timer_get_time() / 1000000);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t values_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    size_t total = converter_label_count();
    char chunk[256];

    snprintf(chunk, sizeof(chunk), "{\"total\":%u,\"values\":[", (unsigned)total);
    httpd_resp_sendstr_chunk(req, chunk);

    // Chunked rather than one buffer: a protocol-I machine has hundreds of
    // labels, and this handler should not care.
    bool first = true;
    for (size_t i = 0; i < total; i++) {
        uint8_t reg;
        const char *label;
        const char *value;
        if (!converter_label_at(i, &reg, &label, &value) || value[0] == '\0') {
            continue;
        }
        snprintf(chunk, sizeof(chunk),
                 "%s{\"reg\":\"0x%02x\",\"label\":\"%s\",\"value\":\"%s\"}",
                 first ? "" : ",", reg, label, value);
        httpd_resp_sendstr_chunk(req, chunk);
        first = false;
    }

    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Per-registry link state. The unit runs off X10A power with no USB attached,
// so this is the only way to see why a registry is not decoding.
static esp_err_t x10a_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char head[128];
    snprintf(head, sizeof(head), "{\"rxIdle\":\"%s\",\"rxWhen\":\"%s\",\"probing\":%s,\"registries\":[",
             alt_rx_idle_state(), alt_rx_idle_when(), alt_probe_running() ? "true" : "false");
    httpd_resp_sendstr_chunk(req, head);

    char chunk[420];
    size_t n = alt_diag_count();
    for (size_t i = 0; i < n; i++) {
        uint8_t reg = 0;
        const char *status = "";
        const char *hex = "";
        int bytes = 0;
        char proto = 0;
        uint32_t ok = 0, fail = 0;
        if (!alt_diag_at(i, &reg, &proto, &status, &hex, &bytes, &ok, &fail)) {
            continue;
        }
        snprintf(chunk, sizeof(chunk),
                 "%s{\"reg\":\"0x%02x\",\"proto\":\"%c\",\"status\":\"%s\",\"bytes\":%d,"
                 "\"ok\":%u,\"fail\":%u,\"raw\":\"%s\"}",
                 i ? "," : "", reg, proto ? proto : 63, status, bytes,
                 (unsigned)ok, (unsigned)fail, hex);
        httpd_resp_sendstr_chunk(req, chunk);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Asks BOTH protocols across their known registries and records every result,
// so "is this machine protocol I or S" is answerable from the web UI.
static esp_err_t probe_post(httpd_req_t *req)
{
    alt_probe_start();
    httpd_resp_sendstr(req, "started");
    return ESP_OK;
}

// Walks every registry ID 0x00-0xFF, to find out what this machine actually
// implements rather than what someone else's did. Protocol S is barely
// documented: upstream's 40 definition files use only five 0x5x registries
// between them, and no full scan has been published.
static esp_err_t scan_post(httpd_req_t *req)
{
    alt_scan_start(converter_protocol());
    httpd_resp_sendstr(req, "started");
    return ESP_OK;
}

static const char *scan_outcome_name(alt_scan_outcome_t o)
{
    switch (o) {
    case ALT_SCAN_OK:       return "ok";
    case ALT_SCAN_NOT_IMPL: return "not implemented";
    case ALT_SCAN_BAD_CRC:  return "replied, bad CRC";
    case ALT_SCAN_SILENT:   return "silent";
    default:                return "untested";
    }
}

static esp_err_t scan_get(httpd_req_t *req)
{
    int ok = 0, ni = 0, bad = 0, sil = 0;
    alt_scan_totals(&ok, &ni, &bad, &sil);

    httpd_resp_set_type(req, "application/json");

    char chunk[320];
    snprintf(chunk, sizeof(chunk),
             "{\"running\":%s,\"progress\":%d,\"ok\":%d,\"notImpl\":%d,"
             "\"badCrc\":%d,\"silent\":%d,\"hits\":[",
             alt_scan_running() ? "true" : "false", alt_scan_progress(),
             ok, ni, bad, sil);
    httpd_resp_sendstr_chunk(req, chunk);

    size_t n = alt_scan_hit_count();
    for (size_t i = 0; i < n; i++) {
        uint8_t reg = 0;
        const char *hex = "";
        int bytes = 0;
        alt_scan_outcome_t outcome = ALT_SCAN_UNTESTED;
        if (!alt_scan_hit_at(i, &reg, &hex, &bytes, &outcome)) {
            continue;
        }
        snprintf(chunk, sizeof(chunk),
                 "%s{\"reg\":\"0x%02x\",\"outcome\":\"%s\",\"bytes\":%d,\"raw\":\"%s\"}",
                 i ? "," : "", reg, scan_outcome_name(outcome), bytes, hex);
        httpd_resp_sendstr_chunk(req, chunk);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Re-reads the RX pin level now, so a wire can be moved and re-checked without
// power-cycling the heat pump to get a fresh boot-time sample.
static esp_err_t rxcheck_post(httpd_req_t *req)
{
    alt_resample_rx();
    httpd_resp_set_type(req, "application/json");
    char body[128];
    snprintf(body, sizeof(body), "{\"rxIdle\":\"%s\",\"rxWhen\":\"%s\"}",
             alt_rx_idle_state(), alt_rx_idle_when());
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get(httpd_req_t *req)
{
    char body[320];
    // The stored password is deliberately never sent to the browser; the UI
    // only learns whether one exists.
    snprintf(body, sizeof(body), "{\"uri\":\"%s\",\"user\":\"%s\",\"passSet\":%s}",
             alt_settings_mqtt_uri(), alt_settings_mqtt_user(),
             alt_settings_mqtt_pass_set() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// Minimal extractor for the three flat string fields the Config tab posts.
// Not a general JSON parser, and does not need to be.
static bool json_field(const char *body, const char *key, char *out, size_t len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(body, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < len) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

static void reboot_task(void *arg)
{
    (void)arg;
    // Long enough for the HTTP response to reach the browser.
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t config_post(httpd_req_t *req)
{
    char body[512];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[got] = '\0';

    char uri[ALT_SETTING_MAX] = {0};
    char user[ALT_SETTING_MAX] = {0};
    char pass[ALT_SETTING_MAX] = {0};

    if (!json_field(body, "uri", uri, sizeof(uri)) || uri[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "uri required");
        return ESP_FAIL;
    }
    json_field(body, "user", user, sizeof(user));
    bool has_pass = json_field(body, "pass", pass, sizeof(pass)) && pass[0] != '\0';

    // A blank password field means "keep what is stored", so the user does not
    // have to retype it to change the broker address.
    esp_err_t err = alt_settings_set_mqtt(uri, user, has_pass ? pass : NULL);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    // Reboot rather than rebuilding the MQTT client in place: settings are read
    // once at start-up, and a restart is the one path guaranteed to be
    // consistent. It costs a few seconds on a device that polls every 30.
    ESP_LOGI(TAG, "config saved, rebooting");
    xTaskCreate(&reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// -------------------------------------------------------------------- OTA

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA -> %s, %d bytes", target->label, req->content_len);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload truncated");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }
        remaining -= got;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    ESP_LOGI(TAG, "OTA written, rebooting into %s", target->label);
    xTaskCreate(&reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ------------------------------------------------------------------- start

esp_err_t alt_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;      // headroom; registrations past the cap 404 silently
    cfg.stack_size = 8192;          // OTA handler needs more than the default
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 20;     // a large OTA body must not trip the default
    cfg.send_wait_timeout = 20;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/",            .method = HTTP_GET,  .handler = root_get},
        {.uri = "/api/status",  .method = HTTP_GET,  .handler = status_get},
        {.uri = "/api/values",  .method = HTTP_GET,  .handler = values_get},
        {.uri = "/api/x10a",    .method = HTTP_GET,  .handler = x10a_get},
        {.uri = "/api/probe",   .method = HTTP_POST, .handler = probe_post},
        {.uri = "/api/rxcheck", .method = HTTP_POST, .handler = rxcheck_post},
        {.uri = "/api/scan",    .method = HTTP_POST, .handler = scan_post},
        {.uri = "/api/scan",    .method = HTTP_GET,  .handler = scan_get},
        {.uri = "/api/config",  .method = HTTP_GET,  .handler = config_get},
        {.uri = "/api/config",  .method = HTTP_POST, .handler = config_post},
        {.uri = "/ota/upload",  .method = HTTP_POST, .handler = ota_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }

    char ip[16];
    alt_wifi_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "web UI on http://%s/", ip);
    return ESP_OK;
}
