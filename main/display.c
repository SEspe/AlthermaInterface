// SSD1306 status page. See display.h for why the panel is optional.
//
// The driver is written out here rather than pulled from the component registry
// because it is small - an init sequence, a framebuffer and a font - and this
// project already keeps its dependency list to the one component IDF 6 dropped
// (esp-mqtt). A registry component would be more code, not less, once its
// configuration and version pinning are counted.

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_config.h"
#include "converters.h"
#include "derived.h"
#include "display.h"
#include "version.h"
#include "wifi.h"

static const char *TAG = "display";

#define OLED_W     128
#define OLED_H      64
#define OLED_PAGES  (OLED_H / 8)
#define COLS        (OLED_W / 6)   // 5x7 glyph + 1 px gap = 21 columns

// Where the readings live. Located by position, not by name: label strings are
// user-facing and do get renamed, while the register and offset are the
// physical thing. Same reasoning, and the same guards, as derived.c.
#define REG_TEMPS   0x54
#define OFF_INLET   2
#define OFF_OUTLET  4
#define OFF_TANK    8
#define GUARD_INLET   "Inlet water"
#define GUARD_OUTLET  "Outlet Water"
#define GUARD_TANK    "DHW tank"

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static bool s_io_failed;   // rate-limits the log when the panel stops answering

// ---- panel -------------------------------------------------------------

static esp_err_t cmds(const uint8_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b[2] = { 0x00, list[i] };
        esp_err_t e = i2c_master_transmit(s_dev, b, sizeof b, 200);
        if (e != ESP_OK) {
            return e;
        }
    }
    return ESP_OK;
}

static const uint8_t INIT[] = {
    0xAE,             // display off
    0xD5, 0x80,       // clock divide
    0xA8, 0x3F,       // multiplex = 63
    0xD3, 0x00,       // no display offset
    0x40,             // start line 0
    0x8D, 0x14,       // charge pump on
    0x20, 0x00,       // horizontal addressing
    0xA1,             // segment remap
    0xC8,             // COM scan descending
    0xDA, 0x12,       // COM pin config
    0x81, 0x7F,       // contrast
    0xD9, 0xF1,       // precharge
    0xDB, 0x40,       // VCOM detect
    0xA4,             // resume from RAM
    0xA6,             // normal, not inverted
    0xAF,             // display on
};

// 5x7 font, ASCII 32..126. Each glyph is 5 columns, LSB = top pixel.
static const uint8_t FONT[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},
    {0x3E,0x41,0x41,0x51,0x32},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
};

static uint8_t s_fb[OLED_W * OLED_PAGES];

static void fb_clear(void)
{
    memset(s_fb, 0, sizeof s_fb);
}

static void fb_text(int col, int page, const char *s)
{
    if (page < 0 || page >= OLED_PAGES) {
        return;
    }
    for (int x = col * 6; *s && x + 5 <= OLED_W; s++, x += 6) {
        char ch = (*s < 32 || *s > 126) ? '?' : *s;
        memcpy(s_fb + page * OLED_W + x, FONT[(int)ch - 32], 5);
    }
}

static void fb_flush(void)
{
    const uint8_t win[] = { 0x21, 0, OLED_W - 1, 0x22, 0, OLED_PAGES - 1 };
    esp_err_t e = cmds(win, sizeof win);

    for (int p = 0; p < OLED_PAGES && e == ESP_OK; p++) {
        uint8_t buf[1 + OLED_W];
        buf[0] = 0x40;
        memcpy(buf + 1, s_fb + p * OLED_W, OLED_W);
        e = i2c_master_transmit(s_dev, buf, sizeof buf, 300);
    }

    if (e != ESP_OK) {
        // Logged once per outage, not once per poll: a loose panel must not
        // drown the log the X10A link is diagnosed from.
        if (!s_io_failed) {
            ESP_LOGW(TAG, "panel stopped answering (%s); still trying",
                     esp_err_to_name(e));
            s_io_failed = true;
        }
    } else if (s_io_failed) {
        ESP_LOGI(TAG, "panel answering again");
        s_io_failed = false;
    }
}

// ---- readings ----------------------------------------------------------

