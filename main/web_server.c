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
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
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
"<button onclick='tab(1,this)'>Debug</button>"
"<button onclick='tab(2,this)'>Config</button>"
"<button onclick='tab(3,this)'>OTA</button>"
"</nav><main>"

"<section class='on'><table><thead><tr><th>Reg</th><th>Value</th><th class='v'>Reading</th></tr>"
"</thead><tbody id='vals'><tr><td colspan='3'>loading...</td></tr></tbody></table>"
"<p class='hint' id='vhint'></p>"
"</section>"

"<section>"
"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:0 0 8px'>X10A LINK</h3>"
"<p class='hint' id='rxl'></p>"
"<table><thead><tr><th>Reg</th><th>Proto</th><th>Last result</th><th>OK/fail</th><th>Raw bytes</th></tr>"
"</thead><tbody id='diag'><tr><td colspan='5'>no queries yet</td></tr></tbody></table>"
"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>WIFI</h3>"
"<table id='wifi'></table>"
"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>MQTT</h3>"
"<table id='mq'></table>"
"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>DEVICE</h3>"
"<table id='dev'></table>"
"</section>"

"<section>"
"<label>Broker URI</label><input id='uri' placeholder='mqtt://192.168.1.4:1883'>"
"<p class='hint'>mqtt:// for plain TCP, mqtts:// for TLS.</p>"
"<label>Username</label><input id='usr' autocomplete='off'>"
"<label>Password</label><input id='pwd' type='password' autocomplete='new-password'>"
"<p class='hint' id='pwdh'></p>"

"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>"
"X10A PINS</h3>"
"<label>RX (GPIO) &mdash; to the X10A TX pin</label>"
"<input id='rxp' type='number' min='0' max='39' style='max-width:120px'>"
"<label>TX (GPIO) &mdash; to the X10A RX pin</label>"
"<input id='txp' type='number' min='0' max='39' style='max-width:120px'>"
"<p class='hint'>Defaults are RX 16 / TX 15. GPIO6-11 are the SPI flash and are "
"refused; GPIO34-39 are input-only so cannot be TX. A wrong pin costs the heat "
"pump link but not the device &mdash; WiFi and this page still come up, so it "
"can be corrected from here.</p>"

"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>"
"FIRMWARE SOURCE</h3>"
"<label>GitHub repository (owner/name)</label><input id='ghr'>"
"<p class='hint'>Releases of this repository are the only images the device "
"will download and flash.</p>"

"<button class='go' onclick='save()'>Save and reboot</button>"
"<p class='msg' id='cmsg'></p></section>"

"<section>"
"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:0 0 8px'>"
"FIRMWARE UPDATE</h3>"
"<p class='hint' id='rv'></p>"
"<p class='hint'>Upload an AlthermaInterface-vX.Y.Z.bin release. The device "
"writes it to the inactive slot and boots straight into it; if the new image "
"never starts cleanly, the previous version is restored automatically at the "
"next boot (dual OTA partitions, FSD &sect;9) &mdash; a bad upload cannot brick "
"the device. Do not cut power mid-upload.</p>"
"<input id='fw' type='file' accept='.bin'>"
"<button class='go' onclick='ota()'>Upload and flash</button>"
"<progress id='pg' value='0' max='100' style='display:none'></progress>"
"<p class='msg' id='omsg'></p>"

"<h3 style='font-size:13px;color:#8b93a1;font-weight:500;margin:26px 0 8px'>"
"FLASH FROM GITHUB RELEASE</h3>"
"<p class='hint'>Fetch a published release directly &mdash; the device downloads "
"and flashes it itself over HTTPS, no PC needed. Locked to this project's "
"releases; same dual-partition rollback as a manual upload.</p>"
"<select id='ghRel' style='max-width:380px;padding:8px;background:#1c1f26;"
"border:1px solid #333a46;border-radius:4px;color:#e6e6e6'>"
"<option>Loading releases&hellip;</option></select> "
"<button class='go' onclick='ghLoad()'>&#8635; Reload</button> "
"<button class='go' onclick='ghFlash()'>&#8681; Download and flash</button>"
"<progress id='gpg' value='0' max='100' style='display:none'></progress>"
"<p class='msg' id='gmsg'></p></section>"

