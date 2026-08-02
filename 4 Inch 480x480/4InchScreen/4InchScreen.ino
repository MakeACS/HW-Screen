#include <Arduino.h>
#include <Wire.h>
#include "WS_CH32_IO.h"
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>

// ==========================================
// 0. STRUCTS & PROTOTYPES
// ==========================================
struct GroupedCard {
  String device_name;
  String state;
  int time_left;
  int duration;
  String reason;
};

struct Announcement {
  String title;
  String description;
};

struct SSDevice {
  String name;
  String state;
  String color;
};

struct DayHours {
  bool active = false;
  String open;
  String close;
  bool closed;
};

enum DisplayMode { MODE_STARTUP,
                   MODE_MAIN,
                   MODE_OTA,
                   MODE_INFO,
                   MODE_SCREENSAVER,
                   MODE_FAULT };

long get_sort_score(const GroupedCard &g);
void set_display_mode(DisplayMode mode);
void update_screensaver_content();

// ==========================================
// LOCAL LVGL CONFIGURATION
// ==========================================
#define LV_CONF_SKIP
#include "lv_conf.h"
#include <lvgl.h>

#define LCD_WIDTH 480
#define LCD_HEIGHT 480
#define MAX_CHANNELS 4
#define MAX_ANNOUNCEMENTS 5

#define ACS_RX_PIN 44
#define ACS_TX_PIN 43

// ==========================================
// 1. HARDWARE INIT
// ==========================================
Arduino_DataBus *bus = new Arduino_SWSPI(
  GFX_NOT_DEFINED /* DC */, 42 /* CS */,
  2 /* SCK */, 1 /* MOSI */, GFX_NOT_DEFINED /* MISO */
);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40 /* DE */, 39 /* VSYNC */, 38 /* HSYNC */, 41 /* PCLK */,
  46 /* R0 */, 3 /* R1 */, 8 /* R2 */, 18 /* R3 */, 17 /* R4 */,
  14 /* G0 */, 13 /* G1 */, 12 /* G2 */, 11 /* G3 */, 10 /* G4 */, 9 /* G5 */,
  5 /* B0 */, 45 /* B1 */, 48 /* B2 */, 47 /* B3 */, 21 /* B4 */,
  1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
  1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  LCD_WIDTH, LCD_HEIGHT, rgbpanel, 2 /* rotation */, true /* auto_flush */,
  bus, GFX_NOT_DEFINED /* RST */, st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

static uint8_t *draw_buf;

// ==========================================
// 2. LVGL FLUSH CALLBACK
// ==========================================
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);
}

// ==========================================
// 3. UI GLOBALS & SETUP
// ==========================================
lv_obj_t *card_container;
lv_obj_t *channel_cards[MAX_CHANNELS];
lv_obj_t *channel_device_labels[MAX_CHANNELS];
lv_obj_t *channel_state_time_rows[MAX_CHANNELS];
lv_obj_t *channel_state_labels[MAX_CHANNELS];
lv_obj_t *channel_time_labels[MAX_CHANNELS];
lv_obj_t *channel_hint_labels[MAX_CHANNELS];
lv_obj_t *channel_bars[MAX_CHANNELS];

lv_obj_t *startup_screen;
lv_obj_t *startup_label;

lv_obj_t *ota_screen;
lv_obj_t *ota_bar;
lv_obj_t *ota_label;

lv_obj_t *info_screen;
lv_obj_t *info_device_name_label;
lv_obj_t *info_station_name_label;
lv_obj_t *info_device_labels[MAX_CHANNELS];

lv_obj_t *screensaver_screen;
lv_obj_t *ss_content_container;

lv_obj_t *ss_top_card;
lv_obj_t *ss_top_label;
lv_obj_t *ss_top_station_label;
lv_obj_t *ss_top_table;

lv_obj_t *ss_center_card;
lv_obj_t *ss_center_title;
lv_obj_t *ss_center_label;
lv_obj_t *ss_hours_container;
lv_obj_t *ss_hours_day_labels[7];
lv_obj_t *ss_hours_time_labels[7];

lv_obj_t *ss_bottom_card;
lv_obj_t *ss_bottom_label;
lv_obj_t *ss_bottom_station_label;
lv_obj_t *ss_bottom_table;

lv_obj_t *fault_screen;
lv_obj_t *fault_label;

DisplayMode current_mode = MODE_STARTUP;
String global_url = "make.rit.edu";
String global_station_name = "Makerspace Equipment";
String global_device_name = "Makerspace Access Reader";

Announcement announcements[MAX_ANNOUNCEMENTS];
int num_announcements = 0;

DayHours week_hours[7];
bool has_hours = false;

int ss_cycle = 0;
SSDevice ss_devices[MAX_CHANNELS];
int ss_device_count = 0;

