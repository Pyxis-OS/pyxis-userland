#include "../common/bootstrap_environment.h"
#include <display.h>
#include <font.h>
#include <gamepad.h>
#include <keyboard.h>
#include <startup.h>
#include <wait.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CANVAS_WIDTH 960
#define CANVAS_HEIGHT 640
#define BACKGROUND 0x111923
#define PANEL 0x1b2938
#define TEXT 0xe9eff6
#define MUTED 0x92a5b9
#define BODY 0xcbd5df
#define BODY_EDGE 0x76889c
#define INSET 0x263648
#define IDLE 0x3c5065
#define ACTIVE 0x51e0bd
#define WARNING 0xf5c477
#define FONT_PATH "pkg://fonts/ibm-plex-sans/IBMPlexSans-Regular.ttf"

struct app {
  handle_t display, keyboard, gamepad;
  struct display_buffer buffer;
  uint64_t slot;
  struct font_face *face;
  struct font_size *small, *normal, *title;
  struct gamepad_catalog catalog;
  struct gamepad_info info;
  struct gamepad_event accepted;
  uint32_t selected;
  bool focused, valid, reset, dirty, refresh, quit;
  int scale, left, top;
  enum font_result draw_result;
};

static uint32_t packed(struct app *app, uint32_t color)
{
  return ((color >> 16) & 255) << app->buffer.red_shift |
      ((color >> 8) & 255) << app->buffer.green_shift |
      (color & 255) << app->buffer.blue_shift;
}

/* Drawing coordinates use a 960x640 canvas, fitted without changing its aspect. */
static int position(struct app *app, int value)
{
  return value * app->scale / 1000;
}

static void rectangle(struct app *app, int x, int y, int w, int h, uint32_t color)
{
  int x0 = app->left + position(app, x);
  int y0 = app->top + position(app, y);
  int x1 = app->left + position(app, x + w);
  int y1 = app->top + position(app, y + h);
  uint8_t *pixels = (uint8_t *)display_slot_address(&app->buffer, app->slot);
  for (int row = y0; row < y1; ++row) {
    if (row < 0 || row >= (int)app->buffer.height) {
      continue;
    }
    uint32_t *line = (uint32_t *)(pixels + (uint64_t)row * app->buffer.pitch);
    for (int column = x0; column < x1; ++column) {
      if (column >= 0 && column < (int)app->buffer.width) {
        line[column] = packed(app, color);
      }
    }
  }
}

static void ellipse(struct app *app, int cx, int cy, int rx, int ry, uint32_t color)
{
  for (int y = -ry; y <= ry; ++y) {
    int extent = 0;
    while (extent < rx && (int64_t)(extent + 1) * (extent + 1) * ry * ry +
        (int64_t)y * y * rx * rx <= (int64_t)rx * rx * ry * ry) {
      ++extent;
    }
    rectangle(app, cx - extent, cy + y, 2 * extent + 1, 1, color);
  }
}

static void rounded(struct app *app, int x, int y, int w, int h, int r,
    uint32_t color)
{
  rectangle(app, x + r, y, w - 2 * r, h, color);
  rectangle(app, x, y + r, w, h - 2 * r, color);
  ellipse(app, x + r, y + r, r, r, color);
  ellipse(app, x + w - r - 1, y + r, r, r, color);
  ellipse(app, x + r, y + h - r - 1, r, r, color);
  ellipse(app, x + w - r - 1, y + h - r - 1, r, r, color);
}

static enum font_result text(struct app *app, struct font_size *font,
    int x, int baseline, const char *value, uint32_t color)
{
  struct font_xrgb8888 surface = {
    .pixels = (uint32_t *)display_slot_address(&app->buffer, app->slot),
    .capacity_bytes = (size_t)(app->buffer.pitch * app->buffer.height),
    .stride_bytes = (size_t)app->buffer.pitch,
    .width = (uint32_t)app->buffer.width, .height = (uint32_t)app->buffer.height,
    .red_shift = (uint8_t)app->buffer.red_shift,
    .green_shift = (uint8_t)app->buffer.green_shift,
    .blue_shift = (uint8_t)app->buffer.blue_shift,
  };
  if (app->draw_result != FONT_OK) {
    return app->draw_result;
  }
  app->draw_result = font_draw_xrgb8888(font, &surface,
      (app->left + position(app, x)) * 64,
      (app->top + position(app, baseline)) * 64, value, strlen(value), color);
  return app->draw_result;
}

