#include <pebble.h>

// ---------------------------------------------------------------------------
// Types & constants
// ---------------------------------------------------------------------------

typedef enum {
  WorkoutTreadmill,
  WorkoutRun,
  WorkoutWalk,
  WorkoutBike,
  WorkoutCount,
} WorkoutType;

typedef enum {
  StateRunning,
  StatePaused,
  StateEnded,
} WorkoutState;

// Commands sent to the phone (must match src/pkjs/index.js)
enum { CMD_STOP = 0, CMD_START = 1, CMD_PAUSE = 2, CMD_RESUME = 3 };

#define PERSIST_STEPS_PER_MILE 1
#define DEFAULT_STEPS_PER_MILE 1800
#define MIN_STEPS_PER_MILE 500
#define MAX_STEPS_PER_MILE 4000
#define METERS_PER_MILE_X10 16093  // meters * 10

static const char *const s_type_names[WorkoutCount] = {"Treadmill", "Run", "Walk", "Bike"};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static Window *s_menu_window;
static MenuLayer *s_menu_layer;

static Window *s_workout_window;
static Layer *s_workout_layer;
static AppTimer *s_tick_timer;

static WorkoutType s_type;
static WorkoutState s_state;
static int s_steps_per_mile = DEFAULT_STEPS_PER_MILE;

static int s_elapsed_s;        // seconds spent in StateRunning
static int s_steps;            // steps taken while running
static int s_steps_base;       // health step count when current segment began
static int32_t s_gps_dm;       // GPS distance, decimeters (from phone)
static bool s_gps_fix;
static bool s_phone_ok;        // have we ever heard from the phone this workout

static GColor s_bg, s_fg, s_text;
#define PERSIST_COLORS 2

static void prv_load_colors(void) {
  s_bg = GColorBlack;
  s_text = GColorWhite;
  s_fg = GColorGreen;
  uint8_t c[3];
  if (persist_read_data(PERSIST_COLORS, c, sizeof(c)) == (int)sizeof(c)) {
    s_bg.argb = c[0];
    s_text.argb = c[1];
    s_fg.argb = c[2];
  }
#if defined(PBL_BW)
  // Black & white screens can't show the colored accent; keep selections legible.
  s_bg = GColorBlack;
  s_text = GColorWhite;
  s_fg = GColorWhite;
#endif
}

static void prv_save_colors(void) {
  uint8_t c[3] = {s_bg.argb, s_text.argb, s_fg.argb};
  persist_write_data(PERSIST_COLORS, c, sizeof(c));
}

static void prv_apply_colors(void);

static bool prv_uses_gps(void) { return s_type != WorkoutTreadmill; }

// ---------------------------------------------------------------------------
// Health / steps
// ---------------------------------------------------------------------------

static int prv_health_steps(void) {
#if defined(PBL_HEALTH)
  return (int)health_service_sum_today(HealthMetricStepCount);
#else
  return 0;
#endif
}

// Steps taken in the current running segment.
static int prv_segment_steps(void) {
  int delta = prv_health_steps() - s_steps_base;
  return delta > 0 ? delta : 0;  // guards against midnight rollover
}

// ---------------------------------------------------------------------------
// Phone messaging
// ---------------------------------------------------------------------------

static int s_pending_cmd = -1;
static AppTimer *s_retry_timer;

static void prv_send_cmd(int cmd);

static void prv_retry_cb(void *ctx) {
  s_retry_timer = NULL;
  if (s_pending_cmd >= 0) {
    prv_send_cmd(s_pending_cmd);
  }
}

static void prv_send_cmd(int cmd) {
  s_pending_cmd = cmd;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_int32(iter, MESSAGE_KEY_CMD, cmd);
    if (app_message_outbox_send() == APP_MSG_OK) {
      return;
    }
  }
  if (!s_retry_timer) {
    s_retry_timer = app_timer_register(1000, prv_retry_cb, NULL);
  }
}

