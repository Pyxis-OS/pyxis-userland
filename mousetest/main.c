#include <clock.h>
#include <display.h>
#include <keyboard.h>
#include <pointer.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_INTERVAL_NS 10000000ull
#define SCROLL_SHOWN_NS 500000000ull
#define PANEL_PERCENT 30
#define GLYPH_WIDTH 5
#define GLYPH_HEIGHT 7
#define CURSOR_WIDTH 16
#define CURSOR_HEIGHT 20

/* 5x7 glyphs, one byte per row, bit 4 on the left. Only what the panel shows. */
static const uint8_t glyph_digits[10][GLYPH_HEIGHT] = {
  {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
  {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
  {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
  {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
  {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
  {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
  {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
  {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
  {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
  {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
};
static const uint8_t glyph_l[GLYPH_HEIGHT] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
static const uint8_t glyph_m[GLYPH_HEIGHT] = {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11};
static const uint8_t glyph_r[GLYPH_HEIGHT] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
static const uint8_t glyph_x[GLYPH_HEIGHT] = {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11};
static const uint8_t glyph_y[GLYPH_HEIGHT] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
static const uint8_t glyph_minus[GLYPH_HEIGHT] = {0, 0, 0, 0x1f, 0, 0, 0};
static const uint8_t glyph_a[GLYPH_HEIGHT] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
static const uint8_t glyph_c[GLYPH_HEIGHT] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
static const uint8_t glyph_d[GLYPH_HEIGHT] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
static const uint8_t glyph_e[GLYPH_HEIGHT] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
static const uint8_t glyph_f[GLYPH_HEIGHT] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
static const uint8_t glyph_i[GLYPH_HEIGHT] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
static const uint8_t glyph_k[GLYPH_HEIGHT] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
static const uint8_t glyph_o[GLYPH_HEIGHT] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
static const uint8_t glyph_t[GLYPH_HEIGHT] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
static const uint8_t glyph_w[GLYPH_HEIGHT] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a};

struct colors {
  uint32_t panel, idle, left, middle, right, text, ink, paper;
};

struct screen {
  const struct display_buffer *buffer;
  uint8_t *pixels;
  int64_t width, height;
  int64_t panel_width;
  uint8_t *ink; /* Drawing pad pixels, one byte each. */
  struct colors colors;
};

struct mouse_state {
  int64_t x, y;
  int32_t dx, dy;
  uint32_t buttons;
  int scroll; /* -1 away from the user, +1 toward, 0 idle. */
  uint64_t scroll_until;
  bool keyboard_focused;
  bool pointer_focused;
  bool locked;
  bool lock_desired;
  bool cursor_visible;
  bool quit;
  bool dirty;
  uint64_t observed_generation;
  struct pointer_geometry geometry;
};

static uint32_t rgb(const struct display_buffer *buffer, uint8_t red, uint8_t green, uint8_t blue)
{
  return (uint32_t)red << buffer->red_shift | (uint32_t)green << buffer->green_shift |
      (uint32_t)blue << buffer->blue_shift;
}

static void put_pixel(const struct screen *screen, int64_t x, int64_t y, uint32_t color)
{
  if (x < 0 || y < 0 || x >= screen->width || y >= screen->height) {
    return;
  }
  uint32_t *row = (uint32_t *)(screen->pixels + (uint64_t)y * screen->buffer->pitch);
  row[x] = color;
}

static void fill_rect(const struct screen *screen, int64_t x, int64_t y, int64_t width,
    int64_t height, uint32_t color)
{
  for (int64_t row = y; row < y + height; ++row) {
    for (int64_t column = x; column < x + width; ++column) {
      put_pixel(screen, column, row, color);
    }
  }
}

static const uint8_t *glyph(char character)
{
  if (character >= '0' && character <= '9') {
    return glyph_digits[character - '0'];
  }
  switch (character) {
  case 'A': return glyph_a;
  case 'C': return glyph_c;
  case 'D': return glyph_d;
  case 'E': return glyph_e;
  case 'F': return glyph_f;
  case 'I': return glyph_i;
  case 'K': return glyph_k;
  case 'L': return glyph_l;
  case 'M': return glyph_m;
  case 'O': return glyph_o;
  case 'R': return glyph_r;
  case 'T': return glyph_t;
  case 'W': return glyph_w;
  case 'X': return glyph_x;
  case 'Y': return glyph_y;
  case '-': return glyph_minus;
  default: return NULL;
  }
}

static void draw_text(const struct screen *screen, int64_t x, int64_t y, int64_t scale,
    const char *text, uint32_t color)
{
  for (; *text; ++text, x += (GLYPH_WIDTH + 1) * scale) {
    const uint8_t *rows = glyph(*text);
    if (!rows) {
      continue;
    }
    for (int row = 0; row < GLYPH_HEIGHT; ++row) {
      for (int column = 0; column < GLYPH_WIDTH; ++column) {
        if (rows[row] & (0x10 >> column)) {
          fill_rect(screen, x + column * scale, y + row * scale, scale, scale, color);
        }
      }
    }
  }
}

/* An isosceles triangle in a size x size box, pointing up or down. */
static void draw_arrow(const struct screen *screen, int64_t x, int64_t y, int64_t size,
    bool up, uint32_t color)
{
  for (int64_t row = 0; row < size; ++row) {
    int64_t depth = up ? row : size - 1 - row;
    int64_t half = depth / 2;
    fill_rect(screen, x + size / 2 - half, y + row, 2 * half + 1, 1, color);
  }
}

static void draw_button(const struct screen *screen, int64_t x, int64_t y, int64_t size,
    char label, bool pressed, uint32_t pressed_color)
{
  const struct colors *colors = &screen->colors;
  fill_rect(screen, x, y, size, size, pressed ? pressed_color : colors->idle);
  int64_t scale = size / 20 > 0 ? size / 20 : 1;
  char text[] = {label, '\0'};
  draw_text(screen, x + (size - GLYPH_WIDTH * scale) / 2,
      y + (size - GLYPH_HEIGHT * scale) / 2, scale, text, colors->text);
}

static void draw_panel(const struct screen *screen, const struct mouse_state *state)
{
  const struct colors *colors = &screen->colors;
  int64_t width = screen->panel_width;
  int64_t margin = width / 12;
  int64_t box = (width - 4 * margin) / 3;
  fill_rect(screen, 0, 0, width, screen->height, colors->panel);

  int64_t y = margin;
  draw_button(screen, margin, y, box, 'L', state->buttons & POINTER_BUTTON_LEFT, colors->left);
  draw_button(screen, 2 * margin + box, y, box, 'M',
      state->buttons & POINTER_BUTTON_MIDDLE, colors->middle);
  draw_button(screen, 3 * margin + 2 * box, y, box, 'R',
      state->buttons & POINTER_BUTTON_RIGHT, colors->right);

  y += box + margin;
  int64_t arrow = box;
  int64_t arrow_x = (width - arrow) / 2;
  draw_arrow(screen, arrow_x, y, arrow, true, state->scroll < 0 ? colors->left : colors->idle);
  y += arrow + margin / 2;
  draw_arrow(screen, arrow_x, y, arrow, false, state->scroll > 0 ? colors->left : colors->idle);

  y += arrow + margin;
  int64_t scale = box / 16 > 0 ? box / 16 : 1;
  char line[24];
  snprintf(line, sizeof(line), "X %lld", (long long)state->x);
  draw_text(screen, margin, y, scale, line, colors->text);
  y += (GLYPH_HEIGHT + 3) * scale;
  snprintf(line, sizeof(line), "Y %lld", (long long)state->y);
  draw_text(screen, margin, y, scale, line, colors->text);
  y += (GLYPH_HEIGHT + 3) * scale;
  draw_text(screen, margin, y, scale,
      state->locked ? "LOCK" : state->lock_desired ? "WAIT" : "FREE", colors->text);
  y += (GLYPH_HEIGHT + 3) * scale;
  snprintf(line, sizeof(line), "DX %ld", (long)state->dx);
  draw_text(screen, margin, y, scale, line, colors->text);
  y += (GLYPH_HEIGHT + 3) * scale;
  snprintf(line, sizeof(line), "DY %ld", (long)state->dy);
  draw_text(screen, margin, y, scale, line, colors->text);
}

static void draw_pad(const struct screen *screen)
{
  const struct colors *colors = &screen->colors;
  int64_t pad_width = screen->width - screen->panel_width;
  for (int64_t y = 0; y < screen->height; ++y) {
    uint32_t *row = (uint32_t *)(screen->pixels + (uint64_t)y * screen->buffer->pitch);
    const uint8_t *ink = screen->ink + y * pad_width;
    for (int64_t x = 0; x < pad_width; ++x) {
      row[screen->panel_width + x] = ink[x] ? colors->ink : colors->paper;
    }
  }
}

static void redraw(const struct screen *screen, const struct mouse_state *state)
{
  draw_panel(screen, state);
  draw_pad(screen);
}

static enum call_status custom_cursor(handle_t pointer)
{
  static const uint16_t rows[CURSOR_HEIGHT] = {
    0x8000, 0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00, 0xff00,
    0xff80, 0xffc0, 0xffe0, 0xfff0, 0xfff8, 0xff80, 0xcc00, 0x0c00,
    0x0600, 0x0600, 0x0300, 0x0300,
  };
  uint32_t pixels[CURSOR_WIDTH * CURSOR_HEIGHT] = {0};
  for (uint32_t y = 0; y < CURSOR_HEIGHT; ++y) {
    for (uint32_t x = 0; x < CURSOR_WIDTH; ++x) {
      uint16_t bit = UINT16_C(0x8000) >> x;
      if (rows[y] & bit) {
        bool interior = x && y && y + 1 < CURSOR_HEIGHT &&
            (rows[y] & (bit << 1)) && (rows[y] & (bit >> 1)) &&
            (rows[y - 1] & bit) && (rows[y + 1] & bit);
        pixels[y * CURSOR_WIDTH + x] = interior ? UINT32_C(0xff8fe6ff) :
            UINT32_C(0xff182028);
      }
    }
  }
  return pointer_set_image(pointer, pixels, CURSOR_WIDTH, CURSOR_HEIGHT, 1, 1);
}

static void apply_pointer(const struct screen *screen, struct mouse_state *state,
    const struct pointer_event *event, uint64_t now)
{
  state->pointer_focused = (event->flags & POINTER_EVENT_FOCUSED) != 0;
  state->dirty = true;
  if (event->generation == state->geometry.generation &&
      event->mapping_identity == state->geometry.mapping_identity) {
    state->x = event->x;
    state->y = event->y;
  }
  if (event->type == POINTER_GEOMETRY_CHANGED && state->locked &&
      (event->flags & POINTER_EVENT_LOCKED)) {
    state->buttons = event->buttons;
    return;
  }
  if (event->type != POINTER_INPUT) {
    /* Revocation/reset ends held controls; locked resize preserves them. */
    state->buttons = 0;
    state->scroll = 0;
    state->dx = 0;
    state->dy = 0;
    return;
  }
  if (!state->pointer_focused ||
      ((event->flags & POINTER_EVENT_LOCKED) != 0) != state->locked) {
    return;
  }

  state->buttons = event->buttons;
  state->dx = state->locked ? event->dx : 0;
  state->dy = state->locked ? event->dy : 0;
  if (event->wheel) {
    state->scroll = event->wheel < 0 ? -1 : 1;
    state->scroll_until = now + SCROLL_SHOWN_NS;
  }
  if (!state->locked && (state->buttons & POINTER_BUTTON_LEFT) &&
      state->x >= screen->panel_width &&
      state->x < screen->width && state->y >= 0 && state->y < screen->height) {
    int64_t pad_width = screen->width - screen->panel_width;
    screen->ink[state->y * pad_width + state->x - screen->panel_width] = 1;
  }
}

static enum call_status refresh_lock_state(struct mouse_state *state, handle_t pointer)
{
  uint64_t flags;
  enum call_status status = pointer_state(pointer, &flags);
  if (status != CALL_OK) {
    return status;
  }
  bool focused = (flags & POINTER_EVENT_FOCUSED) != 0;
  bool locked = (flags & POINTER_EVENT_LOCKED) != 0;
  if (state->pointer_focused != focused || state->locked != locked) {
    state->buttons = 0;
    state->scroll = 0;
    state->dx = 0;
    state->dy = 0;
    state->dirty = true;
  }
  state->pointer_focused = focused;
  state->locked = locked;
  return CALL_OK;
}

static enum call_status refresh_geometry(struct screen *screen, struct display_buffer *buffer,
    struct mouse_state *state, handle_t display, handle_t pointer)
{
  enum call_status status = pointer_geometry(pointer, &state->geometry);
  if (status != CALL_OK || state->geometry.generation == state->observed_generation) {
    return status;
  }
  state->observed_generation = state->geometry.generation;
  if (!state->locked) {
    state->buttons = 0;
    state->scroll = 0;
    state->dx = 0;
    state->dy = 0;
  }
  state->dirty = true;

  uint64_t width = state->geometry.width;
  uint64_t height = state->geometry.height;
  if (!width || !height || width > INT64_MAX / PANEL_PERCENT || height > INT64_MAX) {
    return CALL_BAD_REQUEST;
  }
  uint64_t panel_width = width * PANEL_PERCENT / 100;
  if (width - panel_width > SIZE_MAX / height) {
    return CALL_BAD_REQUEST;
  }
  uint8_t *ink = calloc((size_t)(width - panel_width) * (size_t)height, 1);
  if (!ink) {
    return CALL_OK;
  }
  struct display_buffer replacement;
  status = display_replace(display, state->geometry.generation, &replacement);
  if (status != CALL_OK) {
    /* Keep the old drawing and mapping; another geometry change can retry. */
    free(ink);
    return CALL_OK;
  }

  uint64_t old_pad_width = screen->width - screen->panel_width;
  uint64_t new_pad_width = width - panel_width;
  uint64_t copy_width = old_pad_width < new_pad_width ? old_pad_width : new_pad_width;
  uint64_t copy_height = (uint64_t)screen->height < height ? screen->height : height;
  for (uint64_t y = 0; y < copy_height; ++y) {
    memcpy(ink + y * new_pad_width, screen->ink + y * old_pad_width, copy_width);
  }
  free(screen->ink);
  *buffer = replacement;
  screen->pixels = (uint8_t *)(uintptr_t)buffer->address;
  screen->width = (int64_t)width;
  screen->height = (int64_t)height;
  screen->panel_width = (int64_t)panel_width;
  screen->ink = ink;
  screen->colors = (struct colors){
    .panel = rgb(buffer, 0x30, 0x30, 0x30),
    .idle = rgb(buffer, 0x58, 0x58, 0x58),
    .left = rgb(buffer, 0x2e, 0xa0, 0x43),
    .middle = rgb(buffer, 0xd2, 0x99, 0x22),
    .right = rgb(buffer, 0x38, 0x7a, 0xd6),
    .text = rgb(buffer, 0xf0, 0xf0, 0xf0),
    .ink = rgb(buffer, 0x00, 0x00, 0x00),
    .paper = rgb(buffer, 0xff, 0xff, 0xff),
  };
  return pointer_geometry(pointer, &state->geometry);
}

static enum call_status apply_key(struct mouse_state *state, handle_t pointer,
    const struct keyboard_event *event)
{
  state->keyboard_focused = (event->flags & KEYBOARD_EVENT_FOCUSED) != 0;
  if (!state->keyboard_focused || event->action != KEY_PRESS) {
    return CALL_OK;
  }
  switch (event->key) {
  case KEY_ESCAPE:
    state->quit = true;
    return CALL_OK;
  case KEY_H: {
    enum call_status status = pointer_set_visible(pointer, !state->cursor_visible);
    if (status == CALL_OK) {
      state->cursor_visible = !state->cursor_visible;
    }
    return status;
  }
  case KEY_D:
    return pointer_default_image(pointer);
  case KEY_C:
    return custom_cursor(pointer);
  case KEY_L:
    state->dirty = true;
    if (state->lock_desired || state->locked) {
      state->lock_desired = false;
      return pointer_unlock(pointer);
    }
    state->lock_desired = true;
    return pointer_lock(pointer);
  case KEY_W: {
    struct pointer_geometry geometry;
    enum call_status status = pointer_geometry(pointer, &geometry);
    if (status != CALL_OK) {
      return status;
    }
    uint64_t width = geometry.width < geometry.mapping_width ?
        geometry.width : geometry.mapping_width;
    uint64_t height = geometry.height < geometry.mapping_height ?
        geometry.height : geometry.mapping_height;
    return pointer_warp(pointer, (int64_t)(width / 2), (int64_t)(height / 2),
        geometry.generation, geometry.mapping_identity);
  }
  default:
    return CALL_OK;
  }
}

static enum call_status run(struct screen *screen, struct display_buffer *buffer,
    handle_t display, handle_t keyboard, handle_t pointer, handle_t clock)
{
  struct mouse_state state = {
    .cursor_visible = true,
    .dirty = true,
    .observed_generation = buffer->generation,
  };
  enum call_status status = refresh_geometry(screen, buffer, &state, display, pointer);
  if (status != CALL_OK) {
    return status;
  }

  while (!state.quit) {
    struct keyboard_event key;
    while ((status = keyboard_read(keyboard, KEYBOARD_READ_POLL, &key)) == CALL_OK) {
      enum call_status command = apply_key(&state, pointer, &key);
      if (command != CALL_OK) {
        fprintf(stderr, "mousetest: cursor command failed (status %u)\n", (unsigned)command);
      }
    }
    if (status != CALL_TIMED_OUT) {
      return status;
    }
    status = refresh_lock_state(&state, pointer);
    if (status != CALL_OK) {
      return status;
    }

    uint64_t now;
    status = clock_now(clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    struct pointer_event event;
    while ((status = pointer_read(pointer, POINTER_READ_POLL, &event)) == CALL_OK) {
      bool locked_geometry = event.type == POINTER_GEOMETRY_CHANGED &&
          state.locked && (event.flags & POINTER_EVENT_LOCKED);
      if (event.type != POINTER_INPUT && !locked_geometry) {
        state.buttons = 0;
        state.scroll = 0;
        state.dx = 0;
        state.dy = 0;
        state.dirty = true;
        state.pointer_focused = (event.flags & POINTER_EVENT_FOCUSED) != 0;
      }
      if (event.type == POINTER_ACTIVATED && state.lock_desired) {
        enum call_status lock_status = pointer_lock(pointer);
        if (lock_status != CALL_OK) {
          fprintf(stderr, "mousetest: lock refused (status %u)\n", (unsigned)lock_status);
        }
      }
      if (event.type != POINTER_INPUT) {
        enum call_status state_status = refresh_lock_state(&state, pointer);
        if (state_status != CALL_OK) {
          return state_status;
        }
      }
      if (event.type == POINTER_GEOMETRY_CHANGED ||
          state.observed_generation != state.geometry.generation ||
          event.generation != state.geometry.generation ||
          event.mapping_identity != state.geometry.mapping_identity) {
        enum call_status geometry_status = refresh_geometry(screen, buffer, &state,
            display, pointer);
        if (geometry_status != CALL_OK) {
          return geometry_status;
        }
      }
      bool locked_input = event.type == POINTER_INPUT && state.locked &&
          (event.flags & POINTER_EVENT_LOCKED);
      /* Relative counts do not change meaning when the mapping is replaced. */
      if (locked_input || locked_geometry ||
          (event.generation == state.geometry.generation &&
           event.mapping_identity == state.geometry.mapping_identity)) {
        apply_pointer(screen, &state, &event, now);
      }
    }
    if (status != CALL_TIMED_OUT) {
      return status;
    }
    if (state.scroll && now >= state.scroll_until) {
      state.scroll = 0;
      state.dirty = true;
    }

    if (state.dirty) {
      redraw(screen, &state);
      state.dirty = false;
    }
    status = clock_sleep_for(clock, POLL_INTERVAL_NS);
    if (status != CALL_OK) {
      return status;
    }
  }
  return CALL_OK;
}

int main(void)
{
  handle_t display = startup_resource("display");
  handle_t keyboard = startup_resource("keyboard");
  handle_t pointer = startup_resource("pointer");
  handle_t clock = startup_resource("clock");
  if (display == HANDLE_INVALID || keyboard == HANDLE_INVALID ||
      pointer == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("mousetest: display, keyboard, pointer and clock resources are required\n", stderr);
    return 1;
  }

  struct display_buffer buffer;
  enum call_status status = display_acquire(display, &buffer);
  if (status != CALL_OK) {
    fprintf(stderr, "mousetest: cannot acquire display (status %u)\n", (unsigned)status);
    return 1;
  }

  if (!buffer.width || !buffer.height || buffer.width > INT64_MAX / PANEL_PERCENT ||
      buffer.height > INT64_MAX ||
      buffer.width - buffer.width * PANEL_PERCENT / 100 > SIZE_MAX / buffer.height) {
    display_release(display);
    fputs("mousetest: invalid display dimensions\n", stderr);
    return 1;
  }

  struct screen screen = {
    .buffer = &buffer,
    .pixels = (uint8_t *)(uintptr_t)buffer.address,
    .width = (int64_t)buffer.width,
    .height = (int64_t)buffer.height,
    .panel_width = (int64_t)buffer.width * PANEL_PERCENT / 100,
    .colors = {
      .panel = rgb(&buffer, 0x30, 0x30, 0x30),
      .idle = rgb(&buffer, 0x58, 0x58, 0x58),
      .left = rgb(&buffer, 0x2e, 0xa0, 0x43),
      .middle = rgb(&buffer, 0xd2, 0x99, 0x22),
      .right = rgb(&buffer, 0x38, 0x7a, 0xd6),
      .text = rgb(&buffer, 0xf0, 0xf0, 0xf0),
      .ink = rgb(&buffer, 0x00, 0x00, 0x00),
      .paper = rgb(&buffer, 0xff, 0xff, 0xff),
    },
  };
  size_t pad_pixels = (size_t)(screen.width - screen.panel_width) * (size_t)screen.height;
  screen.ink = calloc(pad_pixels, 1);
  if (!screen.ink) {
    display_release(display);
    fputs("mousetest: cannot allocate the drawing pad\n", stderr);
    return 1;
  }

  enum call_status keyboard_status = keyboard_acquire(keyboard);
  enum call_status pointer_status = pointer_acquire(pointer);
  if (keyboard_status != CALL_OK) {
    status = keyboard_status;
  } else if (pointer_status != CALL_OK) {
    status = pointer_status;
  } else {
    status = custom_cursor(pointer);
    if (status == CALL_OK) {
      status = display_present(display);
    }
    if (status == CALL_OK) {
      status = run(&screen, &buffer, display, keyboard, pointer, clock);
    }
  }

  enum call_status pointer_released = pointer_status == CALL_OK ?
      pointer_release(pointer) : CALL_OK;
  enum call_status keyboard_released = keyboard_status == CALL_OK ?
      keyboard_release(keyboard) : CALL_OK;
  enum call_status display_released = display_release(display);
  free(screen.ink);
  if (pointer_status == CALL_UNAVAILABLE) {
    fputs("mousetest: no mouse is available\n", stderr);
    return 1;
  }
  if (status != CALL_OK || pointer_released != CALL_OK || keyboard_released != CALL_OK ||
      display_released != CALL_OK) {
    fprintf(stderr, "mousetest: status %u, pointer release %u, keyboard release %u, "
        "display release %u\n", (unsigned)status, (unsigned)pointer_released,
        (unsigned)keyboard_released, (unsigned)display_released);
    return 1;
  }
  return 0;
}