static bool pressed(struct app *app, uint32_t mask)
{
  return app->valid && (app->accepted.state.buttons & mask);
}

static void button(struct app *app, int x, int y, int w, int h,
    const char *label, uint32_t mask)
{
  bool down = pressed(app, mask);
  rounded(app, x, y, w, h, h / 3, down ? ACTIVE : IDLE);
  /* These labels are short; center their approximate Plex Sans advance. */
  int width = (int)strlen(label) * 8;
  text(app, app->small, x + (w - width) / 2, y + h / 2 + 5,
      label, down ? BACKGROUND : TEXT);
}

static void line(struct app *app, int x0, int y0, int x1, int y1, uint32_t color)
{
  int dx = x1 - x0, dy = y1 - y0;
  int steps = dx < 0 ? -dx : dx;
  int height = dy < 0 ? -dy : dy;
  if (height > steps) {
    steps = height;
  }
  if (!steps) {
    return;
  }
  for (int step = 0; step <= steps; ++step) {
    rectangle(app, x0 + dx * step / steps - 1, y0 + dy * step / steps - 1, 3, 3, color);
  }
}

static void face_button(struct app *app, int x, int y, uint32_t mask, uint32_t color)
{
  bool down = pressed(app, mask);
  uint32_t background = down ? ACTIVE : INSET;
  color = down ? BACKGROUND : color;
  ellipse(app, x, y, 22, 22, background);
  if (mask == GAMEPAD_BUTTON_CIRCLE) {
    ellipse(app, x, y, 10, 10, color);
    ellipse(app, x, y, 7, 7, background);
  } else if (mask == GAMEPAD_BUTTON_SQUARE) {
    rectangle(app, x - 10, y - 10, 21, 21, color);
    rectangle(app, x - 7, y - 7, 15, 15, background);
  } else if (mask == GAMEPAD_BUTTON_CROSS) {
    line(app, x - 8, y - 8, x + 8, y + 8, color);
    line(app, x + 8, y - 8, x - 8, y + 8, color);
  } else {
    line(app, x, y - 11, x - 11, y + 8, color);
    line(app, x, y - 11, x + 11, y + 8, color);
    line(app, x - 11, y + 8, x + 11, y + 8, color);
  }
}

static void stick(struct app *app, int x, int y, unsigned axis, uint32_t mask,
    const char *name)
{
  ellipse(app, x, y, 55, 55, BODY_EDGE);
  ellipse(app, x, y, 51, 51, INSET);
  rectangle(app, x - 40, y, 81, 1, IDLE);
  rectangle(app, x, y - 40, 1, 81, IDLE);
  if (app->valid) {
    int dx = ((int)app->accepted.state.axes[axis] * 60 / 255) - 30;
    int dy = ((int)app->accepted.state.axes[axis + 1] * 60 / 255) - 30;
    ellipse(app, x + dx, y + dy, 21, 21, pressed(app, mask) ? ACTIVE : BODY);
    ellipse(app, x + dx, y + dy, 15, 15, pressed(app, mask) ? ACTIVE : BODY_EDGE);
  }
  text(app, app->small, x - 8, y + 6, name, app->valid ? INSET : MUTED);
}

static void trigger(struct app *app, int x, unsigned axis,
    uint32_t mask, const char *name)
{
  button(app, x, 145, 130, 32, name, mask);
  rounded(app, x, 116, 130, 18, 7, INSET);
  if (app->valid) {
    int width = app->accepted.state.axes[axis] * 124 / 255;
    if (width) {
      rectangle(app, x + 3, 119, width, 12, ACTIVE);
    }
  }
  char value[8];
  if (app->valid) {
    snprintf(value, sizeof(value), "%u", app->accepted.state.axes[axis]);
  } else {
    strcpy(value, "--");
  }
  text(app, app->small, x + 140, 131, value, MUTED);
}