static void prv_outbox_sent(DictionaryIterator *iter, void *ctx) {
  s_pending_cmd = -1;
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  if (s_pending_cmd >= 0 && !s_retry_timer) {
    s_retry_timer = app_timer_register(1500, prv_retry_cb, NULL);
  }
}

static void prv_inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *dist = dict_find(iter, MESSAGE_KEY_DIST);
  Tuple *gps = dict_find(iter, MESSAGE_KEY_GPS);
  s_phone_ok = true;
  if (dist && s_state != StateEnded) {
    s_gps_dm = dist->value->int32;
  }
  if (gps) {
    s_gps_fix = gps->value->int32 != 0;
  }

  Tuple *spm = dict_find(iter, MESSAGE_KEY_STEPS_PER_MILE);
  if (spm) {
    int v = spm->value->int32;
    if (v >= MIN_STEPS_PER_MILE && v <= MAX_STEPS_PER_MILE) {
      s_steps_per_mile = v;
      persist_write_int(PERSIST_STEPS_PER_MILE, v);
    }
  }
  Tuple *cbg = dict_find(iter, MESSAGE_KEY_COLOR_BG);
  Tuple *ctx_ = dict_find(iter, MESSAGE_KEY_COLOR_TEXT);
  Tuple *cfg = dict_find(iter, MESSAGE_KEY_COLOR_FG);
  if (cbg || ctx_ || cfg) {
    if (cbg) s_bg = GColorFromHEX(cbg->value->int32);
    if (ctx_) s_text = GColorFromHEX(ctx_->value->int32);
    if (cfg) s_fg = GColorFromHEX(cfg->value->int32);
    prv_save_colors();
    prv_apply_colors();
  }
  if (s_menu_layer) {
    layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
  }
  if (s_workout_layer) {
    layer_mark_dirty(s_workout_layer);
  }
}

// ---------------------------------------------------------------------------
// Workout logic
// ---------------------------------------------------------------------------

static void prv_tick(void *ctx);

static void prv_schedule_tick(void) {
  s_tick_timer = app_timer_register(1000, prv_tick, NULL);
}

static void prv_tick(void *ctx) {
  s_tick_timer = NULL;
  if (s_state == StateRunning) {
    s_elapsed_s++;
    if (s_workout_layer) {
      layer_mark_dirty(s_workout_layer);
    }
  }
  if (s_state != StateEnded) {
    prv_schedule_tick();
  }
}

static int prv_total_steps(void) {
  return s_steps + (s_state == StateRunning ? prv_segment_steps() : 0);
}

// Distance in hundredths of a mile.
static int prv_distance_centimiles(void) {
  if (prv_uses_gps()) {
    return (int)((int64_t)s_gps_dm * 100 / METERS_PER_MILE_X10);
  }
  return (int)((int64_t)prv_total_steps() * 100 / s_steps_per_mile);
}

static void prv_pause(void) {
  s_steps += prv_segment_steps();
  s_state = StatePaused;
  if (prv_uses_gps()) prv_send_cmd(CMD_PAUSE);
  vibes_short_pulse();
}

static void prv_resume(void) {
  s_steps_base = prv_health_steps();
  s_state = StateRunning;
  if (prv_uses_gps()) prv_send_cmd(CMD_RESUME);
  vibes_short_pulse();
}

static void prv_end(void) {
  if (s_state == StateRunning) {
    s_steps += prv_segment_steps();
  }
  s_state = StateEnded;
  if (prv_uses_gps()) prv_send_cmd(CMD_STOP);
  vibes_double_pulse();
}

static void prv_start_workout(WorkoutType type) {
  s_type = type;
  s_state = StateRunning;
  s_elapsed_s = 0;
  s_steps = 0;
  s_steps_base = prv_health_steps();
  s_gps_dm = 0;
  s_gps_fix = false;
  s_phone_ok = false;
}

// ---------------------------------------------------------------------------
// Workout window
// ---------------------------------------------------------------------------