"</main><script>"
"var RUNNING='';"
"function tab(i,b){"
"document.querySelectorAll('section').forEach((s,n)=>s.className=n==i?'on':'');"
"document.querySelectorAll('nav button').forEach(x=>x.className='');b.className='on';}"
"function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}"
"function row(k,v){return '<tr><td>'+esc(k)+'</td><td class=\"v\">'+esc(v)+'</td></tr>';}"
"async function load(){"
"try{"
"var s=await (await fetch('/api/status')).json();"
"document.getElementById('hv').textContent='v'+s.version+' - '+s.ip;RUNNING=s.version;"
"document.getElementById('rv').textContent='Running version: v'+s.version+'  (slot '+s.partition+')';"
"document.getElementById('wifi').innerHTML="
"row('Status',s.wifi?'connected':'down')+row('SSID',s.ssid)+row('IP address',s.ip)+"
"row('RSSI',s.rssi+' dBm')+row('Channel',s.channel)+row('BSSID',s.bssid);"
"document.getElementById('mq').innerHTML="
"row('Status',s.mqtt?'connected':'down')+row('Broker',s.broker)+"
"row('Messages published',s.pubOk)+row('Publish failures',s.pubFail)+"
"row('Connects',s.connects)+row('Disconnects',s.disconnects)+"
"row('Last publish',s.lastPub<0?'never':s.lastPub+' s ago');"
"document.getElementById('dev').innerHTML="
"row('Firmware','v'+s.version)+row('OTA slot',s.partition)+"
"row('Reset reason',s.resetReason)+row('Free heap',s.heap+' bytes')+"
"row('Uptime',s.uptime+' s')+row('X10A protocol',s.protocol)+"
"row('Refrigerant',s.refrigerant)+row('X10A pins','RX '+s.rxPin+' / TX '+s.txPin);"
"var v=await (await fetch('/api/values')).json();"
"document.getElementById('vals').innerHTML=v.values.length?v.values.map(x=>"
"'<tr><td class=\"r\">'+esc(x.reg)+'</td><td>'+esc(x.label)+'</td><td class=\"v\">'+esc(x.value)+'</td></tr>'"
").join(''):'<tr><td colspan=\"3\">no values read yet</td></tr>';"
"document.getElementById('vhint').textContent="
"v.values.length+' of '+v.total+' labels have been read at least once.';"
"var d=await (await fetch('/api/x10a')).json();"
"document.getElementById('rxl').textContent='RX line at boot: '+d.rxIdle;"
"document.getElementById('diag').innerHTML=d.registries.length?d.registries.map(x=>"
"'<tr><td class=\"r\">'+esc(x.reg)+'</td><td>'+esc(x.proto)+'</td><td>'+esc(x.status)+"
"'</td><td>'+x.ok+'/'+x.fail+'</td><td class=\"r\">'+esc(x.raw||'-')+'</td></tr>'"
").join(''):'<tr><td colspan=\"5\">no queries yet</td></tr>';"
"}catch(e){}}"
"var GH='';"
"async function cfg(){var c=await (await fetch('/api/config')).json();"
"document.getElementById('uri').value=c.uri;document.getElementById('usr').value=c.user;"
"document.getElementById('rxp').value=c.rxPin;document.getElementById('txp').value=c.txPin;"
"document.getElementById('ghr').value=c.repo;GH=c.repo;"
"document.getElementById('pwdh').textContent=c.passSet?"
"'A password is stored. Leave blank to keep it.':'No password stored.';"
"ghLoad();}"
"async function ghLoad(){var s=document.getElementById('ghRel');"
"if(!GH){s.innerHTML='<option>no repository configured</option>';return;}"
"s.innerHTML='<option>Loading releases\\u2026</option>';"
"try{var r=await fetch('https://api.github.com/repos/'+GH+'/releases?per_page=100');"
"if(!r.ok){s.innerHTML='<option>GitHub returned '+r.status+'</option>';return;}"
"var rel=await r.json();var o='',n=0;"
"rel.forEach(x=>{(x.assets||[]).forEach(a=>{"
"if(!a.name.endsWith('.bin'))return;n++;"
"var run=RUNNING&&x.tag_name.replace(/^v/,'')==RUNNING?' (running)':'';"
"o+='<option value=\"'+esc(a.browser_download_url)+'\">'+esc(x.tag_name)+run+"
"' \\u2013 '+Math.round(a.size/1024)+' KB</option>';})});"
"s.innerHTML=n?o:'<option>no releases with a .bin asset</option>';}"
"catch(e){s.innerHTML='<option>could not reach GitHub</option>';}}"
"async function ghPoll(){var r=await (await fetch('/ota/from-url')).json();"
"var m=document.getElementById('gmsg');var p=document.getElementById('gpg');"
"if(r.total>0){p.style.display='block';p.value=100*r.read/r.total;}"
"if(r.state=='running'){m.textContent='downloading '+Math.round(r.read/1024)+' KB'"
"+(r.total?' of '+Math.round(r.total/1024)+' KB':'');setTimeout(ghPoll,1500);}"
"else if(r.state=='done'){m.textContent=r.msg;}"
"else if(r.state=='failed'){m.className='msg err';m.textContent='Failed: '+r.msg;}}"
"async function ghFlash(){var s=document.getElementById('ghRel');"
"var m=document.getElementById('gmsg');m.className='msg';"
"var u=s.value;if(!u||u.indexOf('http')!=0){m.className='msg err';"
"m.textContent='Select a release first.';return;}"
"m.textContent='starting\\u2026';"
"try{var r=await fetch('/ota/from-url',{method:'POST',body:u});"
"if(!r.ok){m.className='msg err';m.textContent='Failed: '+await r.text();return;}"
"setTimeout(ghPoll,1200);}"
"catch(e){m.className='msg err';m.textContent='Failed: '+e;}}"
"async function save(){var m=document.getElementById('cmsg');m.className='msg';m.textContent='saving...';"
"var b={uri:document.getElementById('uri').value,user:document.getElementById('usr').value,"
"pass:document.getElementById('pwd').value,"
"rxPin:document.getElementById('rxp').value,txPin:document.getElementById('txp').value,"
"repo:document.getElementById('ghr').value};"
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

    uint32_t pub_ok = 0, pub_fail = 0, connects = 0, disconnects = 0;
    int64_t last_pub = 0;
    alt_mqtt_stats(&pub_ok, &pub_fail, &connects, &disconnects, &last_pub);

    char body[900];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"channel\":%d,\"bssid\":\"%s\",\"wifi\":%s,\"mqtt\":%s,"
             "\"broker\":\"%s\",\"protocol\":\"%c\",\"refrigerant\":%d,"
             "\"partition\":\"%s\",\"resetReason\":%d,\"heap\":%u,\"uptime\":%lld,"
             "\"rxPin\":%d,\"txPin\":%d,\"pubOk\":%u,\"pubFail\":%u,"
             "\"connects\":%u,\"disconnects\":%u,\"lastPub\":%lld}",
             FIRMWARE_VERSION, alt_wifi_ssid(), ip, alt_wifi_rssi(),
             alt_wifi_channel(), bssid,
             alt_wifi_is_connected() ? "true" : "false",
             alt_mqtt_is_connected() ? "true" : "false",
             alt_settings_mqtt_uri(), converter_protocol(), converter_refrigerant(),
             run ? run->label : "?",
             (int)esp_reset_reason(),
             (unsigned)esp_get_free_heap_size(),
             esp_timer_get_time() / 1000000,
             alt_settings_rx_pin(), alt_settings_tx_pin(),
             (unsigned)pub_ok, (unsigned)pub_fail,
             (unsigned)connects, (unsigned)disconnects,
             last_pub ? (esp_timer_get_time() / 1000 - last_pub) / 1000 : -1);

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
    snprintf(head, sizeof(head), "{\"rxIdle\":\"%s\",\"registries\":[",
             alt_rx_idle_state());
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