static enum call_status render(struct app *app)
{
  app->draw_result = FONT_OK;
  uint8_t *pixels = (uint8_t *)display_slot_address(&app->buffer, app->slot);
  for (uint64_t y = 0; y < app->buffer.height; ++y) {
    uint32_t *row = (uint32_t *)(pixels + y * app->buffer.pitch);
    for (uint64_t x = 0; x < app->buffer.width; ++x) {
      row[x] = packed(app, BACKGROUND);
    }
  }
  text(app, app->title, 40, 49, "Controller", TEXT);
  text(app, app->small, 40, 76, "DUALSENSE EDGE  /  USB INPUT", MUTED);
  const char *status = !app->catalog.count ? "NO CONTROLLER CONNECTED" :
      !app->focused ? "UNFOCUSED - INPUT CLEARED" :
      !app->valid ? "WAITING FOR FRESH INPUT" : "CONNECTED / LIVE";
  uint32_t status_color = app->valid ? ACTIVE : WARNING;
  rounded(app, 580, 27, 340, 37, 10, PANEL);
  ellipse(app, 598, 45, 4, 4, status_color);
  text(app, app->small, 612, 51, status, status_color);

  /* Rounded side grips and a broad shoulder bridge form the front silhouette. */
  ellipse(app, 252, 382, 101, 137, BODY_EDGE);
  ellipse(app, 708, 382, 101, 137, BODY_EDGE);
  rounded(app, 172, 182, 616, 236, 77, BODY_EDGE);
  ellipse(app, 252, 376, 97, 133, BODY);
  ellipse(app, 708, 376, 97, 133, BODY);
  rounded(app, 176, 186, 608, 226, 74, BODY);
  rounded(app, 323, 330, 314, 118, 54, INSET);
  trigger(app, 184, GAMEPAD_AXIS_LEFT_TRIGGER, GAMEPAD_BUTTON_L2, "L2");
  trigger(app, 646, GAMEPAD_AXIS_RIGHT_TRIGGER, GAMEPAD_BUTTON_R2, "R2");
  button(app, 203, 189, 110, 29, "L1", GAMEPAD_BUTTON_L1);
  button(app, 647, 189, 110, 29, "R1", GAMEPAD_BUTTON_R1);
  button(app, 356, 227, 248, 86, "TOUCHPAD", GAMEPAD_BUTTON_TOUCHPAD);
  button(app, 319, 251, 29, 38, "C", GAMEPAD_BUTTON_CREATE);
  button(app, 612, 251, 29, 38, GAMEPAD_BUTTON_OPTIONS);
  text(app, app->small, 300, 239, "Create", INSET);
  text(app, app->small, 612, 239, "Options", INSET);

  uint8_t hat = app->valid ? app->accepted.state.hat : GAMEPAD_HAT_NEUTRAL;
  bool up = hat == 0 || hat == 1 || hat == 7;
  bool right = hat >= 1 && hat <= 3;
  bool down = hat >= 3 && hat <= 5;
  bool left = hat >= 5 && hat <= 7;
  rounded(app, 242, 282, 36, 34, 8, up ? ACTIVE : INSET);
  rounded(app, 242, 352, 36, 34, 8, down ? ACTIVE : INSET);
  rounded(app, 207, 317, 34, 34, 8, left ? ACTIVE : INSET);
  rounded(app, 279, 317, 34, 34, 8, right ? ACTIVE : INSET);
  text(app, app->normal, 252, 307, "^", up ? BACKGROUND : MUTED);
  text(app, app->normal, 252, 375, "v", down ? BACKGROUND : MUTED);
  text(app, app->normal, 219, 341, "<", left ? BACKGROUND : MUTED);
  text(app, app->normal, 289, 341, ">", right ? BACKGROUND : MUTED);
  face_button(app, 700, 288, GAMEPAD_BUTTON_TRIANGLE, 0x62d9be);
  face_button(app, 746, 334, GAMEPAD_BUTTON_CIRCLE, 0xff93a3);
  face_button(app, 700, 380, GAMEPAD_BUTTON_CROSS, 0x8fb8ff);
  face_button(app, 654, 334, GAMEPAD_BUTTON_SQUARE, 0xe8a0e5);
  stick(app, 370, 392, GAMEPAD_AXIS_LEFT_X, GAMEPAD_BUTTON_L3, "L3");
  stick(app, 590, 392, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_BUTTON_R3, "R3");
  button(app, 454, 340, 52, 34, "PS", GAMEPAD_BUTTON_PS);
  button(app, 454, 391, 52, 26, "Mic", GAMEPAD_BUTTON_MIC);
  button(app, 332, 457, 76, 27, "Fn L", GAMEPAD_BUTTON_LEFT_FUNCTION);
  button(app, 552, 457, 76, 27, "Fn R", GAMEPAD_BUTTON_RIGHT_FUNCTION);
  button(app, 211, 498, 104, 28, "Paddle L", GAMEPAD_BUTTON_LEFT_PADDLE);
  button(app, 645, 498, 104, 28, "Paddle R", GAMEPAD_BUTTON_RIGHT_PADDLE);

