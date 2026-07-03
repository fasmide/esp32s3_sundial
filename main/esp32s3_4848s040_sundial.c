#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/ledc.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "generated_font.h"

#define LCD_H_RES 480
#define LCD_V_RES 480
#define LCD_BITS_PER_PIXEL 16
#define LCD_BYTES_PER_PIXEL 2

#define LCD_PIN_PCLK 21
#define LCD_PIN_DE 18
#define LCD_PIN_VSYNC 17
#define LCD_PIN_HSYNC 16

#define LCD_PIN_SPI_CS 39
#define LCD_PIN_SPI_SCL 48
#define LCD_PIN_SPI_SDA 47

#define LCD_PIN_BL 38

#define LCD_PCLK_HZ (12 * 1000 * 1000)
#define LCD_BOUNCE_LINES 10
#define LCD_BOUNCE_PIXELS (LCD_H_RES * LCD_BOUNCE_LINES)

#define WIFI_SSID "ssid"
#define WIFI_PASSWORD "hemmelig kode"

#define WIFI_CONNECTED_BIT BIT0
#define TIME_SYNC_BIT BIT1

#define LATITUDE 57.0488
#define LONGITUDE 9.9217

#define DEFAULT_SUNRISE_MINUTES (6 * 60)
#define DEFAULT_SUNSET_MINUTES (18 * 60)
#define MINUTES_PER_DAY (24 * 60)

#define DEG_TO_RAD (0.017453292519943295769f)
#define RAD_TO_DEG (57.295779513082320876f)
#define TWO_PI (6.28318530717958647692f)

#define SKY_DISC_INSET 0
#define HOUR_NUMBER_RADIUS 190
#define TICK_OUTER_RADIUS 236
#define TICK_INNER_EVEN_RADIUS 222
#define TICK_INNER_ODD_RADIUS 229
#define SUN_DOT_ORBIT_RADIUS 148
#define SUN_DOT_RADIUS 13
#define CENTER_DISC_RADIUS 105
#define HAND_BASE_GAP 6

typedef struct {
    int sunrise_minutes;
    int sunset_minutes;
} render_state_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb8_t;

typedef struct {
    int elevation_deg;
    rgb8_t color;
} sky_keyframe_t;

static const char *TAG = "sunset_port";

static EventGroupHandle_t s_event_group;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_frame_front;
static uint16_t *s_frame_back;
static uint16_t *s_static_background;
static volatile uint16_t *s_active_frame;

static const st7701_lcd_init_cmd_t s_st7701_type9_init_ops[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x31, 0x05}, 2, 0},
    {0xCD, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
                      0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
                      0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x32}, 1, 0},
    {0xB2, (uint8_t[]){0x07}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
                      0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
                      0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
                      0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
                      0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
                      0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE5, (uint8_t[]){0xE4}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x3A, (uint8_t[]){0x60}, 1, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

static inline uint16_t scale_rgb565(uint16_t color, uint8_t scale)
{
    uint8_t r = (uint8_t)((((color >> 11) & 0x1F) * 255U) / 31U);
    uint8_t g = (uint8_t)((((color >> 5) & 0x3F) * 255U) / 63U);
    uint8_t b = (uint8_t)(((color & 0x1F) * 255U) / 31U);

    r = (uint8_t)((r * scale) / 100U);
    g = (uint8_t)((g * scale) / 100U);
    b = (uint8_t)((b * scale) / 100U);
    return rgb565(r, g, b);
}

static inline uint32_t hash_u32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;
static const uint16_t COLOR_DARK_GRAY = 0x632C;
static const uint16_t COLOR_LIGHT_GRAY = 0xC618;
static const uint16_t COLOR_OXFORD_BLUE = 0x19C8;
static const uint16_t COLOR_PICTON_BLUE = 0x35FF;
static const uint16_t COLOR_SUN_GLOW = 0xFFE0;
static const uint16_t COLOR_SUN_FACE = 0xFFF2;
static const uint16_t COLOR_MOON_LIGHT = 0xD69A;
static const uint16_t COLOR_MOON_SHADOW = 0x7BEF;
static const uint16_t COLOR_STAR_SOFT = 0xC618;
static const sky_keyframe_t SKY_KEYFRAMES[] = {
    {20, {74, 144, 217}},
    {5, {120, 170, 225}},
    {0, {255, 150, 100}},
    {-4, {220, 90, 120}},
    {-8, {75, 59, 107}},
    {-14, {26, 33, 81}},
    {-18, {10, 14, 39}},
};
static inline int wrap_minutes(int minutes)
{
    int wrapped = minutes % MINUTES_PER_DAY;
    if (wrapped < 0) {
        wrapped += MINUTES_PER_DAY;
    }
    return wrapped;
}

static inline float minutes_to_radians(int minutes)
{
    int normalized = wrap_minutes(minutes);
    int offset = (normalized + (MINUTES_PER_DAY / 2)) % MINUTES_PER_DAY;
    return ((float)offset / (float)MINUTES_PER_DAY) * TWO_PI;
}

static inline float point_angle_radians(int x, int y, int center_x, int center_y)
{
    float dx = (float)(x - center_x);
    float dy = (float)(y - center_y);
    float angle = atan2f(dx, -dy);
    if (angle < 0.0f) {
        angle += TWO_PI;
    }
    return angle;
}

static inline int angle_to_wall_minutes(float angle)
{
    int offset_minutes = (int)lroundf((angle / TWO_PI) * (float)MINUTES_PER_DAY);
    return wrap_minutes(offset_minutes - (MINUTES_PER_DAY / 2));
}

static inline bool angle_is_between(float angle, float start, float end)
{
    if (start <= end) {
        return angle >= start && angle <= end;
    }
    return angle >= start || angle <= end;
}

static inline int line_from_pos(int pos_px)
{
    return pos_px / LCD_H_RES;
}

static inline void copy_row_480(uint16_t *dst, const uint16_t *src)
{
    memcpy(dst, src, LCD_H_RES * sizeof(uint16_t));
}

static uint16_t blend_rgb565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    uint32_t dst_r = (dst >> 11) & 0x1F;
    uint32_t dst_g = (dst >> 5) & 0x3F;
    uint32_t dst_b = dst & 0x1F;
    uint32_t src_r = (src >> 11) & 0x1F;
    uint32_t src_g = (src >> 5) & 0x3F;
    uint32_t src_b = src & 0x1F;

    uint32_t out_r = (dst_r * (255 - alpha) + src_r * alpha) / 255;
    uint32_t out_g = (dst_g * (255 - alpha) + src_g * alpha) / 255;
    uint32_t out_b = (dst_b * (255 - alpha) + src_b * alpha) / 255;

    return (uint16_t)((out_r << 11) | (out_g << 5) | out_b);
}