void set_display_mode(DisplayMode mode) {
  if (current_mode == mode) return;

  lv_obj_add_flag(startup_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(card_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ota_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(info_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(screensaver_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(fault_screen, LV_OBJ_FLAG_HIDDEN);

  if (mode == MODE_STARTUP) lv_obj_remove_flag(startup_screen, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_MAIN) lv_obj_remove_flag(card_container, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_OTA) lv_obj_remove_flag(ota_screen, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_INFO) lv_obj_remove_flag(info_screen, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_SCREENSAVER) {
    lv_obj_remove_flag(screensaver_screen, LV_OBJ_FLAG_HIDDEN);
    update_screensaver_content();
  } else if (mode == MODE_FAULT) lv_obj_remove_flag(fault_screen, LV_OBJ_FLAG_HIDDEN);

  current_mode = mode;
}

void build_ui() {
  lv_obj_t *main_screen = lv_screen_active();
  lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x1a1a1a), LV_PART_MAIN);

  // --- 1. MAIN CARD CONTAINER ---
  card_container = lv_obj_create(main_screen);
  lv_obj_set_size(card_container, 480, 480);
  lv_obj_center(card_container);
  lv_obj_set_style_bg_opa(card_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(card_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card_container, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(card_container, 15, LV_PART_MAIN);
  lv_obj_set_layout(card_container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(card_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(card_container, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < MAX_CHANNELS; i++) {
    channel_cards[i] = lv_obj_create(card_container);
    lv_obj_set_width(channel_cards[i], LV_PCT(100));
    lv_obj_set_flex_grow(channel_cards[i], 1);
    lv_obj_set_style_bg_color(channel_cards[i], lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_border_width(channel_cards[i], 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(channel_cards[i], 15, LV_PART_MAIN);
    lv_obj_add_flag(channel_cards[i], LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_layout(channel_cards[i], LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(channel_cards[i], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(channel_cards[i], LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    channel_device_labels[i] = lv_label_create(channel_cards[i]);
    lv_label_set_text(channel_device_labels[i], "Device");
    lv_obj_set_style_text_font(channel_device_labels[i], &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(channel_device_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_long_mode(channel_device_labels[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_width(channel_device_labels[i], LV_PCT(100));
    lv_obj_set_style_text_align(channel_device_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    channel_state_time_rows[i] = lv_obj_create(channel_cards[i]);
    lv_obj_set_width(channel_state_time_rows[i], LV_PCT(100));
    lv_obj_set_height(channel_state_time_rows[i], LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(channel_state_time_rows[i], LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(channel_state_time_rows[i], 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(channel_state_time_rows[i], 0, LV_PART_MAIN);
    lv_obj_set_layout(channel_state_time_rows[i], LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(channel_state_time_rows[i], LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(channel_state_time_rows[i], LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    channel_state_labels[i] = lv_label_create(channel_state_time_rows[i]);
    lv_label_set_text(channel_state_labels[i], "--");
    lv_obj_set_style_text_font(channel_state_labels[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_long_mode(channel_state_labels[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_width(channel_state_labels[i], LV_PCT(70));

    channel_time_labels[i] = lv_label_create(channel_state_time_rows[i]);
    lv_label_set_text(channel_time_labels[i], "00:00");
    lv_obj_set_style_text_font(channel_time_labels[i], &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_align(channel_time_labels[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_width(channel_time_labels[i], LV_PCT(30));

    channel_hint_labels[i] = lv_label_create(channel_cards[i]);
    lv_label_set_text(channel_hint_labels[i], "Tap ID to refresh");
    lv_obj_set_style_text_font(channel_hint_labels[i], &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(channel_hint_labels[i], LV_PCT(100));
    lv_obj_set_style_text_align(channel_hint_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(channel_hint_labels[i], LV_OBJ_FLAG_HIDDEN);

    channel_bars[i] = lv_bar_create(channel_cards[i]);
    lv_obj_set_size(channel_bars[i], LV_PCT(100), 14);
    lv_obj_set_style_bg_color(channel_bars[i], lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(channel_bars[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(channel_bars[i], 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(channel_bars[i], 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(channel_bars[i], lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_border_opa(channel_bars[i], LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(channel_bars[i], 6, LV_PART_INDICATOR);
    lv_bar_set_range(channel_bars[i], 0, 100);
    lv_bar_set_value(channel_bars[i], 0, LV_ANIM_OFF);
  }

  // --- 2-4: STARTUP, OTA, INFO ---
  startup_screen = lv_obj_create(main_screen);
  lv_obj_set_size(startup_screen, 480, 480);
  lv_obj_center(startup_screen);
  lv_obj_set_style_bg_color(startup_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_border_width(startup_screen, 0, LV_PART_MAIN);

  startup_label = lv_label_create(startup_screen);
  lv_label_set_text(startup_label, "Waiting for Core...");
  lv_obj_set_style_text_font(startup_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(startup_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(startup_label);

  ota_screen = lv_obj_create(main_screen);
  lv_obj_set_size(ota_screen, 480, 480);
  lv_obj_center(ota_screen);
  lv_obj_set_style_bg_color(ota_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_border_width(ota_screen, 0, LV_PART_MAIN);
  lv_obj_add_flag(ota_screen, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *ota_title = lv_label_create(ota_screen);
  lv_label_set_text(ota_title, "Updating...");
  lv_obj_set_style_text_font(ota_title, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ota_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(ota_title, LV_ALIGN_TOP_MID, 0, 150);

  ota_bar = lv_bar_create(ota_screen);
  lv_obj_set_size(ota_bar, 400, 30);
  lv_obj_center(ota_bar);
  lv_bar_set_range(ota_bar, 0, 100);
  lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x101010), LV_PART_MAIN);
  lv_obj_set_style_border_width(ota_bar, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(ota_bar, lv_color_hex(0x555555), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);

  ota_label = lv_label_create(ota_screen);
  lv_label_set_text(ota_label, "0%");
  lv_obj_set_style_text_font(ota_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(ota_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(ota_label, LV_ALIGN_BOTTOM_MID, 0, -180);

  info_screen = lv_obj_create(main_screen);
  lv_obj_set_size(info_screen, 480, 480);
  lv_obj_center(info_screen);
  lv_obj_set_style_bg_color(info_screen, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
  lv_obj_set_style_border_width(info_screen, 0, LV_PART_MAIN);
  lv_obj_set_layout(info_screen, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(info_screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(info_screen, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(info_screen, 20, LV_PART_MAIN);
  lv_obj_add_flag(info_screen, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *info_title_label = lv_label_create(info_screen);
  lv_label_set_text(info_title_label, "Deployment Info");
  lv_obj_set_style_text_font(info_title_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(info_title_label, lv_color_hex(0x00FF00), LV_PART_MAIN);

  info_station_name_label = lv_label_create(info_screen);
  lv_label_set_text(info_station_name_label, ("Station Name: " + global_station_name).c_str());
  lv_obj_set_style_text_font(info_station_name_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(info_station_name_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  info_device_name_label = lv_label_create(info_screen);
  lv_label_set_text(info_device_name_label, ("ACS Device Name: " + global_device_name).c_str());
  lv_obj_set_style_text_font(info_device_name_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(info_device_name_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(info_device_name_label, 10, LV_PART_MAIN);

  lv_obj_t *info_list = lv_obj_create(info_screen);
  lv_obj_set_width(info_list, LV_PCT(100));
  lv_obj_set_flex_grow(info_list, 1);
  lv_obj_set_style_bg_opa(info_list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(info_list, 0, LV_PART_MAIN);
  lv_obj_set_layout(info_list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(info_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_top(info_list, 10, LV_PART_MAIN);

  for (int i = 0; i < MAX_CHANNELS; i++) {
    info_device_labels[i] = lv_label_create(info_list);
    lv_label_set_text(info_device_labels[i], "");
    lv_obj_set_style_text_font(info_device_labels[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(info_device_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_long_mode(info_device_labels[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info_device_labels[i], LV_PCT(100));
    lv_obj_set_style_pad_bottom(info_device_labels[i], 12, LV_PART_MAIN);
  }

  lv_obj_t *info_footer = lv_label_create(info_screen);
  lv_label_set_text(info_footer, "Press and hold button for\n5 seconds to restart");
  lv_obj_set_style_text_font(info_footer, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(info_footer, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
  lv_label_set_long_mode(info_footer, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(info_footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(info_footer, LV_PCT(100));

  // --- 5. REDESIGNED SCREENSAVER OVERLAY ---
  screensaver_screen = lv_obj_create(main_screen);
  lv_obj_set_size(screensaver_screen, 480, 480);
  lv_obj_center(screensaver_screen);
  lv_obj_set_style_bg_color(screensaver_screen, lv_color_hex(0x121212), LV_PART_MAIN);
  lv_obj_set_style_border_width(screensaver_screen, 0, LV_PART_MAIN);
  lv_obj_remove_flag(screensaver_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(screensaver_screen, LV_OBJ_FLAG_HIDDEN);

  ss_content_container = lv_obj_create(screensaver_screen);
  lv_obj_set_size(ss_content_container, 460, 460);
  lv_obj_center(ss_content_container);
  lv_obj_set_style_bg_opa(ss_content_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_content_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ss_content_container, 0, LV_PART_MAIN);
  lv_obj_remove_flag(ss_content_container, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_set_layout(ss_content_container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_content_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ss_content_container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // TOP BLOCK (23%)
  ss_top_card = lv_obj_create(ss_content_container);
  lv_obj_set_size(ss_top_card, LV_PCT(100), LV_PCT(23));
  lv_obj_set_style_bg_opa(ss_top_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_top_card, 0, LV_PART_MAIN);
  lv_obj_remove_flag(ss_top_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(ss_top_card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_top_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ss_top_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  ss_top_label = lv_label_create(ss_top_card);
  lv_label_set_text(ss_top_label, "");
  lv_obj_set_style_text_font(ss_top_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_top_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_long_mode(ss_top_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ss_top_label, LV_PCT(100));
  lv_obj_set_style_text_align(ss_top_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  ss_top_station_label = lv_label_create(ss_top_card);
  lv_label_set_text(ss_top_station_label, "");
  lv_obj_set_style_text_font(ss_top_station_label, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_top_station_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_long_mode(ss_top_station_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ss_top_station_label, LV_PCT(100));
  lv_obj_set_style_text_align(ss_top_station_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_flag(ss_top_station_label, LV_OBJ_FLAG_HIDDEN);

  // Top Device Table
  ss_top_table = lv_obj_create(ss_top_card);
  lv_obj_set_size(ss_top_table, LV_PCT(90), LV_PCT(100));
  lv_obj_set_style_bg_opa(ss_top_table, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_top_table, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ss_top_table, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(ss_top_table, 0, LV_PART_MAIN);
  lv_obj_set_layout(ss_top_table, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_top_table, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ss_top_table, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(ss_top_table, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ss_top_table, LV_OBJ_FLAG_HIDDEN);

  // CENTER BLOCK (54%)
  ss_center_card = lv_obj_create(ss_content_container);
  lv_obj_set_size(ss_center_card, LV_PCT(100), LV_PCT(54));
  lv_obj_set_style_bg_opa(ss_center_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_center_card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ss_center_card, 15, LV_PART_MAIN);
  lv_obj_remove_flag(ss_center_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(ss_center_card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_center_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ss_center_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  ss_center_title = lv_label_create(ss_center_card);
  lv_label_set_text(ss_center_title, "Welcome to the Makerspace!");
  lv_obj_set_style_text_font(ss_center_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_center_title, lv_color_hex(0xFFA500), LV_PART_MAIN);
  lv_label_set_long_mode(ss_center_title, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ss_center_title, LV_PCT(100));
  lv_obj_set_style_text_align(ss_center_title, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(ss_center_title, 10, LV_PART_MAIN);

  ss_center_label = lv_label_create(ss_center_card);
  lv_label_set_text(ss_center_label, "");
  lv_obj_set_style_text_font(ss_center_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_center_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_long_mode(ss_center_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ss_center_label, LV_PCT(100));
  lv_obj_set_style_text_align(ss_center_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

  // Center Hours Table (Custom tabular flex container)
  ss_hours_container = lv_obj_create(ss_center_card);
  lv_obj_set_width(ss_hours_container, LV_PCT(100));
  lv_obj_set_height(ss_hours_container, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(ss_hours_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_hours_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ss_hours_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(ss_hours_container, 4, LV_PART_MAIN);
  lv_obj_set_layout(ss_hours_container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_hours_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(ss_hours_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ss_hours_container, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < 7; i++) {
    lv_obj_t *row = lv_obj_create(ss_hours_container);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ss_hours_day_labels[i] = lv_label_create(row);
    lv_obj_set_style_text_font(ss_hours_day_labels[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ss_hours_day_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    ss_hours_time_labels[i] = lv_label_create(row);
    lv_obj_set_style_text_font(ss_hours_time_labels[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ss_hours_time_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  }

  // BOTTOM BLOCK (23%)
  ss_bottom_card = lv_obj_create(ss_content_container);
  lv_obj_set_size(ss_bottom_card, LV_PCT(100), LV_PCT(23));
  lv_obj_set_style_bg_opa(ss_bottom_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_bottom_card, 0, LV_PART_MAIN);
  lv_obj_remove_flag(ss_bottom_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(ss_bottom_card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_bottom_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ss_bottom_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  ss_bottom_label = lv_label_create(ss_bottom_card);
  lv_label_set_text(ss_bottom_label, "");
  lv_obj_set_style_text_font(ss_bottom_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_bottom_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_long_mode(ss_bottom_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ss_bottom_label, LV_PCT(100));
  lv_obj_set_style_text_align(ss_bottom_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  ss_bottom_station_label = lv_label_create(ss_bottom_card);
  lv_label_set_text(ss_bottom_station_label, "");
  lv_obj_set_style_text_font(ss_bottom_station_label, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_bottom_station_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_long_mode(ss_bottom_station_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ss_bottom_station_label, LV_PCT(100));
  lv_obj_set_style_text_align(ss_bottom_station_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_flag(ss_bottom_station_label, LV_OBJ_FLAG_HIDDEN);

  // Bottom Device Table
  ss_bottom_table = lv_obj_create(ss_bottom_card);
  lv_obj_set_size(ss_bottom_table, LV_PCT(90), LV_PCT(100));
  lv_obj_set_style_bg_opa(ss_bottom_table, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_bottom_table, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ss_bottom_table, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(ss_bottom_table, 0, LV_PART_MAIN);
  lv_obj_set_layout(ss_bottom_table, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_bottom_table, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ss_bottom_table, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(ss_bottom_table, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ss_bottom_table, LV_OBJ_FLAG_HIDDEN);

  // --- 6. FAULT OVERLAY ---
  fault_screen = lv_obj_create(main_screen);
  lv_obj_set_size(fault_screen, 480, 480);
  lv_obj_center(fault_screen);
  lv_obj_set_style_bg_color(fault_screen, lv_color_hex(0xCC0000), LV_PART_MAIN);
  lv_obj_set_style_border_width(fault_screen, 0, LV_PART_MAIN);
  lv_obj_add_flag(fault_screen, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *fault_title = lv_label_create(fault_screen);
  lv_label_set_text(fault_title, "FAULT!");
  lv_obj_set_style_text_font(fault_title, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(fault_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(fault_title, LV_ALIGN_TOP_MID, 0, 100);

  fault_label = lv_label_create(fault_screen);
  lv_label_set_text(fault_label, "Unknown error");
  lv_obj_set_style_text_font(fault_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(fault_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_long_mode(fault_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(fault_label, LV_PCT(90));
  lv_obj_set_style_text_align(fault_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(fault_label, LV_ALIGN_CENTER, 0, 0);
}

// ==========================================
// 4. DATA PROCESSING & UTILS
// ==========================================
String strip_markdown(String md) {
  String out = "";
  for (size_t i = 0; i < md.length();) {
    if (md[i] == '[') {
      i++;
      while (i < md.length() && md[i] != ']') {
        out += md[i];
        i++;
      }
      if (i < md.length() && md[i] == ']') i++;
      if (i < md.length() && md[i] == '(') {
        while (i < md.length() && md[i] != ')') i++;
        if (i < md.length() && md[i] == ')') i++;
      }
    } else if (md[i] == '*' || md[i] == '#' || md[i] == '_') {
      i++;
    } else {
      out += md[i];
      i++;
    }
  }
  return out;
}

String get_time_string() {
  char time_buf[64];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(time_buf, sizeof(time_buf), "%A, %B %d\n%I:%M %p", &timeinfo);
  } else {
    sprintf(time_buf, "Time Not Synced");
  }
  return String(time_buf);
}

// Calculates Day of the week (0=Sunday to 6=Saturday) via Zeller's congruence
int get_weekday(String iso_date) {
  if (iso_date.length() < 10) return 0;
  int y = iso_date.substring(0, 4).toInt();
  int m = iso_date.substring(5, 7).toInt();
  int d = iso_date.substring(8, 10).toInt();

  if (m < 3) {
    m += 12;
    y -= 1;
  }
  int k = y % 100;
  int j = y / 100;
  int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return (h + 6) % 7;
}

// Converts standard "19:00:00" string from JSON into "7:00 PM"
String format_hour(String time_str) {
  if (time_str.length() < 5) return time_str;
  int h = time_str.substring(0, 2).toInt();
  String m = time_str.substring(3, 5);
  String ampm = (h >= 12) ? "PM" : "AM";
  if (h == 0) h = 12;
  if (h > 12) h -= 12;
  return String(h) + ":" + m + " " + ampm;
}

void build_screensaver_device_list(lv_obj_t *table) {
  lv_obj_clean(table);

  if (ss_device_count == 0) {
    lv_obj_t *no_eq = lv_label_create(table);
    lv_label_set_text(no_eq, "No equipment available");
    lv_obj_set_style_text_color(no_eq, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(no_eq, &lv_font_montserrat_20, LV_PART_MAIN);
    return;
  }

  for (int i = 0; i < ss_device_count; i++) {
    lv_obj_t *row = lv_obj_create(table);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, ss_devices[i].name.c_str());
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_20, LV_PART_MAIN);

    lv_obj_t *state_lbl = lv_label_create(row);
    lv_label_set_text(state_lbl, ss_devices[i].state.c_str());
    uint32_t color_hex = strtoul(ss_devices[i].color.c_str(), NULL, 16);
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(color_hex), LV_PART_MAIN);
    lv_obj_set_style_text_font(state_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
  }
}

void update_screensaver_content() {
  // Outer sections swap every 6 internal cycles (6 * 30 seconds = 3 minutes)
  int outer_cycle = ss_cycle / 6;
  bool tap_at_top = (outer_cycle % 2 == 0);
  bool show_time = ((outer_cycle / 2) % 2 == 0);

  String tap_text = "#00FF00 Tap ID at top to activate#";

  // Check how many items we are rotating through in the center
  int total_center_items = num_announcements + (has_hours ? 2 : 0);
  int center_idx = (total_center_items > 0) ? (ss_cycle % total_center_items) : 0;
  bool showing_hours = (has_hours && center_idx >= num_announcements);

  // Requirement: Whenever an hours-related center card is up, show date/time outer blocks
  if (showing_hours) {
    show_time = true;
  }

  // --- Update Top Block ---
  if (tap_at_top) {
    lv_obj_remove_flag(ss_top_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ss_top_station_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ss_top_table, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ss_top_label, tap_text.c_str());
    lv_label_set_recolor(ss_top_label, true);
    lv_label_set_text(ss_top_station_label, global_station_name.c_str());
  } else {
    if (show_time) {
      lv_obj_remove_flag(ss_top_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ss_top_station_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ss_top_table, LV_OBJ_FLAG_HIDDEN);

      lv_label_set_text(ss_top_label, get_time_string().c_str());
      lv_label_set_recolor(ss_top_label, false);
    } else {
      lv_obj_add_flag(ss_top_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ss_top_station_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(ss_top_table, LV_OBJ_FLAG_HIDDEN);

      build_screensaver_device_list(ss_top_table);
    }
  }

  // --- Update Bottom Block ---
  if (!tap_at_top) {
    lv_obj_remove_flag(ss_bottom_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ss_bottom_station_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ss_bottom_table, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ss_bottom_label, tap_text.c_str());
    lv_label_set_recolor(ss_bottom_label, true);
    lv_label_set_text(ss_bottom_station_label, global_station_name.c_str());
  } else {
    if (show_time) {
      lv_obj_remove_flag(ss_bottom_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ss_bottom_station_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ss_bottom_table, LV_OBJ_FLAG_HIDDEN);

      lv_label_set_text(ss_bottom_label, get_time_string().c_str());
      lv_label_set_recolor(ss_bottom_label, false);
    } else {
      lv_obj_add_flag(ss_bottom_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ss_bottom_station_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(ss_bottom_table, LV_OBJ_FLAG_HIDDEN);

      build_screensaver_device_list(ss_bottom_table);
    }
  }

  // --- Update Center Block ---
  if (total_center_items == 0) {
    lv_obj_remove_flag(ss_center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ss_hours_container, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ss_center_title, "Welcome to the Makerspace!");
    lv_label_set_text(ss_center_label, "");
  } else if (center_idx < num_announcements) {
    // Show Announcement
    lv_obj_remove_flag(ss_center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ss_hours_container, LV_OBJ_FLAG_HIDDEN);

    String clean_desc = strip_markdown(announcements[center_idx].description);
    lv_label_set_text(ss_center_title, announcements[center_idx].title.c_str());

    if (clean_desc.length() > 140) {
      clean_desc = clean_desc.substring(0, 137) + "...";
    }
    String ann_text = clean_desc + "\n\nLearn more at " + global_url;

    lv_label_set_text(ss_center_label, ann_text.c_str());
  } else if (center_idx == num_announcements) {
    // Show Hours Screen 1: Full Week Table View
    lv_obj_add_flag(ss_center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ss_hours_container, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ss_center_title, "Makerspace Hours");

    struct tm timeinfo;
    int current_wday = -1;
    if (getLocalTime(&timeinfo)) {
      current_wday = timeinfo.tm_wday;
    }

    const char *day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

    for (int i = 0; i < 7; i++) {
      String day_str = day_names[i];
      String time_str = "Unknown";

      if (week_hours[i].active) {
        if (week_hours[i].closed) {
          time_str = "Closed";
        } else {
          time_str = format_hour(week_hours[i].open) + " - " + format_hour(week_hours[i].close);
        }
      }

      lv_label_set_text(ss_hours_day_labels[i], day_str.c_str());
      lv_label_set_text(ss_hours_time_labels[i], time_str.c_str());

      // Recolor current day row
      if (i == current_wday) {
        lv_obj_set_style_text_color(ss_hours_day_labels[i], lv_color_hex(0x00FF00), LV_PART_MAIN);
        lv_obj_set_style_text_color(ss_hours_time_labels[i], lv_color_hex(0x00FF00), LV_PART_MAIN);
      } else {
        lv_obj_set_style_text_color(ss_hours_day_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_color(ss_hours_time_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
      }
    }
  } else {
    // Show Hours Screen 2: Today's Summary
    lv_obj_remove_flag(ss_center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ss_hours_container, LV_OBJ_FLAG_HIDDEN);

    struct tm timeinfo;
    int current_wday = -1;
    if (getLocalTime(&timeinfo)) {
      current_wday = timeinfo.tm_wday;
    }

    lv_label_set_text(ss_center_title, "Today's Hours");
    String summary = "";

    if (current_wday != -1 && week_hours[current_wday].active) {
      if (week_hours[current_wday].closed) {
        summary = "The makerspace is closed today.\n\nSee more hours at " + global_url;
      } else {
        summary = "The makerspace is open today until " + format_hour(week_hours[current_wday].close) + ".\n\nSee more hours at " + global_url;
      }
    } else {
      summary = "Today's hours are currently unavailable.\n\nSee more hours at " + global_url;
    }

    lv_label_set_text(ss_center_label, summary.c_str());
  }
}

void screensaver_burnin_cb(lv_timer_t *timer) {
  if (current_mode != MODE_SCREENSAVER) return;

  ss_cycle++;
  update_screensaver_content();

  static int shift_step = 0;
  shift_step = (shift_step + 1) % 4;
  int x_off = (shift_step == 1) ? 6 : (shift_step == 3) ? -6
                                                        : 0;
  int y_off = (shift_step == 2) ? 6 : (shift_step == 0) ? -6
                                                        : 0;
  lv_obj_align(ss_content_container, LV_ALIGN_CENTER, x_off, y_off);
}

void format_time_left(int seconds, char *buffer) {
  if (seconds <= 0) {
    sprintf(buffer, "");
    return;
  }
  int h = seconds / 3600;
  int m = (seconds % 3600) / 60;
  int s = seconds % 60;
  if (h > 0) sprintf(buffer, "%02d:%02d:%02d", h, m, s);
  else sprintf(buffer, "%02d:%02d", m, s);
}

void format_hobbs_time(long seconds, char *buffer) {
  long h = seconds / 3600;
  long m = (seconds % 3600) / 60;
  sprintf(buffer, "%ldh %02ldm", h, m);
}

String translate_denied_reason(String reason) {
  if (reason == "EQUIPMENT_TRAINING") return "Missing equipment-specific training. Please visit " + global_url;
  if (reason == "ROOM_TRAINING" || reason == "MAKERSPACE_TRAINING") return "Missing makerspace-wide training. Please visit " + global_url;
  if (reason == "WELCOME") return "Please sign in at the front desk.";
  if (reason == "UNPAIRED") return "This device is not paired to an equipment?";
  if (reason == "UNKNOWN_USER") return "Card not recognized. Check in with staff.";
  if (reason == "ACTIVE_HOLD" || reason == "ACTIVE_RESTRICTION") return "Restriction on account, please speak with staff.";
  if (reason == "ARCHIVED") return "Account archived. Talk to staff for more info.";
  if (reason == "MISSING_SIGN_OFF") return "Missing equipment sign-off. Speak to staff.";

  return (reason != "null" && reason.length() > 0) ? reason : "ACCESS DENIED";
}

long get_sort_score(const GroupedCard &g) {
  if (g.state == "DENIED") return 0;
  if (g.state == "UNLOCKED" || g.state == "ACTIVE" || g.state == "ON") {
    if (g.time_left > 0) return g.time_left;
    return 999990;
  }
  if (g.state == "ALWAYS_ON") return 1000000;
  if (g.state == "IDLE") return 1000001;
  if (g.state == "LOCKED_OUT") return 1000002;
  return 999999;
}

void update_acs_display(int num_channels, String states[], int time_left[], int durations[], String devices[], String reasons[]) {
  if (num_channels > MAX_CHANNELS) num_channels = MAX_CHANNELS;

  GroupedCard groups[MAX_CHANNELS];
  int num_groups = 0;

  for (int i = 0; i < num_channels; i++) {
    bool added = false;
    for (int j = 0; j < num_groups; j++) {
      if (groups[j].time_left == time_left[i] && groups[j].duration == durations[i] && groups[j].state == states[i] && groups[j].reason == reasons[i]) {

        groups[j].device_name += "\n" + devices[i];
        added = true;
        break;
      }
    }
    if (!added) {
      groups[num_groups].device_name = devices[i];
      groups[num_groups].state = states[i];
      groups[num_groups].time_left = time_left[i];
      groups[num_groups].duration = durations[i];
      groups[num_groups].reason = reasons[i];
      num_groups++;
    }
  }

  for (int i = 0; i < num_groups - 1; i++) {
    for (int j = i + 1; j < num_groups; j++) {
      if (get_sort_score(groups[i]) > get_sort_score(groups[j])) {
        GroupedCard temp = groups[i];
        groups[i] = groups[j];
        groups[j] = temp;
      }
    }
  }

  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (i < num_groups) {
      lv_obj_remove_flag(channel_cards[i], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(channel_device_labels[i], groups[i].device_name.c_str());
      lv_obj_add_flag(channel_hint_labels[i], LV_OBJ_FLAG_HIDDEN);

      uint32_t text_color;
      uint32_t bg_color;
      char time_str[16];
      format_time_left(groups[i].time_left, time_str);

      if (groups[i].state == "DENIED") {
        text_color = 0xFF0000;
        bg_color = 0x331A1A;
        lv_label_set_text(channel_state_labels[i], groups[i].reason.c_str());
        lv_obj_add_flag(channel_time_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(channel_state_labels[i], LV_PCT(100));
        lv_obj_set_style_text_align(channel_state_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      } else if (groups[i].state == "ALWAYS_ON") {
        text_color = 0x00FF00;
        bg_color = 0x1A331A;
        lv_label_set_text(channel_state_labels[i], "ALWAYS ON");
        lv_obj_add_flag(channel_time_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(channel_state_labels[i], LV_PCT(100));
        lv_obj_set_style_text_align(channel_state_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      } else if (groups[i].state == "UNLOCKED" || groups[i].state == "ACTIVE" || groups[i].state == "ON") {
        bool is_expiring_soon = (groups[i].duration > 0) && (groups[i].time_left > 0) && (groups[i].time_left * 10 <= groups[i].duration);

        if (is_expiring_soon) {
          text_color = 0xFFA500;
          bg_color = 0x33271A;
          lv_label_set_text(channel_state_labels[i], "EXPIRING SOON");
          lv_obj_remove_flag(channel_hint_labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
          text_color = 0x00FF00;
          bg_color = 0x1A331A;
          lv_label_set_text(channel_state_labels[i], groups[i].state.c_str());
        }

        lv_label_set_text(channel_time_labels[i], time_str);
        lv_obj_remove_flag(channel_time_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(channel_state_labels[i], LV_PCT(70));
        lv_obj_set_style_text_align(channel_state_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);

      } else if (groups[i].state == "LOCKED_OUT") {
        text_color = 0xFF0000;
        bg_color = 0x331A1A;
        lv_label_set_text(channel_state_labels[i], "LOCKED OUT");
        lv_obj_add_flag(channel_time_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(channel_state_labels[i], LV_PCT(100));
        lv_obj_set_style_text_align(channel_state_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      } else if (groups[i].state == "IDLE") {
        text_color = 0xAAAAAA;
        bg_color = 0x2A2A2A;
        lv_label_set_text(channel_state_labels[i], "Tap ID at top of screen to activate");
        lv_obj_add_flag(channel_time_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(channel_state_labels[i], LV_PCT(100));
        lv_obj_set_style_text_align(channel_state_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      } else {
        text_color = 0xFFA500;
        bg_color = 0x33271A;
        lv_label_set_text(channel_state_labels[i], groups[i].state.c_str());
        lv_label_set_text(channel_time_labels[i], time_str);
        lv_obj_remove_flag(channel_time_labels[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(channel_state_labels[i], LV_PCT(70));
        lv_obj_set_style_text_align(channel_state_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
      }

      lv_obj_set_style_bg_color(channel_cards[i], lv_color_hex(bg_color), LV_PART_MAIN);
      lv_obj_set_style_text_color(channel_state_labels[i], lv_color_hex(text_color), LV_PART_MAIN);
      lv_obj_set_style_text_color(channel_time_labels[i], lv_color_hex(text_color), LV_PART_MAIN);
      lv_obj_set_style_text_color(channel_hint_labels[i], lv_color_hex(text_color), LV_PART_MAIN);
      lv_obj_set_style_bg_color(channel_bars[i], lv_color_hex(text_color), LV_PART_INDICATOR);

      if (groups[i].state == "IDLE" || groups[i].state == "LOCKED_OUT" || groups[i].state == "ALWAYS_ON" || groups[i].state == "DENIED") {
        lv_bar_set_range(channel_bars[i], 0, 100);
        lv_bar_set_value(channel_bars[i], 100, LV_ANIM_ON);
      } else if (groups[i].duration > 0) {
        lv_bar_set_range(channel_bars[i], 0, groups[i].duration);
        lv_bar_set_value(channel_bars[i], groups[i].time_left, LV_ANIM_ON);
      } else {
        lv_bar_set_range(channel_bars[i], 0, 100);
        lv_bar_set_value(channel_bars[i], 100, LV_ANIM_ON);
      }
    } else {
      lv_obj_add_flag(channel_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}


uint32_t calculate_crc32(const String &data) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < data.length(); i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
      else crc >>= 1;
    }
  }
  return ~crc;
}

void process_incoming_json(String jsonString) {
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, jsonString);
  if (error) return;

  // --- 1. Update Globals & Time ---
  if (doc.containsKey("url")) {
    global_url = doc["url"].as<String>();
  }

  if (doc.containsKey("deviceName")) {
    global_device_name = doc["deviceName"].as<String>();
    lv_label_set_text(info_device_name_label, ("ACS Device Name: " + global_device_name).c_str());
  }

  if (doc.containsKey("stationName")) {
    global_station_name = doc["stationName"].as<String>();
    lv_label_set_text(info_station_name_label, ("Station Name: " + global_station_name).c_str());
  }

  if (doc.containsKey("time")) {
    time_t raw_time = doc["time"].as<time_t>();
    if (raw_time > 1000000000000L) {
      raw_time /= 1000;
    }
    struct timeval tv;
    tv.tv_sec = raw_time;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
  }

  if (doc.containsKey("announcements")) {
    JsonArray ann_array = doc["announcements"].as<JsonArray>();
    num_announcements = 0;
    for (JsonObject ann : ann_array) {
      if (num_announcements >= MAX_ANNOUNCEMENTS) break;
      announcements[num_announcements].title = ann["title"].as<String>();
      announcements[num_announcements].description = ann["description"].as<String>();
      num_announcements++;
    }
  }

  // --- Grab Hours Information ---
  if (doc.containsKey("hours")) {
    JsonArray hrs_array = doc["hours"].as<JsonArray>();
    has_hours = false;
    for (JsonObject h_obj : hrs_array) {
      String day_str = h_obj["day"].as<String>();
      int wday = get_weekday(day_str);
      if (wday >= 0 && wday <= 6) {
        week_hours[wday].active = true;
        week_hours[wday].open = h_obj["open"].as<String>();
        week_hours[wday].close = h_obj["close"].as<String>();
        week_hours[wday].closed = h_obj["closed"].as<bool>();
        has_hours = true;
      }
    }
  }

  // --- 2. Check for Global Fault State ---
  int num_channels = doc.containsKey("channels") ? doc["channels"].as<int>() : 0;
  if (num_channels <= 0 || num_channels > MAX_CHANNELS) num_channels = MAX_CHANNELS;

  bool system_fault = false;
  for (int i = 0; i < num_channels; i++) {
    if (doc["state"][i].as<String>() == "FAULT") {
      system_fault = true;
      break;
    }
  }

  if (system_fault) {
    set_display_mode(MODE_FAULT);
    String fmsg = doc.containsKey("faultMessage") ? doc["faultMessage"].as<String>() : "System Fault Detected";
    lv_label_set_text(fault_label, fmsg.c_str());
    return;
  }

  // --- 3. Handle OTA Mode ---
  if (doc.containsKey("coreOta") || doc.containsKey("ota")) {
    set_display_mode(MODE_OTA);
    int progress = doc.containsKey("coreOta") ? doc["coreOta"].as<int>() : doc["ota"].as<int>();
    lv_bar_set_value(ota_bar, progress, LV_ANIM_ON);
    lv_label_set_text_fmt(ota_label, "%d%%", progress);
    return;
  }

  // --- 4. Handle Info Mode ---
  static unsigned long button_release_time = 0;
  bool button_pressed = doc.containsKey("button") ? doc["button"].as<bool>() : false;

  if (button_pressed) {
    button_release_time = millis();
    set_display_mode(MODE_INFO);
    return;
  } else if (current_mode == MODE_INFO) {
    if (millis() - button_release_time < 5000) {
      return;
    }
  }

  // --- 5. Handle Startup Message Mode ---
  if (doc.containsKey("startupMessage")) {
    String msg = doc["startupMessage"].as<String>();
    if (msg.length() > 0) {
      set_display_mode(MODE_STARTUP);
      lv_label_set_text(startup_label, msg.c_str());
      return;
    }
  }

  // --- 6. Equipment & Hobbs Information Parsing ---
  ss_device_count = 0;
  for (int i = 0; i < num_channels; i++) {
    String state = (doc.containsKey("state") && doc["state"][i].is<String>()) ? doc["state"][i].as<String>() : "";

    if (doc.containsKey("deviceNames") && doc["deviceNames"][i].is<String>()) {
      String dName = doc["deviceNames"][i].as<String>();

      // Save info for Screensaver Equipment Table
      ss_devices[ss_device_count].name = dName;
      ss_devices[ss_device_count].state = state;

      if (state == "LOCKED_OUT") {
        ss_devices[ss_device_count].color = "FF0000";
      } else if (state == "IDLE") {
        ss_devices[ss_device_count].color = "FFFF00";
      } else {
        ss_devices[ss_device_count].color = "00FF00";
      }
      ss_device_count++;

      // Update Info Screen Device + Hobbs Seconds Labels
      long hSecs = doc.containsKey("hobbsSeconds") ? doc["hobbsSeconds"][i].as<long>() : 0;
      char hBuf[32];
      format_hobbs_time(hSecs, hBuf);

      String infoStr = dName + "  -  " + String(hBuf);
      lv_label_set_text(info_device_labels[i], infoStr.c_str());
    } else {
      lv_label_set_text(info_device_labels[i], "");
    }
  }

  // --- 7. Determine Screen Mode & Trigger Updates ---
  bool is_denied = doc["denied"].as<bool>();
  bool all_idle_or_locked = true;

  if (is_denied) {
    all_idle_or_locked = false;
  } else {
    for (int i = 0; i < num_channels; i++) {
      String state = doc["state"][i].as<String>();
      if (state != "IDLE" && state != "LOCKED_OUT") {
        all_idle_or_locked = false;
        break;
      }
    }
  }

  if (all_idle_or_locked) {
    set_display_mode(MODE_SCREENSAVER);
    if (current_mode == MODE_SCREENSAVER) {
      update_screensaver_content();
    }
  } else {
    set_display_mode(MODE_MAIN);
  }

  String states[MAX_CHANNELS];
  int time_left[MAX_CHANNELS];
  int durations[MAX_CHANNELS];
  String devices[MAX_CHANNELS];
  String reasons[MAX_CHANNELS];

  for (int i = 0; i < num_channels; i++) {
    states[i] = doc["state"][i].as<String>();
    time_left[i] = doc["currentAuthExpires"][i].as<long>() / 1000;
    durations[i] = doc["durations"][i].as<long>() / 1000;
    devices[i] = doc["deviceNames"][i].as<String>();
    String reason = doc["deniedReason"][i].as<String>();

    if (is_denied && states[i] == "IDLE") {
      states[i] = "DENIED";
      reasons[i] = translate_denied_reason(reason);
    } else {
      reasons[i] = "";
    }
  }

  update_acs_display(num_channels, states, time_left, durations, devices, reasons);
}

// ==========================================
// 5. SETUP & LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, ACS_RX_PIN, ACS_TX_PIN);

  delay(1000);
  Wire.begin(15, 7);
  WS_CH32_IO::begin(Wire, 15, 7, WS_CH32_IO::DEFAULT_I2C_FREQ);
  gfx->begin();

  lv_init();
  lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  size_t buffer_size = (LCD_WIDTH * LCD_HEIGHT / 4) * sizeof(uint16_t);
  draw_buf = (uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  lv_display_set_buffers(disp, draw_buf, NULL, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, my_disp_flush);

  build_ui();

  lv_timer_create(screensaver_burnin_cb, 30000, NULL);
}

int brace_count = 0;
bool is_reading_json = false;
String json_buffer = "";

void loop() {
  lv_tick_inc(5);
  lv_timer_handler();

  while (Serial1.available()) {
    char c = Serial1.read();

    if (c == '{') {
      if (brace_count == 0) is_reading_json = true;
      brace_count++;
    }

    if (is_reading_json) json_buffer += c;

    if (c == '}') {
      brace_count--;
      if (brace_count == 0 && is_reading_json) {
        process_incoming_json(json_buffer);
        json_buffer = "";
        is_reading_json = false;
      }
    }
  }

  delay(5);
}