// The value at (reg, offset), or NULL if the label is missing, renamed, or not
// read yet. A renamed label is a silent wrong number on the screen otherwise.
static const char *reading(uint8_t reg, int offset, const char *guard)
{
    const char *label = NULL;
    const char *value = NULL;

    if (!converter_label_by_offset(reg, offset, &label, &value)) {
        return NULL;
    }
    if (strstr(label, guard) == NULL) {
        return NULL;
    }
    return value[0] != '\0' ? value : NULL;
}

// "32.3" from "32.2812" - the panel has 21 columns, and a tenth of a kelvin is
// already finer than these sensors resolve.
static void temp_1dp(char *out, size_t len, uint8_t reg, int offset,
                     const char *guard)
{
    const char *v = reading(reg, offset, guard);
    char *end = NULL;
    double d;

    if (v == NULL) {
        snprintf(out, len, "--");
        return;
    }
    d = strtod(v, &end);
    if (end == v || (end && *end != '\0')) {
        snprintf(out, len, "--");   // a converter error string, not a number
        return;
    }
    snprintf(out, len, "%.1f", d);
}

// ---- public ------------------------------------------------------------

bool alt_display_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = ALT_OLED_I2C_PORT,
        .sda_io_num = ALT_OLED_SDA_PIN,
        .scl_io_num = ALT_OLED_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ALT_OLED_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t e;

    e = i2c_new_master_bus(&bc, &s_bus);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus on SDA=%d SCL=%d failed (%s); no display",
                 ALT_OLED_SDA_PIN, ALT_OLED_SCL_PIN, esp_err_to_name(e));
        return false;
    }

    if (i2c_master_probe(s_bus, ALT_OLED_ADDR, 100) != ESP_OK) {
        // The expected case on a board without the panel. Info, not warning.
        ESP_LOGI(TAG, "no panel at 0x%02X on SDA=%d SCL=%d; display disabled",
                 ALT_OLED_ADDR, ALT_OLED_SDA_PIN, ALT_OLED_SCL_PIN);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return false;
    }

    e = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
    if (e == ESP_OK) {
        e = cmds(INIT, sizeof INIT);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "panel found but would not initialise (%s)",
                 esp_err_to_name(e));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        s_dev = NULL;
        return false;
    }

    s_present = true;
    ESP_LOGI(TAG, "SSD1306 at 0x%02X on SDA=%d SCL=%d",
             ALT_OLED_ADDR, ALT_OLED_SDA_PIN, ALT_OLED_SCL_PIN);
    return true;
}

bool alt_display_present(void)
{
    return s_present;
}

void alt_display_banner(const char *line1, const char *line2)
{
    if (!s_present) {
        return;
    }
    fb_clear();
    if (line1) {
        fb_text(0, 2, line1);
    }
    if (line2) {
        fb_text(0, 4, line2);
    }
    fb_flush();
}

void alt_display_update(void)
{
    char ip[16] = "";
    char in[8], out[8], tank[8];
    char line[COLS + 1];
    const char *comp;
    const char *src;

    if (!s_present) {
        return;
    }

    alt_wifi_ip(ip, sizeof ip);
    comp = alt_derived_compressor();
    src = alt_derived_compressor_source();
    temp_1dp(in,   sizeof in,   REG_TEMPS, OFF_INLET,  GUARD_INLET);
    temp_1dp(out,  sizeof out,  REG_TEMPS, OFF_OUTLET, GUARD_OUTLET);
    temp_1dp(tank, sizeof tank, REG_TEMPS, OFF_TANK,   GUARD_TANK);

    fb_clear();
    fb_text(0, 0, "AlthermaInterface");
    fb_text(0, 1, ip[0] ? ip : "no IP");

    // The source is shown beside the state because the two differ by tens of
    // seconds on every edge, so "ON" alone does not say how fresh it is.
    snprintf(line, sizeof line, "Comp %-4s%s",
             comp[0] ? comp : "--", src[0] ? src : "");
    fb_text(0, 3, line);

    snprintf(line, sizeof line, "In   %s C", in);
    fb_text(0, 5, line);
    snprintf(line, sizeof line, "Out  %s C", out);
    fb_text(0, 6, line);
    snprintf(line, sizeof line, "Tank %s C", tank);
    fb_text(0, 7, line);

    fb_flush();
}