static void prv_draw_value(GContext *ctx, const char *text, const char *unit, GFont font,
                           GFont unit_font, int center_x, int y, int h, GColor color,
                           int unit_w) {
  graphics_context_set_text_color(ctx, color);
  GSize sz = graphics_text_layout_get_content_size(text, font, GRect(0, 0, 400, h),
                                                   GTextOverflowModeFill, GTextAlignmentLeft);
  int total = sz.w + (unit ? unit_w : 0);
  int x = center_x - total / 2;
  graphics_draw_text(ctx, text, font, GRect(x, y, sz.w + 4, h), GTextOverflowModeFill,
                     GTextAlignmentLeft, NULL);
  if (unit) {
    graphics_draw_text(ctx, unit, unit_font, GRect(x + sz.w + 3, y + h - 24, unit_w + 8, 20),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
}

// Big "12.34" style distance: the 60pt numbers font has no '.', so draw the point ourselves.
static void prv_draw_big_distance(GContext *ctx, int cm, GFont font, GFont unit_font, int cx, int y,
                                  int h, GColor color) {
  char ip[8], fp[4];
  snprintf(ip, sizeof(ip), "%d", cm / 100);
  snprintf(fp, sizeof(fp), "%02d", cm % 100);
  GRect big = GRect(0, 0, 400, h);
  int iw = graphics_text_layout_get_content_size(ip, font, big, GTextOverflowModeFill, GTextAlignmentLeft).w;
  int fw = graphics_text_layout_get_content_size(fp, font, big, GTextOverflowModeFill, GTextAlignmentLeft).w;
  const int dot = 12, gap = 4, unit_w = 24;
  int x = cx - (iw + dot + fw + unit_w) / 2;
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, ip, font, GRect(x, y, iw + 4, h), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, GRect(x + iw + gap, y + h * 3 / 4 - 6, dot - gap + 1, dot - gap + 1), 0, GCornerNone);
  graphics_draw_text(ctx, fp, font, GRect(x + iw + dot, y, fw + 4, h), GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, "mi", unit_font, GRect(x + iw + dot + fw + 2, y + h - 30, unit_w + 8, 20),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);
}

static void prv_pill(GContext *ctx, GRect r, const char *text, GColor fill, GColor fg) {
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, r, 8, GCornersAll);
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(r.origin.x, r.origin.y - 1, r.size.w, r.size.h), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
}

static void prv_workout_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const int w = b.size.w, h = b.size.h;
  const bool tall = h >= 200;
  #if defined(PBL_PLATFORM_GABBRO) || defined(PBL_PLATFORM_EMERY)
  bool huge = h >= 220;  // use the 60pt numbers
#else
  bool huge = false;
#endif

  graphics_context_set_fill_color(ctx, s_bg);
  graphics_fill_rect(ctx, b, 0, GCornersAll);

#if defined(PBL_PLATFORM_GABBRO) || defined(PBL_PLATFORM_EMERY)
  GFont huge_font = fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM);
#else
  GFont huge_font = NULL;  // unused: `huge` is false on these screens