  rounded(app, 40, 547, 880, 62, 12, PANEL);
  char line[160];
  if (app->catalog.count) {
    snprintf(line, sizeof(line), "%u / %u   %s   %04x:%04x   instance %llu",
        app->selected + 1, app->catalog.count, app->info.name,
        app->info.vendor, app->info.product, (unsigned long long)app->info.instance);
  } else {
    strcpy(line, "Connect a supported USB controller. This view updates on attachment.");
  }
  text(app, app->small, 57, 570, line, TEXT);
  if (app->valid) {
    const uint8_t *axes = app->accepted.state.axes;
    snprintf(line, sizeof(line), "Left %3u, %3u   Right %3u, %3u   Hat %u   Buttons %05x",
        axes[0], axes[1], axes[2], axes[3], app->accepted.state.hat,
        app->accepted.state.buttons);
  } else {
    snprintf(line, sizeof(line), "%s  Axes unavailable; no held input is displayed.",
        app->reset ? "Input reset." : "");
  }
  text(app, app->small, 57, 593, line, MUTED);
  text(app, app->small, 40, 633, "Esc  Exit     Tab / Left / Right  Select controller", MUTED);
  text(app, app->small, 639, 633, "Raw axes 0-255 / no deadzone", MUTED);
  if (app->draw_result != FONT_OK) {
    fprintf(stderr, "gamepadtest: drawing: %s\n", font_result_string(app->draw_result));
    return CALL_UNAVAILABLE;
  }
  struct display_submit_reply reply;
  enum call_status result = display_submit(app->display, app->slot, &reply);
  if (result == CALL_OK) {
    app->slot = reply.next;
    app->dirty = false;
  }
  return result;
}

static void invalidate(struct app *app)
{
  app->accepted = (struct gamepad_event){0};
  app->valid = false;
  app->dirty = true;
}

static void accept(struct app *app, const struct gamepad_event *event)
{
  app->accepted = *event;
  app->focused = (event->flags & GAMEPAD_EVENT_FOCUSED) != 0;
  app->valid = app->focused && (event->flags & GAMEPAD_EVENT_VALID);
  app->reset = !app->valid;
  app->dirty = true;
}

static enum call_status refresh_catalog(struct app *app)
{
  uint64_t previous = app->info.instance;
  struct gamepad_catalog catalog, confirmed;
  enum call_status result = gamepad_catalog(app->gamepad, &catalog);
  if (result != CALL_OK) {
    return result;
  }
  if (catalog.count > GAMEPAD_DEVICE_CAPACITY) {
    return CALL_BAD_REQUEST;
  }
  uint32_t selected = 0;
  for (uint32_t i = 0; i < catalog.count; ++i) {
    if (catalog.instances[i] == previous) {
      selected = i;
      break;
    }
  }
  struct gamepad_info info = {0};
  struct gamepad_event state = {0};
  if (catalog.count) {
    result = gamepad_info(app->gamepad, catalog.instances[selected], &info);
    if (result == CALL_OK) {
      result = gamepad_state(app->gamepad, info.instance, &state);
    }
    if (result == CALL_NOT_FOUND) {
      invalidate(app);
      return CALL_OK; /* Retry next loop; the attachment disappeared. */
    }
    if (result != CALL_OK) {
      return result;
    }
  }
  result = gamepad_catalog(app->gamepad, &confirmed);
  if (result != CALL_OK) {
    return result;
  }
  if (catalog.generation != confirmed.generation) {
    invalidate(app);
    return CALL_OK;
  }
  app->catalog = catalog;
  app->selected = selected;
  app->info = info;
  app->refresh = false;
  if (catalog.count) {
    accept(app, &state);
  } else {
    invalidate(app);
  }
  return CALL_OK;
}