static esp_err_t config_get(httpd_req_t *req)
{
    char body[320];
    // The stored password is deliberately never sent to the browser; the UI
    // only learns whether one exists.
    snprintf(body, sizeof(body),
             "{\"uri\":\"%s\",\"user\":\"%s\",\"passSet\":%s,"
             "\"rxPin\":%d,\"txPin\":%d,\"repo\":\"%s\"}",
             alt_settings_mqtt_uri(), alt_settings_mqtt_user(),
             alt_settings_mqtt_pass_set() ? "true" : "false",
             alt_settings_rx_pin(), alt_settings_tx_pin(), alt_settings_gh_repo());

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

    char repo[ALT_SETTING_MAX] = {0};
    if (json_field(body, "repo", repo, sizeof(repo)) && repo[0] != 0) {
        alt_settings_set_gh_repo(repo);
    }

    // Pins are optional in the body; absent means "leave them alone".
    char rxs[8] = {0};
    char txs[8] = {0};
    if (json_field(body, "rxPin", rxs, sizeof(rxs)) &&
        json_field(body, "txPin", txs, sizeof(txs))) {
        int rx = atoi(rxs);
        int tx = atoi(txs);
        const char *bad = alt_settings_check_pins(rx, tx);
        if (bad) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, bad);
            return ESP_FAIL;
        }
        // Saved before the MQTT settings so a failure here cannot leave the
        // broker updated and the pin map not.
        esp_err_t perr = alt_settings_set_pins(rx, tx);
        if (perr != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                esp_err_to_name(perr));
            return ESP_FAIL;
        }
    }

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