#endif
  GFont big = fonts_get_system_font(tall ? FONT_KEY_LECO_42_NUMBERS : FONT_KEY_LECO_32_BOLD_NUMBERS);
  GFont mid = fonts_get_system_font(tall ? FONT_KEY_LECO_38_BOLD_NUMBERS : FONT_KEY_LECO_28_LIGHT_NUMBERS);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  const bool paused = s_state == StatePaused;
  const bool ended = s_state == StateEnded;
  // Narrow screens (emery) have no room for the pause buttons beside 60pt digits.
  if (paused && w < 240) huge = false;
  GColor accent = paused ? GColorOrange : s_fg;
  GColor main_c = s_text;
  // While paused, make room for the button pills on the right.
  int cx = paused ? (w - 52) / 2 + 4 : w / 2;

  // Status header
  char head[32];
  if (ended) {
    snprintf(head, sizeof(head), "%s done", s_type_names[s_type]);
  } else if (paused) {
    snprintf(head, sizeof(head), "PAUSED");
  } else if (prv_uses_gps() && !s_gps_fix) {
    snprintf(head, sizeof(head), "%s - GPS...", s_type_names[s_type]);
  } else {
    snprintf(head, sizeof(head), "%s", s_type_names[s_type]);
  }
  graphics_context_set_text_color(ctx, accent);
  graphics_draw_text(ctx, head, small, GRect(cx - 70, h * 5 / 100, 140, 24), GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);

  // Distance
  int cm = prv_distance_centimiles();
  int row_h = huge ? 62 : tall ? 52 : 40;
  int y_dist = huge ? h * 10 / 100 : h * 18 / 100;
  int y_time = huge ? h * 36 / 100 : h * 42 / 100;
  int y_steps = huge ? h * 63 / 100 : h * 67 / 100;
  if (huge) {
    prv_draw_big_distance(ctx, cm, huge_font, small, cx, y_dist, row_h, main_c);
  } else {
    char dist[16];
    snprintf(dist, sizeof(dist), "%d.%02d", cm / 100, cm % 100);
    prv_draw_value(ctx, dist, "mi", big, small, cx, y_dist, row_h, main_c, 22);
  }

  // Time (drops to the smaller font once it needs H:MM:SS)
  int t = s_elapsed_s;
  char time[16];
  if (t >= 3600) {
    snprintf(time, sizeof(time), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
  } else {
    snprintf(time, sizeof(time), "%d:%02d", t / 60, t % 60);
  }
  if (huge && t < 3600) {
    prv_draw_value(ctx, time, NULL, huge_font, small, cx, y_time, row_h, main_c, 0);
  } else {
    prv_draw_value(ctx, time, NULL, big, small, huge ? cx : cx, huge ? y_time + 6 : y_time,
                   tall ? 52 : 40, main_c, 0);
  }

  // Steps
  char steps[16];
  snprintf(steps, sizeof(steps), "%d", prv_total_steps());
  prv_draw_value(ctx, steps, "steps", huge ? big : mid, small, cx, y_steps,
                 tall ? (huge ? 52 : 46) : 36, main_c, 42);

  // Button hints
  if (paused) {
    prv_pill(ctx, GRect(w - 74, h * 22 / 100, 52, 24), "END", GColorRed, GColorWhite);
    prv_pill(ctx, GRect(w - 66, h / 2 - 12, 52, 24), "GO", s_fg, s_bg);
  } else if (ended) {
    graphics_context_set_text_color(ctx, s_text);
    graphics_draw_text(ctx, "Select to close", small, GRect(0, h * 88 / 100, w, 24),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

static void prv_workout_select(ClickRecognizerRef r, void *ctx) {
  if (s_state == StateEnded) {
    window_stack_pop(true);
    return;
  }
  if (s_state == StateRunning) {
    prv_pause();
  } else {
    prv_resume();
  }
  layer_mark_dirty(s_workout_layer);
}

static void prv_workout_up(ClickRecognizerRef r, void *ctx) {
  if (s_state == StatePaused) {
    prv_end();
    layer_mark_dirty(s_workout_layer);
  }
}

static void prv_workout_back(ClickRecognizerRef r, void *ctx) {
  // Back must not accidentally quit a workout. Only leaves when finished.
  if (s_state == StateEnded) {
    window_stack_pop(true);
  } else if (s_state == StateRunning) {
    prv_pause();
    layer_mark_dirty(s_workout_layer);
  }
}

static void prv_workout_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_workout_select);
  window_single_click_subscribe(BUTTON_ID_UP, prv_workout_up);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_workout_back);
}

static void prv_workout_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_workout_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_workout_layer, prv_workout_update);
  layer_add_child(root, s_workout_layer);
}

static void prv_workout_unload(Window *window) {
  layer_destroy(s_workout_layer);
  s_workout_layer = NULL;
  if (s_tick_timer) {
    app_timer_cancel(s_tick_timer);
    s_tick_timer = NULL;
  }
  if (s_state != StateEnded && prv_uses_gps()) {
    prv_send_cmd(CMD_STOP);
  }
  light_enable(false);
}