static enum call_status drain_gamepad(struct app *app)
{
  struct gamepad_event event;
  enum call_status result;
  while ((result = gamepad_read(app->gamepad, GAMEPAD_READ_POLL, &event)) == CALL_OK) {
    if (event.type == GAMEPAD_RESET || event.type == GAMEPAD_ATTACHED ||
        event.type == GAMEPAD_REMOVED ||
        event.catalog_generation != app->catalog.generation) {
      invalidate(app);
      app->reset = true;
      app->refresh = true;
    }
    if (event.type == GAMEPAD_FOCUS_LOST || event.type == GAMEPAD_FOCUS_GAINED) {
      invalidate(app);
      app->focused = (event.flags & GAMEPAD_EVENT_FOCUSED) != 0;
      app->reset = true;
      app->refresh = true;
    }
    if (!app->refresh && event.instance == app->info.instance &&
        event.sequence >= app->accepted.sequence) {
      accept(app, &event);
    }
  }
  return result == CALL_TIMED_OUT ? CALL_OK : result;
}

static enum call_status drain_keyboard(struct app *app)
{
  struct keyboard_event event;
  enum call_status result;
  while ((result = keyboard_read(app->keyboard, KEYBOARD_READ_POLL, &event)) == CALL_OK) {
    if (!(event.flags & KEYBOARD_EVENT_FOCUSED) || event.action != KEY_PRESS) {
      continue;
    }
    if (event.key == KEY_ESCAPE) {
      app->quit = true;
    } else if (app->catalog.count > 1 && (event.key == KEY_TAB ||
        event.key == KEY_LEFT || event.key == KEY_RIGHT)) {
      uint32_t count = app->catalog.count;
      app->selected = (app->selected + (event.key == KEY_LEFT ? count - 1 : 1)) % count;
      /* Preserve the explicit selection through the catalog recovery path. */
      app->info.instance = app->catalog.instances[app->selected];
      invalidate(app);
      app->refresh = true;
    }
  }
  return result == CALL_TIMED_OUT ? CALL_OK : result;
}

static enum call_status resize(struct app *app)
{
  struct display_size_reply size;
  enum call_status result = display_size(app->display, &size);
  if (result != CALL_OK) {
    return result;
  }
  if (size.generation != app->buffer.generation) {
    result = display_replace(app->display, size.generation, &app->buffer);
    if (result == CALL_BUSY) {
      return CALL_OK;
    }
    if (result != CALL_OK) {
      return result;
    }
    app->slot = 0;
  }
  int scale = (int)(app->buffer.width * 1000 / CANVAS_WIDTH);
  int vertical = (int)(app->buffer.height * 1000 / CANVAS_HEIGHT);
  if (scale > vertical) {
    scale = vertical;
  }
  if (scale < 1) {
    return CALL_LIMIT;
  }
  if (scale != app->scale) {
    struct font_size *small = NULL, *normal = NULL, *title = NULL;
    unsigned pixels = 16 * scale / 1000;
    if (!pixels) {
      pixels = 1;
    }
    enum font_result font_result = font_size_create(app->face, pixels, NULL, &small);
    if (font_result == FONT_OK) {
      font_result = font_size_create(app->face, pixels * 5 / 4 + 1, NULL, &normal);
    }
    if (font_result == FONT_OK) {
      font_result = font_size_create(app->face, pixels * 2, NULL, &title);
    }
    if (font_result != FONT_OK) {
      font_size_destroy(small);
      font_size_destroy(normal);
      font_size_destroy(title);
      fprintf(stderr, "gamepadtest: font size: %s\n", font_result_string(font_result));
      return CALL_UNAVAILABLE;
    }
    font_size_destroy(app->small);
    font_size_destroy(app->normal);
    font_size_destroy(app->title);
    app->small = small;
    app->normal = normal;
    app->title = title;
    app->scale = scale;
  }
  app->left = ((int)app->buffer.width - position(app, CANVAS_WIDTH)) / 2;
  app->top = ((int)app->buffer.height - position(app, CANVAS_HEIGHT)) / 2;
  app->dirty = true;
  return CALL_OK;
}

