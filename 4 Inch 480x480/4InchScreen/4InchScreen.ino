#include <Arduino.h>
#include <Wire.h>
#include "WS_CH32_IO.h"
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <time.h>

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

enum DisplayMode { MODE_STARTUP,
                   MODE_MAIN,
                   MODE_OTA,
                   MODE_INFO,
                   MODE_SCREENSAVER };

long get_sort_score(const GroupedCard &g);
void set_display_mode(DisplayMode mode);

// ==========================================
// LOCAL LVGL CONFIGURATION
// ==========================================
#define LV_CONF_SKIP
#include "lv_conf.h"
#include <lvgl.h>

#define LCD_WIDTH 480
#define LCD_HEIGHT 480
#define MAX_CHANNELS 4

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

// Info Screen Globals
lv_obj_t *info_screen;
lv_obj_t *info_device_name_label;
lv_obj_t *info_device_labels[MAX_CHANNELS];

// Clock Screensaver Globals
lv_obj_t *screensaver_screen;
lv_obj_t *ss_content_container;
lv_obj_t *ss_cta_label;
lv_obj_t *ss_time_label;
lv_obj_t *ss_date_label;
lv_obj_t *ss_equip_label;

DisplayMode current_mode = MODE_STARTUP;
String global_url = "make.rit.edu";

