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
#define MARKER_RADIUS 6

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

struct colors {
  uint32_t panel, idle, left, middle, right, text, ink, paper, marker;
};

struct screen {
  const struct display_buffer *buffer;
  uint8_t *pixels;
  int64_t width, height;
  int64_t panel_width;
  uint8_t *ink; /* Drawing pad pixels, one byte each; kept apart from the marker. */
  struct colors colors;
};

struct mouse_state {
  int64_t x, y;
  uint32_t buttons;
  int scroll; /* -1 away from the user, +1 toward, 0 idle. */
  uint64_t scroll_until;
  bool focused;
  bool quit;
  bool dirty;
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
  case 'L': return glyph_l;
  case 'M': return glyph_m;
  case 'R': return glyph_r;
  case 'X': return glyph_x;
  case 'Y': return glyph_y;
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

static void draw_marker(const struct screen *screen, const struct mouse_state *state)
{
  for (int64_t offset = -MARKER_RADIUS; offset <= MARKER_RADIUS; ++offset) {
    if (offset) {
      put_pixel(screen, state->x + offset, state->y, screen->colors.marker);
      put_pixel(screen, state->x, state->y + offset, screen->colors.marker);
    }
  }
}

static void redraw(const struct screen *screen, const struct mouse_state *state)
{
  draw_panel(screen, state);
  draw_pad(screen);
  draw_marker(screen, state);
}

static int64_t clamp(int64_t value, int64_t low, int64_t high)
{
  return value < low ? low : value > high ? high : value;
}

static void apply_pointer(const struct screen *screen, struct mouse_state *state,
    const struct pointer_event *event, uint64_t now)
{
  state->focused = event->flags & POINTER_EVENT_FOCUSED;
  state->dirty = true;
  if (event->type != POINTER_INPUT) {
    /* Focus changes and resets end every held button. */
    state->buttons = 0;
    return;
  }

  /* One pixel per device count, without acceleration. */
  state->x = clamp(state->x + event->dx, 0, screen->width - 1);
  state->y = clamp(state->y + event->dy, 0, screen->height - 1);
  state->buttons = event->buttons;
  if (event->wheel) {
    state->scroll = event->wheel < 0 ? -1 : 1;
    state->scroll_until = now + SCROLL_SHOWN_NS;
  }
  if ((state->buttons & POINTER_BUTTON_LEFT) && state->x >= screen->panel_width) {
    int64_t pad_width = screen->width - screen->panel_width;
    screen->ink[state->y * pad_width + state->x - screen->panel_width] = 1;
  }
}

static void apply_key(struct mouse_state *state, const struct keyboard_event *event)
{
  state->focused = event->flags & KEYBOARD_EVENT_FOCUSED;
  if (event->key == KEY_ESCAPE && event->action == KEY_PRESS) {
    state->quit = true;
  }
}

static enum call_status run(const struct screen *screen, handle_t keyboard, handle_t pointer,
    handle_t clock)
{
  struct mouse_state state = {
    .x = screen->width / 2,
    .y = screen->height / 2,
    .focused = true,
    .dirty = true,
  };

  while (!state.quit) {
    struct keyboard_event key;
    enum call_status status;
    if (!state.focused) {
      /* Both sessions report focus; the keyboard's blocking read waits for it. */
      status = keyboard_read(keyboard, 0, &key);
      if (status != CALL_OK) {
        return status;
      }
      apply_key(&state, &key);
      continue;
    }

    while ((status = keyboard_read(keyboard, KEYBOARD_READ_POLL, &key)) == CALL_OK) {
      apply_key(&state, &key);
    }
    if (status != CALL_TIMED_OUT) {
      return status;
    }

    uint64_t now;
    status = clock_now(clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    struct pointer_event event;
    while ((status = pointer_read(pointer, POINTER_READ_POLL, &event)) == CALL_OK) {
      apply_pointer(screen, &state, &event, now);
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
      .marker = rgb(&buffer, 0xe0, 0x30, 0x30),
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
    status = display_present(display);
    if (status == CALL_OK) {
      status = run(&screen, keyboard, pointer, clock);
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