static void blend_pixel(uint16_t *fb, int x, int y, uint16_t color, uint8_t alpha)
{
    if ((unsigned)x >= LCD_H_RES || (unsigned)y >= LCD_V_RES || alpha == 0) {
        return;
    }
    fb[(y * LCD_H_RES) + x] = blend_rgb565(fb[(y * LCD_H_RES) + x], color, alpha);
}

static void clear_frame(uint16_t *fb, uint16_t color)
{
    for (int i = 0; i < (LCD_H_RES * LCD_V_RES); i++) {
        fb[i] = color;
    }
}

static void copy_frame(uint16_t *dst, const uint16_t *src)
{
    memcpy(dst, src, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
}

static void fill_circle(uint16_t *fb, int cx, int cy, int radius, uint16_t color)
{
    int radius_sq = radius * radius;

    for (int y = -radius; y <= radius; y++) {
        int yy = cy + y;
        if ((unsigned)yy >= LCD_V_RES) {
            continue;
        }

        for (int x = -radius; x <= radius; x++) {
            int xx = cx + x;
            if ((unsigned)xx >= LCD_H_RES) {
                continue;
            }
            if ((x * x) + (y * y) <= radius_sq) {
                fb[(yy * LCD_H_RES) + xx] = color;
            }
        }
    }
}

static void fill_disc_arc(uint16_t *fb, int cx, int cy, int radius, uint16_t color, int from_min, int to_min)
{
    int radius_sq = radius * radius;
    float start = minutes_to_radians(from_min);
    float end = minutes_to_radians(to_min);

    for (int y = -radius; y <= radius; y++) {
        int yy = cy + y;
        if ((unsigned)yy >= LCD_V_RES) {
            continue;
        }

        for (int x = -radius; x <= radius; x++) {
            int xx = cx + x;
            if ((unsigned)xx >= LCD_H_RES) {
                continue;
            }
            if ((x * x) + (y * y) > radius_sq) {
                continue;
            }
            if (angle_is_between(point_angle_radians(xx, yy, cx, cy), start, end)) {
                fb[(yy * LCD_H_RES) + xx] = color;
            }
        }
    }
}

static void draw_line_thick(uint16_t *fb, int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int radius = thickness / 2;

    while (true) {
        fill_circle(fb, x0, y0, radius, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_circle_outline(uint16_t *fb, int cx, int cy, int radius, int thickness, uint16_t color)
{
    for (int angle_deg = 0; angle_deg < 360; angle_deg++) {
        float angle = ((float)angle_deg / 360.0f) * TWO_PI;
        int x = cx + (int)lroundf(sinf(angle) * (float)radius);
        int y = cy - (int)lroundf(cosf(angle) * (float)radius);
        fill_circle(fb, x, y, thickness / 2, color);
    }
}

static void point_on_circle(int center_x, int center_y, int radius, int wall_minutes, int *out_x, int *out_y)
{
    float angle = minutes_to_radians(wall_minutes);
    *out_x = center_x + (int)lroundf(sinf(angle) * (float)radius);
    *out_y = center_y - (int)lroundf(cosf(angle) * (float)radius);
}

static const generated_glyph_t *glyph_for_char(const generated_glyph_t *glyphs, size_t glyph_count, char c)
{
    for (size_t i = 0; i < glyph_count; i++) {
        if (glyphs[i].ch == c) {
            return &glyphs[i];
        }
    }
    return &glyphs[glyph_count - 1];
}

static void draw_char(uint16_t *fb, int x, int y, char c,
                      const generated_glyph_t *glyphs, size_t glyph_count,
                      int glyph_width, int glyph_height, uint16_t color)
{
    const generated_glyph_t *glyph = glyph_for_char(glyphs, glyph_count, c);

    for (int row = 0; row < glyph_height; row++) {
        for (int col = 0; col < glyph_width; col++) {
            blend_pixel(fb, x + col, y + row, color, glyph->bitmap[(row * glyph_width) + col]);
        }
    }
}

static int text_width_px(const char *text, int glyph_advance)
{
    int len = (int)strlen(text);

    if (len == 0) {
        return 0;
    }
    return len * glyph_advance;
}

static void draw_text(uint16_t *fb, int x, int y, const char *text,
                      const generated_glyph_t *glyphs, size_t glyph_count,
                      int glyph_width, int glyph_height, int glyph_advance,
                      uint16_t color)
{
    int cursor_x = x;

    for (size_t i = 0; text[i] != '\0'; i++) {
        draw_char(fb, cursor_x, y, text[i], glyphs, glyph_count, glyph_width, glyph_height, color);
        cursor_x += glyph_advance;
    }
}

static void draw_text_centered_outline(uint16_t *fb, int center_x, int y, const char *text,
                                       const generated_glyph_t *glyphs, size_t glyph_count,
                                       int glyph_width, int glyph_height, int glyph_advance,
                                       uint16_t color, uint16_t outline)
{
    int start_x = center_x - (text_width_px(text, glyph_advance) / 2);

    for (int oy = -1; oy <= 1; oy++) {
        for (int ox = -1; ox <= 1; ox++) {
            if (ox == 0 && oy == 0) {
                continue;
            }
            draw_text(fb, start_x + ox, y + oy, text,
                      glyphs, glyph_count, glyph_width, glyph_height, glyph_advance,
                      outline);
        }
    }
    draw_text(fb, start_x, y, text,
              glyphs, glyph_count, glyph_width, glyph_height, glyph_advance,
              color);
}

static float solar_elevation_degrees(time_t unix_seconds, double lat_deg, double lng_deg)
{
    const double j2000_unix_seconds = 946728000.0;
    const double deg_to_rad = M_PI / 180.0;
    const double rad_to_deg = 180.0 / M_PI;
    double d = ((double)unix_seconds - j2000_unix_seconds) / 86400.0;
    double g = (357.529 + (0.98560028 * d)) * deg_to_rad;
    double q = 280.459 + (0.98564736 * d);
    double L = (q + (1.915 * sin(g)) + (0.020 * sin(2.0 * g))) * deg_to_rad;
    double e = (23.439 - (0.00000036 * d)) * deg_to_rad;
    double dec = asin(sin(e) * sin(L));
    double ra = atan2(cos(e) * sin(L), cos(L)) * rad_to_deg;
    double gmst = fmod(280.46061837 + (360.98564736629 * d), 360.0);
    double lat_rad;
    double h;

    if (gmst < 0.0) {
        gmst += 360.0;
    }

    lat_rad = lat_deg * deg_to_rad;
    h = (gmst + lng_deg - ra) * deg_to_rad;
    return (float)(asin((sin(lat_rad) * sin(dec))
        + (cos(lat_rad) * cos(dec) * cos(h))) * rad_to_deg);
}

static uint16_t sky_color_for_elevation(float elevation_deg)
{
    size_t keyframe_count = sizeof(SKY_KEYFRAMES) / sizeof(SKY_KEYFRAMES[0]);

    if (elevation_deg >= (float)SKY_KEYFRAMES[0].elevation_deg) {
        rgb8_t color = SKY_KEYFRAMES[0].color;
        return rgb565(color.r, color.g, color.b);
    }
    if (elevation_deg <= (float)SKY_KEYFRAMES[keyframe_count - 1].elevation_deg) {
        rgb8_t color = SKY_KEYFRAMES[keyframe_count - 1].color;
        return rgb565(color.r, color.g, color.b);
    }

    for (size_t i = 0; i + 1 < keyframe_count; i++) {
        sky_keyframe_t upper = SKY_KEYFRAMES[i];
        sky_keyframe_t lower = SKY_KEYFRAMES[i + 1];

        if (elevation_deg <= (float)upper.elevation_deg && elevation_deg >= (float)lower.elevation_deg) {
            float t = (elevation_deg - (float)upper.elevation_deg)
                / (float)(lower.elevation_deg - upper.elevation_deg);
            uint8_t r = (uint8_t)lroundf((float)upper.color.r + (t * (float)(lower.color.r - upper.color.r)));
            uint8_t g = (uint8_t)lroundf((float)upper.color.g + (t * (float)(lower.color.g - upper.color.g)));
            uint8_t b = (uint8_t)lroundf((float)upper.color.b + (t * (float)(lower.color.b - upper.color.b)));

            return rgb565(r, g, b);
        }
    }

    return COLOR_BLACK;
}

static float star_visibility_for_elevation(float elevation_deg)
{
    if (elevation_deg > -8.0f) {
        return 0.0f;
    }
    if (elevation_deg <= -18.0f) {
        return 1.0f;
    }
    return (-8.0f - elevation_deg) / 10.0f;
}

static uint8_t encode_star_visibility(float elevation_deg)
{
    return (uint8_t)lroundf(star_visibility_for_elevation(elevation_deg) * 255.0f);
}

static void build_sky_palette_for_day(const struct tm *local_tm, uint16_t *minute_colors,
                                      uint8_t *minute_star_visibility)
{
    struct tm midnight_tm = *local_tm;
    time_t midnight_local;

    midnight_tm.tm_hour = 0;
    midnight_tm.tm_min = 0;
    midnight_tm.tm_sec = 0;
    midnight_local = mktime(&midnight_tm);

    if (midnight_local == (time_t)-1) {
        for (int minute = 0; minute < MINUTES_PER_DAY; minute++) {
            minute_colors[minute] = COLOR_BLACK;
            minute_star_visibility[minute] = 255;
        }
        return;
    }

    for (int minute = 0; minute < MINUTES_PER_DAY; minute++) {
        time_t sample_time = midnight_local + ((time_t)minute * 60);
        float elevation = solar_elevation_degrees(sample_time, LATITUDE, LONGITUDE);

        minute_star_visibility[minute] = encode_star_visibility(elevation);
        minute_colors[minute] = sky_color_for_elevation(elevation);
    }
}

static int day_of_year_1_based(const struct tm *tm)
{
    return tm->tm_yday + 1;
}

static int timezone_offset_minutes_from_tm(const struct tm *local_tm)
{
    char tz_buf[8] = {0};
    int sign = 1;
    int hours = 0;
    int minutes = 0;

    if (strftime(tz_buf, sizeof(tz_buf), "%z", local_tm) == 0) {
        return 60;
    }

    if (tz_buf[0] == '-') {
        sign = -1;
    }

    hours = ((tz_buf[1] - '0') * 10) + (tz_buf[2] - '0');
    minutes = ((tz_buf[3] - '0') * 10) + (tz_buf[4] - '0');
    return sign * ((hours * 60) + minutes);
}

static void compute_sun_times_for_date(const struct tm *local_tm, int *sunrise_out, int *sunset_out)
{
    int day = day_of_year_1_based(local_tm);
    struct tm noon_tm = *local_tm;
    int tz_offset_minutes;
    float gamma = (2.0f * (float)M_PI / 365.0f) * (float)(day - 1);
    float eq_time = 229.18f * (0.000075f
        + 0.001868f * cosf(gamma)
        - 0.032077f * sinf(gamma)
        - 0.014615f * cosf(2.0f * gamma)
        - 0.040849f * sinf(2.0f * gamma));
    float decl = 0.006918f
        - 0.399912f * cosf(gamma)
        + 0.070257f * sinf(gamma)
        - 0.006758f * cosf(2.0f * gamma)
        + 0.000907f * sinf(2.0f * gamma)
        - 0.002697f * cosf(3.0f * gamma)
        + 0.00148f * sinf(3.0f * gamma);
    float lat_rad = (float)LATITUDE * DEG_TO_RAD;
    float zenith = 90.833f * DEG_TO_RAD;
    float cos_ha = (cosf(zenith) / (cosf(lat_rad) * cosf(decl))) - (tanf(lat_rad) * tanf(decl));

    noon_tm.tm_hour = 12;
    noon_tm.tm_min = 0;
    noon_tm.tm_sec = 0;
    noon_tm.tm_isdst = -1;
    tz_offset_minutes = timezone_offset_minutes_from_tm(&noon_tm);

    if (cos_ha <= -1.0f || cos_ha >= 1.0f) {
        *sunrise_out = DEFAULT_SUNRISE_MINUTES;
        *sunset_out = DEFAULT_SUNSET_MINUTES;
        return;
    }

    float ha = acosf(cos_ha) * RAD_TO_DEG;
    float sunrise_utc = 720.0f - (4.0f * ((float)LONGITUDE + ha)) - eq_time;
    float sunset_utc = 720.0f - (4.0f * ((float)LONGITUDE - ha)) - eq_time;
    int sunrise_local = (int)lroundf(sunrise_utc + (float)tz_offset_minutes);
    int sunset_local = (int)lroundf(sunset_utc + (float)tz_offset_minutes);

    *sunrise_out = wrap_minutes(sunrise_local);
    *sunset_out = wrap_minutes(sunset_local);

    if (*sunset_out <= *sunrise_out) {
        *sunrise_out = DEFAULT_SUNRISE_MINUTES;
        *sunset_out = DEFAULT_SUNSET_MINUTES;
    }
}

static void draw_sky(uint16_t *fb, int center_x, int center_y, const struct tm *local_tm)
{
    int disc_radius = (LCD_H_RES / 2) - SKY_DISC_INSET;
    int radius_sq = disc_radius * disc_radius;
    uint16_t minute_colors[MINUTES_PER_DAY];
    uint8_t minute_star_visibility[MINUTES_PER_DAY];

    build_sky_palette_for_day(local_tm, minute_colors, minute_star_visibility);

    for (int yy = 0; yy < LCD_V_RES; yy++) {
        for (int xx = 0; xx < LCD_H_RES; xx++) {
            int x = xx - center_x;
            int y = yy - center_y;
            int minute = angle_to_wall_minutes(point_angle_radians(xx, yy, center_x, center_y));
            uint16_t color = minute_colors[minute];
            uint8_t star_visibility = minute_star_visibility[minute];

            if ((x * x) + (y * y) > radius_sq) {
                color = scale_rgb565(color, 60);
            }

            fb[(yy * LCD_H_RES) + xx] = color;

            if (star_visibility > 0U) {
                uint32_t star_seed = hash_u32((uint32_t)(xx * 73856093U)
                    ^ (uint32_t)(yy * 19349663U)
                    ^ (uint32_t)(minute * 83492791U));

                if ((star_seed & 0xFFFFU) < (uint32_t)(32U + ((uint32_t)star_visibility * 96U) / 255U)) {
                    fb[(yy * LCD_H_RES) + xx] = COLOR_WHITE;

                    if (((star_seed >> 16) & 0x7U) == 0U) {
                        blend_pixel(fb, xx - 1, yy, COLOR_STAR_SOFT, 160);
                        blend_pixel(fb, xx + 1, yy, COLOR_STAR_SOFT, 160);
                        blend_pixel(fb, xx, yy - 1, COLOR_STAR_SOFT, 160);
                        blend_pixel(fb, xx, yy + 1, COLOR_STAR_SOFT, 160);
                    }
                }
            }
        }
    }

    draw_circle_outline(fb, center_x, center_y, disc_radius - 1, 2, COLOR_DARK_GRAY);
}

static void draw_hour_markers(uint16_t *fb, int center_x, int center_y)
{
    for (int hour = 0; hour < 24; hour++) {
        int outer_x;
        int outer_y;
        int inner_x;
        int inner_y;
        int label_x;
        int label_y;

        point_on_circle(center_x, center_y, TICK_OUTER_RADIUS, hour * 60, &outer_x, &outer_y);
        point_on_circle(center_x, center_y, TICK_INNER_EVEN_RADIUS, hour * 60, &inner_x, &inner_y);

        if ((hour % 2) == 0) {
            char label[3];
            int display_hour = (hour == 0) ? 24 : hour;
            snprintf(label, sizeof(label), "%d", display_hour);

            draw_line_thick(fb, inner_x, inner_y, outer_x, outer_y, 4, COLOR_BLACK);
            draw_line_thick(fb, inner_x, inner_y, outer_x, outer_y, 2, COLOR_WHITE);

            point_on_circle(center_x, center_y, HOUR_NUMBER_RADIUS, hour * 60, &label_x, &label_y);
            draw_text_centered_outline(fb, label_x, label_y - (INFO_FONT_HEIGHT / 2), label,
                                       INFO_GLYPHS, INFO_GLYPH_COUNT,
                                       INFO_FONT_WIDTH, INFO_FONT_HEIGHT, INFO_FONT_ADVANCE,
                                       COLOR_WHITE, COLOR_BLACK);
        } else {
            point_on_circle(center_x, center_y, TICK_INNER_ODD_RADIUS, hour * 60, &inner_x, &inner_y);
            draw_line_thick(fb, inner_x, inner_y, outer_x, outer_y, 2, COLOR_WHITE);
        }
    }
}

static void draw_quarter_hour_markers(uint16_t *fb, int center_x, int center_y)
{
    for (int minute = 0; minute < MINUTES_PER_DAY; minute += 15) {
        if ((minute % 60) == 0) {
            continue;
        }

        int inner_x;
        int inner_y;
        int outer_x;
        int outer_y;

        point_on_circle(center_x, center_y, TICK_INNER_ODD_RADIUS, minute, &inner_x, &inner_y);
        point_on_circle(center_x, center_y, TICK_OUTER_RADIUS, minute, &outer_x, &outer_y);
        draw_line_thick(fb, inner_x, inner_y, outer_x, outer_y, 1, COLOR_WHITE);
    }
}

static void draw_time_hand(uint16_t *fb, int center_x, int center_y, int current_minutes)
{
    int base_x;
    int base_y;
    int tip_x;
    int tip_y;

    point_on_circle(center_x, center_y, CENTER_DISC_RADIUS + HAND_BASE_GAP, current_minutes, &base_x, &base_y);
    point_on_circle(center_x, center_y, TICK_OUTER_RADIUS, current_minutes, &tip_x, &tip_y);
    draw_line_thick(fb, base_x, base_y, tip_x, tip_y, 5, COLOR_BLACK);
    draw_line_thick(fb, base_x, base_y, tip_x, tip_y, 3, COLOR_WHITE);
}

static void draw_sun_or_moon(uint16_t *fb, int center_x, int center_y, int current_minutes, int sunrise_min, int sunset_min)
{
    int x;
    int y;

    point_on_circle(center_x, center_y, SUN_DOT_ORBIT_RADIUS, current_minutes, &x, &y);

    if (current_minutes >= sunrise_min && current_minutes <= sunset_min) {
        fill_circle(fb, x, y, SUN_DOT_RADIUS + 4, COLOR_SUN_GLOW);
        fill_circle(fb, x, y, SUN_DOT_RADIUS, COLOR_SUN_FACE);
        fill_circle(fb, x, y, SUN_DOT_RADIUS - 4, COLOR_WHITE);
    } else {
        fill_circle(fb, x, y, SUN_DOT_RADIUS - 1, COLOR_MOON_LIGHT);
        fill_circle(fb, x + 5, y - 2, SUN_DOT_RADIUS - 3, COLOR_MOON_SHADOW);
    }
}

static void draw_center_disc(uint16_t *fb, int center_x, int center_y)
{
    fill_circle(fb, center_x, center_y, CENTER_DISC_RADIUS, COLOR_OXFORD_BLUE);
    fill_circle(fb, center_x, center_y, CENTER_DISC_RADIUS - 6, COLOR_BLACK);
    draw_circle_outline(fb, center_x, center_y, CENTER_DISC_RADIUS, 2, COLOR_DARK_GRAY);
}

static void draw_time_text(uint16_t *fb, const struct tm *local_tm, const render_state_t *state)
{
    char time_text[9];
    char day_text[16];
    char night_text[16];
    int center_y = LCD_V_RES / 2;
    int line_gap = 12;
    int time_y;
    int day_y;
    int night_y;
    int daylight_minutes;
    int daylight_hours;
    int daylight_remainder_minutes;
    int night_minutes;
    int night_hours;
    int night_remainder_minutes;

    strftime(time_text, sizeof(time_text), "%H:%M:%S", local_tm);
    daylight_minutes = state->sunset_minutes - state->sunrise_minutes;
    if (daylight_minutes < 0) {
        daylight_minutes += MINUTES_PER_DAY;
    }
    night_minutes = MINUTES_PER_DAY - daylight_minutes;
    daylight_hours = daylight_minutes / 60;
    daylight_remainder_minutes = daylight_minutes % 60;
    night_hours = night_minutes / 60;
    night_remainder_minutes = night_minutes % 60;
    snprintf(day_text, sizeof(day_text), "%dh %02dm", daylight_hours, daylight_remainder_minutes);
    snprintf(night_text, sizeof(night_text), "%dh %02dm", night_hours, night_remainder_minutes);

    time_y = center_y - (TIME_FONT_HEIGHT / 2);
    day_y = time_y - INFO_FONT_HEIGHT - line_gap;
    night_y = time_y + TIME_FONT_HEIGHT + line_gap;

    draw_text_centered_outline(fb, LCD_H_RES / 2, day_y,
                               day_text,
                               INFO_GLYPHS, INFO_GLYPH_COUNT,
                               INFO_FONT_WIDTH, INFO_FONT_HEIGHT, INFO_FONT_ADVANCE,
                               COLOR_PICTON_BLUE, COLOR_BLACK);

    draw_text_centered_outline(fb, LCD_H_RES / 2, time_y,
                               time_text,
                               TIME_GLYPHS, TIME_GLYPH_COUNT,
                               TIME_FONT_WIDTH, TIME_FONT_HEIGHT, TIME_FONT_ADVANCE,
                               COLOR_WHITE, COLOR_BLACK);

    draw_text_centered_outline(fb, LCD_H_RES / 2, night_y,
                               night_text,
                               INFO_GLYPHS, INFO_GLYPH_COUNT,
                               INFO_FONT_WIDTH, INFO_FONT_HEIGHT, INFO_FONT_ADVANCE,
                               COLOR_LIGHT_GRAY, COLOR_BLACK);
}

static void render_static_background(uint16_t *fb, const struct tm *local_tm)
{
    int center_x = LCD_H_RES / 2;
    int center_y = LCD_V_RES / 2;

    clear_frame(fb, COLOR_BLACK);
    draw_sky(fb, center_x, center_y, local_tm);
    draw_quarter_hour_markers(fb, center_x, center_y);
    draw_hour_markers(fb, center_x, center_y);
}

static void render_watchface(uint16_t *fb, const struct tm *local_tm, const render_state_t *state)
{
    int center_x = LCD_H_RES / 2;
    int center_y = LCD_V_RES / 2;
    int current_minutes = (local_tm->tm_hour * 60) + local_tm->tm_min;

    copy_frame(fb, s_static_background);
    draw_time_hand(fb, center_x, center_y, current_minutes);
    draw_sun_or_moon(fb, center_x, center_y, current_minutes,
                     state->sunrise_minutes, state->sunset_minutes);
    draw_center_disc(fb, center_x, center_y);
    draw_time_text(fb, local_tm, state);
}

static esp_lcd_panel_io_handle_t new_st7701_io(void)
{
    spi_line_config_t line_config = {
        .cs_io_type = IO_TYPE_GPIO,
        .cs_gpio_num = LCD_PIN_SPI_CS,
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = LCD_PIN_SPI_SCL,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = LCD_PIN_SPI_SDA,
        .io_expander = NULL,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_config = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
    esp_lcd_panel_io_handle_t io_handle = NULL;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io_config, &io_handle));
    return io_handle;
}

static esp_lcd_rgb_timing_t make_panel_timing(void)
{
    esp_lcd_rgb_timing_t timing = {
        .pclk_hz = LCD_PCLK_HZ,
        .h_res = LCD_H_RES,
        .v_res = LCD_V_RES,
        .hsync_pulse_width = 8,
        .hsync_back_porch = 50,
        .hsync_front_porch = 10,
        .vsync_pulse_width = 8,
        .vsync_back_porch = 20,
        .vsync_front_porch = 10,
        .flags = {
            .hsync_idle_low = false,
            .vsync_idle_low = false,
            .de_idle_high = false,
            .pclk_active_neg = false,
            .pclk_idle_high = false,
        },
    };

    return timing;
}

static esp_lcd_rgb_panel_config_t make_rgb_config(void)
{
    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = make_panel_timing(),
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 0,
        .bounce_buffer_size_px = LCD_BOUNCE_PIXELS,
        .dma_burst_size = 64,
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .de_gpio_num = LCD_PIN_DE,
        .pclk_gpio_num = LCD_PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            4, 5, 6, 7, 15,
            8, 20, 3, 46, 9, 10,
            11, 12, 13, 14, 0,
        },
        .flags = {
            .fb_in_psram = 0,
            .no_fb = 1,
        },
    };

    return rgb_config;
}

static bool panel_on_bounce_empty(esp_lcd_panel_handle_t panel, void *bounce_buf, int pos_px, int len_bytes, void *user_ctx)
{
    uint16_t *dst = (uint16_t *)bounce_buf;
    const uint16_t *frame = (const uint16_t *)s_active_frame;
    int lines = len_bytes / (LCD_BYTES_PER_PIXEL * LCD_H_RES);
    int start_line = line_from_pos(pos_px);

    (void)panel;
    (void)user_ctx;

    for (int row = 0; row < lines; row++) {
        int y = start_line + row;
        if (y >= LCD_V_RES) {
            y -= LCD_V_RES;
        }
        copy_row_480(dst + (row * LCD_H_RES), frame + (y * LCD_H_RES));
    }

    return false;
}

static void send_st7701_init_only(void)
{
    esp_lcd_panel_io_handle_t io_handle = new_st7701_io();
    esp_lcd_rgb_panel_config_t rgb_config = make_rgb_config();
    st7701_vendor_config_t vendor_config = {
        .rgb_config = &rgb_config,
        .init_cmds = s_st7701_type9_init_ops,
        .init_cmds_size = sizeof(s_st7701_type9_init_ops) / sizeof(s_st7701_type9_init_ops[0]),
        .flags = {
            .enable_io_multiplex = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    esp_lcd_panel_handle_t init_panel = NULL;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io_handle, &panel_config, &init_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(init_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(init_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_del(init_panel));
}

static esp_lcd_panel_handle_t new_rgb_panel_no_fb(void)
{
    esp_lcd_rgb_panel_config_t rgb_config = make_rgb_config();
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_bounce_empty = panel_on_bounce_empty,
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&rgb_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, NULL));
    return panel;
}

static void configure_backlight(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 150,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel_cfg = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1023,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
}

static void init_display(void)
{
    send_st7701_init_only();
    s_panel = new_rgb_panel_no_fb();
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
}

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    if (s_event_group) {
        xEventGroupSetBits(s_event_group, TIME_SYNC_BIT);
    }
}

static void start_sntp(void)
{
    if (esp_sntp_enabled()) {
        return;
    }

    ESP_LOGI(TAG, "Starting SNTP");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        start_sntp();
    }
}