void set_display_mode(DisplayMode mode) {
  if (current_mode == mode) return;

  lv_obj_add_flag(startup_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(card_container, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ota_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(info_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(screensaver_screen, LV_OBJ_FLAG_HIDDEN);

  if (mode == MODE_STARTUP) lv_obj_remove_flag(startup_screen, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_MAIN) lv_obj_remove_flag(card_container, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_OTA) lv_obj_remove_flag(ota_screen, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_INFO) lv_obj_remove_flag(info_screen, LV_OBJ_FLAG_HIDDEN);
  else if (mode == MODE_SCREENSAVER) lv_obj_remove_flag(screensaver_screen, LV_OBJ_FLAG_HIDDEN);

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

    // Sub-label for refresh instructions right above the progress bar
    channel_hint_labels[i] = lv_label_create(channel_cards[i]);
    lv_label_set_text(channel_hint_labels[i], "Tap ID to refresh");
    lv_obj_set_style_text_font(channel_hint_labels[i], &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(channel_hint_labels[i], LV_PCT(100));
    lv_obj_set_style_text_align(channel_hint_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_add_flag(channel_hint_labels[i], LV_OBJ_FLAG_HIDDEN);

    channel_bars[i] = lv_bar_create(channel_cards[i]);
    lv_obj_set_size(channel_bars[i], LV_PCT(100), 12);
    lv_obj_set_style_bg_color(channel_bars[i], lv_color_hex(0x444444), LV_PART_MAIN);
    lv_bar_set_range(channel_bars[i], 0, 100);
    lv_bar_set_value(channel_bars[i], 0, LV_ANIM_OFF);
  }

  // --- 2. STARTUP OVERLAY ---
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

  // --- 3. OTA OVERLAY ---
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
  lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x444444), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);

  ota_label = lv_label_create(ota_screen);
  lv_label_set_text(ota_label, "0%");
  lv_obj_set_style_text_font(ota_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(ota_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(ota_label, LV_ALIGN_BOTTOM_MID, 0, -180);

  // --- 4. INFO OVERLAY ---
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

  info_device_name_label = lv_label_create(info_screen);
  lv_label_set_text(info_device_name_label, "Reader: Unknown");
  lv_obj_set_style_text_font(info_device_name_label, &lv_font_montserrat_24, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(info_device_labels[i], &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(info_device_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_label_set_long_mode(info_device_labels[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info_device_labels[i], LV_PCT(100));
    lv_obj_set_style_pad_bottom(info_device_labels[i], 15, LV_PART_MAIN);
  }

  lv_obj_t *info_footer = lv_label_create(info_screen);
  lv_label_set_text(info_footer, "Press and hold button for\n5 seconds to restart");
  lv_obj_set_style_text_font(info_footer, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(info_footer, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
  lv_label_set_long_mode(info_footer, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(info_footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(info_footer, LV_PCT(100));

  // --- 5. CLOCK & EQUIPMENT SCREENSAVER OVERLAY ---
  screensaver_screen = lv_obj_create(main_screen);
  lv_obj_set_size(screensaver_screen, 480, 480);
  lv_obj_center(screensaver_screen);
  lv_obj_set_style_bg_color(screensaver_screen, lv_color_hex(0x121212), LV_PART_MAIN);
  lv_obj_set_style_border_width(screensaver_screen, 0, LV_PART_MAIN);
  lv_obj_remove_flag(screensaver_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(screensaver_screen, LV_OBJ_FLAG_HIDDEN);

  // Shiftable container inside screensaver to prevent pixel burn-in over long idle
  ss_content_container = lv_obj_create(screensaver_screen);
  lv_obj_set_size(ss_content_container, 460, 460);
  lv_obj_center(ss_content_container);
  lv_obj_set_style_bg_opa(ss_content_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ss_content_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ss_content_container, 10, LV_PART_MAIN);
  lv_obj_set_layout(ss_content_container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ss_content_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ss_content_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // 5a. Call to Action Header
  ss_cta_label = lv_label_create(ss_content_container);
  lv_label_set_text(ss_cta_label, "Tap ID at top to activate");
  lv_obj_set_style_text_font(ss_cta_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_cta_label, lv_color_hex(0x00FF00), LV_PART_MAIN);
  lv_obj_set_style_text_align(ss_cta_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  // 5b. Clock Box (Time + Date)
  lv_obj_t *clock_box = lv_obj_create(ss_content_container);
  lv_obj_set_width(clock_box, LV_PCT(100));
  lv_obj_set_height(clock_box, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(clock_box, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(clock_box, 0, LV_PART_MAIN);
  lv_obj_set_layout(clock_box, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(clock_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(clock_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  ss_time_label = lv_label_create(clock_box);
  lv_label_set_text(ss_time_label, "--:--");
  lv_obj_set_style_text_font(ss_time_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_time_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  ss_date_label = lv_label_create(clock_box);
  lv_label_set_text(ss_date_label, "------------------");
  lv_obj_set_style_text_font(ss_date_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_date_label, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_set_style_pad_top(ss_date_label, 5, LV_PART_MAIN);

  // 5c. Equipment Card Section
  lv_obj_t *equip_card = lv_obj_create(ss_content_container);
  lv_obj_set_width(equip_card, LV_PCT(100));
  lv_obj_set_height(equip_card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(equip_card, lv_color_hex(0x222222), LV_PART_MAIN);
  lv_obj_set_style_border_color(equip_card, lv_color_hex(0x333333), LV_PART_MAIN);
  lv_obj_set_style_border_width(equip_card, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(equip_card, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(equip_card, 15, LV_PART_MAIN);
  lv_obj_set_layout(equip_card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(equip_card, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *equip_header = lv_label_create(equip_card);
  lv_label_set_text(equip_header, "AVAILABLE EQUIPMENT");
  lv_obj_set_style_text_font(equip_header, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(equip_header, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(equip_header, 8, LV_PART_MAIN);

  ss_equip_label = lv_label_create(equip_card);
  lv_label_set_text(ss_equip_label, "No equipment available");
  lv_obj_set_style_text_font(ss_equip_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(ss_equip_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_long_mode(ss_equip_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ss_equip_label, LV_PCT(100));
}

// ==========================================
// 4. DATA PROCESSING & UTILS
// ==========================================
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
      lv_obj_add_flag(channel_hint_labels[i], LV_OBJ_FLAG_HIDDEN);  // Hidden by default

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
        // Warning threshold: time left is 10% or less of total duration
        bool is_expiring_soon = (groups[i].duration > 0) && (groups[i].time_left > 0) && (groups[i].time_left * 10 <= groups[i].duration);

        if (is_expiring_soon) {
          text_color = 0xFFA500;  // Orange warning color
          bg_color = 0x33271A;    // Dark orange tint
          lv_label_set_text(channel_state_labels[i], "EXPIRING SOON");
          lv_obj_remove_flag(channel_hint_labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
          text_color = 0x00FF00;  // Normal active green
          bg_color = 0x1A331A;    // Dark green tint
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

// Pixel-jitter timer callback to prevent screen burn-in on idle clock
void screensaver_burnin_cb(lv_timer_t *timer) {
  if (current_mode != MODE_SCREENSAVER) return;

  static int shift_step = 0;
  shift_step = (shift_step + 1) % 4;

  // Subtle 8px offset shifts
  int x_off = (shift_step == 1) ? 8 : (shift_step == 3) ? -8
                                                        : 0;
  int y_off = (shift_step == 2) ? 8 : (shift_step == 0) ? -8
                                                        : 0;

  lv_obj_align(ss_content_container, LV_ALIGN_CENTER, x_off, y_off);
}

void process_incoming_json(String jsonString) {
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, jsonString);
  if (error) return;

  // --- 1. Update Global Configs & Time ---
  if (doc.containsKey("url")) {
    global_url = doc["url"].as<String>();
  }

  // --- 2. Update Screensaver Clock & Date from Epoch ---
  if (doc.containsKey("time")) {
    time_t raw_time = doc["time"].as<time_t>();

    // Handle millisecond vs second timestamps
    if (raw_time > 1000000000000L) {
      raw_time /= 1000;
    }

    struct tm *timeinfo = localtime(&raw_time);

    char time_buf[16];
    char date_buf[32];

    strftime(time_buf, sizeof(time_buf), "%I:%M %p", timeinfo);   // e.g., "02:45 PM"
    strftime(date_buf, sizeof(date_buf), "%A, %b %d", timeinfo);  // e.g., "Saturday, Aug 01"

    // Clean leading zero from hour if present (e.g. "02:45 PM" -> "2:45 PM")
    if (time_buf[0] == '0') {
      memmove(time_buf, time_buf + 1, strlen(time_buf));
    }

    lv_label_set_text(ss_time_label, time_buf);
    lv_label_set_text(ss_date_label, date_buf);
  }

  // --- 3. Handle OTA Mode ---
  if (doc.containsKey("coreOta") || doc.containsKey("ota")) {
    set_display_mode(MODE_OTA);
    int progress = doc.containsKey("coreOta") ? doc["coreOta"].as<int>() : doc["ota"].as<int>();

    lv_bar_set_value(ota_bar, progress, LV_ANIM_ON);
    lv_label_set_text_fmt(ota_label, "%d%%", progress);
    return;
  }

  // --- 4. Handle Info Mode (Button Press) ---
  static unsigned long button_release_time = 0;
  bool button_pressed = doc.containsKey("button") ? doc["button"].as<bool>() : false;

  if (button_pressed) {
    button_release_time = millis();
    set_display_mode(MODE_INFO);

    String acs_name = doc.containsKey("deviceName") ? doc["deviceName"].as<String>() : "Unknown";
    lv_label_set_text_fmt(info_device_name_label, "Reader: %s", acs_name.c_str());

    int num_channels = doc["channels"].as<int>();
    if (num_channels <= 0 || num_channels > MAX_CHANNELS) num_channels = MAX_CHANNELS;

    for (int i = 0; i < MAX_CHANNELS; i++) {
      if (i < num_channels) {
        lv_obj_remove_flag(info_device_labels[i], LV_OBJ_FLAG_HIDDEN);
        String dev_name = doc["deviceNames"][i].as<String>();
        long hobbs = doc.containsKey("hobbsSeconds") ? doc["hobbsSeconds"][i].as<long>() : 0;
        char hobbs_str[32];
        format_hobbs_time(hobbs, hobbs_str);

        // Formats as equipment name with run time on a new line underneath
        lv_label_set_text_fmt(info_device_labels[i], "%s\nRun time: %s", dev_name.c_str(), hobbs_str);
      } else {
        lv_obj_add_flag(info_device_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
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

  // --- 6. Equipment List Consolidation for Screensaver ---
  // (Only includes equipment that is NOT "LOCKED_OUT")
  int num_channels = doc["channels"].as<int>();
  if (num_channels <= 0 || num_channels > MAX_CHANNELS) num_channels = MAX_CHANNELS;

  String equip_list_str = "";
  int available_count = 0;

  for (int i = 0; i < num_channels; i++) {
    String state = (doc.containsKey("state") && doc["state"][i].is<String>()) ? doc["state"][i].as<String>() : "";

    // Skip equipment that is currently locked out
    if (state == "LOCKED_OUT") continue;

    if (doc.containsKey("deviceNames") && doc["deviceNames"][i].is<String>()) {
      if (available_count > 0) equip_list_str += "\n";
      equip_list_str += "• " + doc["deviceNames"][i].as<String>();
      available_count++;
    }
  }

  if (available_count > 0) {
    lv_label_set_text(ss_equip_label, equip_list_str.c_str());
  } else {
    lv_label_set_text(ss_equip_label, "No equipment available");
  }

  // --- 7. Determine Screen Mode ---
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
  } else {
    set_display_mode(MODE_MAIN);
  }

  // Process channel cards for background/main mode
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

  // Pixel-jitter timer for screen burn-in protection (triggers every 60 seconds)
  lv_timer_create(screensaver_burnin_cb, 60000, NULL);
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