static void prv_workout_appear(Window *window) {
  light_enable_interaction();
}

static void prv_push_workout(WorkoutType type) {
  prv_start_workout(type);
  if (prv_uses_gps()) {
    prv_send_cmd(CMD_START);
  }
  if (!s_workout_window) {
    s_workout_window = window_create();
    window_set_background_color(s_workout_window, s_bg);
    window_set_click_config_provider(s_workout_window, prv_workout_click_config);
    window_set_window_handlers(s_workout_window, (WindowHandlers){
      .load = prv_workout_load,
      .unload = prv_workout_unload,
      .appear = prv_workout_appear,
    });
  }
  window_stack_push(s_workout_window, true);
  prv_schedule_tick();
}

// ---------------------------------------------------------------------------
// Workout type menu
// ---------------------------------------------------------------------------

static uint16_t prv_menu_rows(MenuLayer *m, uint16_t section, void *ctx) { return WorkoutCount; }

static int16_t prv_menu_cell_height(MenuLayer *m, MenuIndex *idx, void *ctx) {
  return PBL_IF_ROUND_ELSE(menu_layer_is_index_selected(m, idx) ? 56 : 40, 44);
}

static void prv_menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  char sub[24];
  const char *subtitle = NULL;
  switch (idx->row) {
    case WorkoutRun:
    case WorkoutWalk:
    case WorkoutBike:
      subtitle = "GPS";
      break;
    default:
      snprintf(sub, sizeof(sub), "%d steps/mi", s_steps_per_mile);
      subtitle = sub;
  }
  menu_cell_basic_draw(ctx, cell, s_type_names[idx->row], subtitle, NULL);
}

static void prv_menu_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  prv_push_workout((WorkoutType)idx->row);
}

static void prv_menu_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_menu_layer = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = prv_menu_rows,
    .get_cell_height = prv_menu_cell_height,
    .draw_row = prv_menu_draw_row,
    .select_click = prv_menu_select,
  });
  menu_layer_set_highlight_colors(s_menu_layer, s_fg, s_bg);
  menu_layer_set_normal_colors(s_menu_layer, s_bg, s_text);
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
#if defined(PBL_ROUND)
  menu_layer_set_center_focused(s_menu_layer, true);
#endif
  layer_add_child(root, menu_layer_get_layer(s_menu_layer));
}

static void prv_menu_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
}

static void prv_apply_colors(void) {
  if (s_menu_window) window_set_background_color(s_menu_window, s_bg);
  if (s_workout_window) window_set_background_color(s_workout_window, s_bg);
  if (s_menu_layer) {
    menu_layer_set_highlight_colors(s_menu_layer, s_fg, s_bg);
    menu_layer_set_normal_colors(s_menu_layer, s_bg, s_text);
  }
  if (s_workout_layer) layer_mark_dirty(s_workout_layer);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void prv_init(void) {
  prv_load_colors();
  if (persist_exists(PERSIST_STEPS_PER_MILE)) {
    s_steps_per_mile = persist_read_int(PERSIST_STEPS_PER_MILE);
    if (s_steps_per_mile < MIN_STEPS_PER_MILE || s_steps_per_mile > MAX_STEPS_PER_MILE) {
      s_steps_per_mile = DEFAULT_STEPS_PER_MILE;
    }
  }

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(256, 64);

  s_menu_window = window_create();
  window_set_background_color(s_menu_window, s_bg);
  window_set_window_handlers(s_menu_window, (WindowHandlers){
    .load = prv_menu_load,
    .unload = prv_menu_unload,
  });
  window_stack_push(s_menu_window, true);
}

static void prv_deinit(void) {
  if (s_retry_timer) app_timer_cancel(s_retry_timer);
  if (s_workout_window) window_destroy(s_workout_window);
  window_destroy(s_menu_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