// ----------------------------------------------- OTA from a GitHub release
//
// The browser can LIST releases — the GitHub API sends CORS `*` — but it cannot
// download the .bin, because the release asset host sends no CORS header at
// all. So the page picks a release and the device fetches and flashes it
// itself over TLS.
//
// Repo-locked: the URL must begin with the configured repository's release
// download prefix, so this can only ever install this project's own releases,
// never an arbitrary binary pointed at the endpoint. esp_https_ota does
// begin/write/verify/set-boot and inherits the same dual-slot rollback as a
// manual upload.

typedef enum { OTAU_IDLE = 0, OTAU_RUNNING, OTAU_DONE, OTAU_FAILED } otau_state_t;

static volatile otau_state_t s_otau_state = OTAU_IDLE;
static volatile int s_otau_read, s_otau_total;
static char s_otau_msg[96];
static char s_otau_url[320];

static void otau_fail(const char *m)
{
    snprintf(s_otau_msg, sizeof(s_otau_msg), "%s", m);
    s_otau_state = OTAU_FAILED;
    ESP_LOGE(TAG, "OTA from URL failed: %s", m);
}

static void otau_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t http_cfg = {
        .url               = s_otau_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        .buffer_size       = 2048,
        .buffer_size_tx    = 4096,   // GitHub's signed redirect URL is ~900 chars
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };
    esp_https_ota_handle_t h = NULL;

    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK || h == NULL) {
        otau_fail("could not connect to GitHub");
        vTaskDelete(NULL);
        return;
    }

    s_otau_total = esp_https_ota_get_image_size(h);
    do {
        err = esp_https_ota_perform(h);
        s_otau_read = esp_https_ota_get_image_len_read(h);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            snprintf(s_otau_msg, sizeof(s_otau_msg), "downloaded %d KB - rebooting",
                     s_otau_read / 1024);
            s_otau_state = OTAU_DONE;
            ESP_LOGW(TAG, "OTA from URL complete, rebooting into the new image");
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        } else {
            otau_fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation"
                                                         : "flash finalize failed");
        }
    } else {
        esp_https_ota_abort(h);
        otau_fail("download interrupted");
    }
    vTaskDelete(NULL);
}

// Only this project's own release assets, and nothing that could traverse away
// from them.
static bool otau_url_allowed(const char *u)
{
    char prefix[192];
    snprintf(prefix, sizeof(prefix), "https://github.com/%s/releases/download/",
             alt_settings_gh_repo());
    return strncmp(u, prefix, strlen(prefix)) == 0 &&
           strstr(u, "..") == NULL && strchr(u, ' ') == NULL &&
           strlen(u) < sizeof(s_otau_url);
}

static esp_err_t otau_post(httpd_req_t *req)
{
    if (s_otau_state == OTAU_RUNNING) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "already running");
        return ESP_FAIL;
    }

    char body[352];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len
                                                       : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[got] = '\0';
    while (got > 0 && (body[got - 1] == '\n' || body[got - 1] == '\r')) {
        body[--got] = '\0';
    }

    if (!otau_url_allowed(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "URL is not a release asset of the configured repo");
        return ESP_FAIL;
    }

    strlcpy(s_otau_url, body, sizeof(s_otau_url));
    s_otau_read = 0;
    s_otau_total = 0;
    s_otau_msg[0] = '\0';
    s_otau_state = OTAU_RUNNING;
    ESP_LOGI(TAG, "OTA from URL: %s", s_otau_url);
    // 8 KB: TLS handshake plus the HTTPS OTA machinery.
    xTaskCreate(&otau_task, "ota_url", 8192, NULL, 5, NULL);

    httpd_resp_sendstr(req, "started");
    return ESP_OK;
}

static esp_err_t otau_get(httpd_req_t *req)
{
    const char *st = s_otau_state == OTAU_RUNNING ? "running"
                   : s_otau_state == OTAU_DONE    ? "done"
                   : s_otau_state == OTAU_FAILED  ? "failed" : "idle";
    char body[192];
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"read\":%d,\"total\":%d,\"msg\":\"%s\"}",
             st, s_otau_read, s_otau_total, s_otau_msg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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
        {.uri = "/api/config",  .method = HTTP_GET,  .handler = config_get},
        {.uri = "/api/config",  .method = HTTP_POST, .handler = config_post},
        {.uri = "/ota/upload",  .method = HTTP_POST, .handler = ota_post},
        {.uri = "/ota/from-url",.method = HTTP_POST, .handler = otau_post},
        {.uri = "/ota/from-url",.method = HTTP_GET,  .handler = otau_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }

    char ip[16];
    alt_wifi_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "web UI on http://%s/", ip);
    return ESP_OK;
}