static void init_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config;

    memset(&wifi_config, 0, sizeof(wifi_config));
    memcpy(wifi_config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void init_time_zone(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
}

static void init_storage(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void render_task(void *arg)
{
    render_state_t state = {
        .sunrise_minutes = DEFAULT_SUNRISE_MINUTES,
        .sunset_minutes = DEFAULT_SUNSET_MINUTES,
    };
    int last_yday = -1;
    int last_second = -1;

    (void)arg;

    while (true) {
        time_t now = time(NULL);
        struct tm local_tm;
        EventBits_t bits = xEventGroupGetBits(s_event_group);

        localtime_r(&now, &local_tm);

        if (local_tm.tm_yday != last_yday) {
            compute_sun_times_for_date(&local_tm, &state.sunrise_minutes, &state.sunset_minutes);
            render_static_background(s_static_background, &local_tm);
            last_yday = local_tm.tm_yday;
            ESP_LOGI(TAG, "Aalborg sun times: %02d:%02d / %02d:%02d",
                     state.sunrise_minutes / 60, state.sunrise_minutes % 60,
                     state.sunset_minutes / 60, state.sunset_minutes % 60);
        }

        if (local_tm.tm_sec != last_second || (bits & TIME_SYNC_BIT) != 0) {
            render_watchface(s_frame_back, &local_tm, &state);
            s_active_frame = s_frame_back;

            if (s_frame_back == s_frame_front) {
                s_frame_back = s_frame_front + (LCD_H_RES * LCD_V_RES);
            } else {
                s_frame_back = s_frame_front;
            }

            last_second = local_tm.tm_sec;
            xEventGroupClearBits(s_event_group, TIME_SYNC_BIT);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    size_t frame_pixels = LCD_H_RES * LCD_V_RES;
    size_t buffer_size = frame_pixels * LCD_BYTES_PER_PIXEL * 2;

    ESP_LOGI(TAG, "Booting Sunset");
    s_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_event_group ? ESP_OK : ESP_ERR_NO_MEM);

    s_frame_front = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_frame_front ? ESP_OK : ESP_ERR_NO_MEM);

    s_static_background = heap_caps_malloc(frame_pixels * LCD_BYTES_PER_PIXEL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_static_background ? ESP_OK : ESP_ERR_NO_MEM);

    s_frame_back = s_frame_front + frame_pixels;
    s_active_frame = s_frame_front;

    init_storage();
    init_time_zone();
    configure_backlight();

    clear_frame(s_frame_front, COLOR_BLACK);
    clear_frame(s_frame_back, COLOR_BLACK);
    clear_frame(s_static_background, COLOR_BLACK);
    init_display();
    init_wifi();

    xTaskCreate(render_task, "render_task", 8192, NULL, 5, NULL);
}