static enum call_status run(struct app *app)
{
  enum call_status result = resize(app);
  app->refresh = true;
  while (result == CALL_OK && !app->quit) {
    result = drain_keyboard(app);
    if (result == CALL_OK) {
      result = drain_gamepad(app);
    }
    if (result == CALL_OK && app->refresh) {
      result = refresh_catalog(app);
    }
    if (result != CALL_OK || app->quit) {
      break;
    }
    if (app->dirty) {
      result = render(app);
      if (result != CALL_OK) {
        break;
      }
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
      return CALL_UNAVAILABLE;
    }
    uint64_t deadline = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec + UINT64_C(1000000000);
    struct wait_interest interests[] = {
      {app->keyboard, WAIT_READABLE, 0}, {app->gamepad, WAIT_READABLE, 0},
      {app->display, WAIT_RESIZED, app->buffer.generation},
    };
    uint64_t ready[3];
    result = wait_many(interests, 3, deadline, ready);
    if (result == CALL_TIMED_OUT) {
      result = CALL_OK;
      continue;
    }
    if (result == CALL_OK) {
      if ((ready[0] | ready[1] | ready[2]) & WAIT_ERROR) {
        return CALL_UNAVAILABLE;
      }
      if (ready[2] & WAIT_RESIZED) {
        result = resize(app);
      }
    }
  }
  return result;
}

int main(void)
{
  struct app app = {
    .display = bootstrap_capability("display"),
    .keyboard = bootstrap_capability("keyboard"),
    .gamepad = bootstrap_capability("gamepad"),
  };
  if (app.display == HANDLE_INVALID || app.keyboard == HANDLE_INVALID ||
      app.gamepad == HANDLE_INVALID) {
    fputs("gamepadtest: display, keyboard and gamepad grants are required\n", stderr);
    return 1;
  }
  enum font_result font_result = font_open(FONT_PATH, NULL, &app.face);
  if (font_result != FONT_OK) {
    fprintf(stderr, "gamepadtest: %s: %s\n", FONT_PATH, font_result_string(font_result));
    return 1;
  }
  enum call_status result = display_acquire(app.display, &app.buffer);
  bool display_owned = result == CALL_OK;
  bool keyboard_owned = false, gamepad_owned = false;
  if (result == CALL_OK) {
    result = keyboard_acquire(app.keyboard);
    keyboard_owned = result == CALL_OK;
  }
  if (result == CALL_OK) {
    result = gamepad_acquire(app.gamepad);
    gamepad_owned = result == CALL_OK;
  }
  if (result == CALL_OK) {
    result = run(&app);
  }
  enum call_status gamepad_released = gamepad_owned ? gamepad_release(app.gamepad) : CALL_OK;
  enum call_status keyboard_released = keyboard_owned ? keyboard_release(app.keyboard) : CALL_OK;
  enum call_status display_released = display_owned ? display_release(app.display) : CALL_OK;
  font_size_destroy(app.title);
  font_size_destroy(app.normal);
  font_size_destroy(app.small);
  font_close(app.face);
  if (result != CALL_OK || gamepad_released != CALL_OK || keyboard_released != CALL_OK ||
      display_released != CALL_OK) {
    fprintf(stderr, "gamepadtest: status %u; release gamepad %u, keyboard %u, display %u\n",
        result, gamepad_released, keyboard_released, display_released);
    return 1;
  }
  return 0;
}
