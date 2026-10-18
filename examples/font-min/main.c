/* font-min: pointer-min plus text.
 *
 * It opens nine font styles, draws one line of text in each, and shows the
 * pointer position as text that changes while you move the mouse. Escape quits.
 * Read pointer-min first: the display, keyboard and pointer code is the same.
 * Only the font code is new, and every part of it is commented.
 *
 * Build it with the host SDK (userspace/Makefile builds it as font-min) or
 * inside Pyxis, where the image ships this file as boot://share/examples/.
 * libfont is not a default library, so name it:
 *
 *   tcc boot://share/examples/font-min.c -lfont -o font-min.pxe
 *   ./font-min.pxe
 *
 * Run it from a local shell that was given the display, keyboard and pointer
 * grants, with a mouse present and the fonts under boot://share/fonts/.
 */
#include <display.h>
#include <environment.h>
#include <font.h>
#include <keyboard.h>
#include <pointer.h>
#include <startup.h>
#include <wait.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define STYLE_COUNT 9
#define FONT_PIXELS 24
#define MARGIN 16
#define TEXT_RGB 0xe8e8f0
#define BACKGROUND_RGB 0x202028
#define IDLE_WAIT_NS UINT64_C(1000000000)

/* libfont positions text in 26.6 fixed point: 64 units are one pixel. */
#define FIXED_ONE 64

struct style {
  const char *label; /* Also the text drawn in that style. */
  const char *path;
};

/* The last bytes of the first label are the UTF-8 for U+F013, a Nerd Font gear.
 * Icons are ordinary code points, so they need no special call. */
static const struct style styles[STYLE_COUNT] = {
  {"Caskaydia Cove Regular \xef\x80\x93",
      "boot://share/fonts/caskaydia-cove/CaskaydiaCoveNerdFont-Regular.ttf"},
  {"Caskaydia Cove Bold", "boot://share/fonts/caskaydia-cove/CaskaydiaCoveNerdFont-Bold.ttf"},
  {"Caskaydia Cove Italic", "boot://share/fonts/caskaydia-cove/CaskaydiaCoveNerdFont-Italic.ttf"},
  {"Caskaydia Cove Bold Italic",
      "boot://share/fonts/caskaydia-cove/CaskaydiaCoveNerdFont-BoldItalic.ttf"},
  {"IBM Plex Sans Regular", "boot://share/fonts/ibm-plex-sans/IBMPlexSans-Regular.ttf"},
  {"IBM Plex Sans Bold", "boot://share/fonts/ibm-plex-sans/IBMPlexSans-Bold.ttf"},
  {"IBM Plex Serif Regular", "boot://share/fonts/ibm-plex-serif/IBMPlexSerif-Regular.ttf"},
  {"IBM Plex Serif Bold", "boot://share/fonts/ibm-plex-serif/IBMPlexSerif-Bold.ttf"},
  {"IBM Plex Serif Italic", "boot://share/fonts/ibm-plex-serif/IBMPlexSerif-Italic.ttf"},
};

struct app {
  handle_t display, keyboard, pointer;
  struct display_buffer buffer; /* The mapped frame slots and their layout. */
  uint64_t slot;                /* The slot we may draw into next. */
  struct pointer_geometry geometry;
  int64_t x, y;                 /* Pointer position in the current geometry. */
  bool dirty;                   /* The frame needs redrawing. */
  /* One face (the font file) and one size (that face at FONT_PIXELS) per style.
   * A zero-filled app means nothing is open yet. */
  struct font_face *faces[STYLE_COUNT];
  struct font_size *sizes[STYLE_COUNT];
};

/* A process receives its authorities as named grants in a startup environment.
 * Looking one up hands this process its own copy, which lives until exit. */
static handle_t find_grant(const char *name)
{
  handle_t grant;
  if (environment_capability(startup_environment_handle(), name, &grant) != CALL_OK) {
    return HANDLE_INVALID;
  }
  return grant;
}

/* wait_many takes an absolute deadline on the monotonic clock, so the loop
 * needs the current time. libc's CLOCK_MONOTONIC reads the same clock. */
static bool now_ns(uint64_t *now)
{
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
    return false;
  }
  *now = (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
  return true;
}

/* The framebuffer is 32-bit words. Where each colour channel sits is reported
 * by the display, not fixed, so pack colours with the returned shifts. */
static uint32_t pack_rgb(const struct display_buffer *buffer, uint32_t rgb)
{
  return (rgb >> 16 & 0xff) << buffer->red_shift | (rgb >> 8 & 0xff) << buffer->green_shift |
      (rgb & 0xff) << buffer->blue_shift;
}

/* Every font call reports failure as an enum font_result. font_result_string
 * turns it into a name such as BAD_FONT or LIMIT. */
static bool open_fonts(struct app *app)
{
  for (int i = 0; i < STYLE_COUNT; ++i) {
    /* NULL options mean the default limits. font_open reads the whole file into
     * memory, which the face then owns. A size keeps its face alive, but this
     * program holds both and closes both. On failure the output stays NULL. */
    enum font_result result = font_open(styles[i].path, NULL, &app->faces[i]);
    if (result == FONT_OK) {
      result = font_size_create(app->faces[i], FONT_PIXELS, NULL, &app->sizes[i]);
    }
    if (result != FONT_OK) {
      fprintf(stderr, "font-min: %s: %s\n", styles[i].path, font_result_string(result));
      return false;
    }
  }
  return true;
}

/* Sizes go before the faces they use, and the last opened goes first. Closing
 * a NULL pointer does nothing, so this is also safe after a partial open. */
static void close_fonts(struct app *app)
{
  for (int i = STYLE_COUNT - 1; i >= 0; --i) {
    font_size_destroy(app->sizes[i]);
    font_close(app->faces[i]);
  }
}

/* An 8-bit mix of two 0xRRGGBB colours. Coverage 0 gives the background and 255
 * gives the foreground. libfont's own helper blends in linear light instead,
 * which looks better; this plain version is easier to read. */
static uint32_t mix_rgb(uint32_t foreground, uint32_t background, uint32_t coverage)
{
  uint32_t result = 0;
  for (int shift = 0; shift < 24; shift += 8) {
    uint32_t front = foreground >> shift & 0xff;
    uint32_t back = background >> shift & 0xff;
    result |= ((front * coverage + back * (255 - coverage)) / 255) << shift;
  }
  return result;
}

/* Draws text without libfont's helper, so you can see what the library gives a
 * caller. x and baseline are in pixels; the baseline is the line letters sit on. */
static enum font_result draw_by_hand(const struct app *app, struct font_size *size, int64_t x,
    int64_t baseline, const char *text)
{
  const struct display_buffer *buffer = &app->buffer;
  uint8_t *pixels = (uint8_t *)display_slot_address(buffer, app->slot);

  /* A run walks the text and returns one positioned glyph per step. The text
   * and the size must stay valid while the run is in use. */
  struct font_run run;
  enum font_result result = font_run_begin(size, text, strlen(text), x * FIXED_ONE,
      baseline * FIXED_ONE, &run);
  if (result != FONT_OK) {
    return result;
  }
  struct font_positioned_glyph glyph;
  while ((result = font_run_next(&run, &glyph)) == FONT_OK) {
    /* glyph.x and glyph.y are where the top-left of the coverage bitmap goes, in
     * whole pixels. They already include the glyph's side bearing and its height
     * above the baseline. Draw there, not at the run's own pen position: the pen
     * has already moved on to where the next glyph will start. */
    const struct font_glyph *mask = &glyph.glyph;
    for (uint32_t row = 0; row < mask->height; ++row) {
      int64_t y = glyph.y + (int64_t)row;
      if (y < 0 || y >= (int64_t)buffer->height) {
        continue;
      }
      uint32_t *line = (uint32_t *)(pixels + (uint64_t)y * buffer->pitch);
      for (uint32_t column = 0; column < mask->width; ++column) {
        /* Coverage is one byte per pixel, rows are stride bytes apart: 0 is no
         * ink and 255 is full ink. The bytes are borrowed from the library and
         * stay valid only until the next call that makes a glyph on this size,
         * which is the next font_run_next. Use them now, keep none. */
        uint32_t coverage = mask->coverage[row * mask->stride + column];
        int64_t px = glyph.x + (int64_t)column;
        if (coverage == 0 || px < 0 || px >= (int64_t)buffer->width) {
          continue;
        }
        /* The caller owns clipping too: bitmaps can hang off any screen edge. */
        line[px] = pack_rgb(buffer, mix_rgb(TEXT_RGB, BACKGROUND_RGB, coverage));
      }
    }
  }
  /* END is the normal finish. Anything else is an error. */
  return result == FONT_END ? FONT_OK : result;
}

static enum call_status present(struct app *app)
{
  const struct display_buffer *buffer = &app->buffer;
  uint8_t *pixels = (uint8_t *)display_slot_address(buffer, app->slot);
  uint32_t background = pack_rgb(buffer, BACKGROUND_RGB);

  /* A slot still holds the frame it last showed, so every redraw must cover
   * every pixel. Clear the whole slot first, then draw the text on top. */
  for (uint64_t y = 0; y < buffer->height; ++y) {
    /* Rows are pitch bytes apart, which can exceed width * 4. */
    uint32_t *row = (uint32_t *)(pixels + y * buffer->pitch);
    for (uint64_t x = 0; x < buffer->width; ++x) {
      row[x] = background;
    }
  }

  /* The helper draws into any XRGB8888 pixels you describe. */
  struct font_xrgb8888 surface = {
    .pixels = (uint32_t *)pixels,
    .capacity_bytes = (size_t)(buffer->pitch * buffer->height),
    .stride_bytes = (size_t)buffer->pitch,
    .width = (uint32_t)buffer->width,
    .height = (uint32_t)buffer->height,
    .red_shift = (uint8_t)buffer->red_shift,
    .green_shift = (uint8_t)buffer->green_shift,
    .blue_shift = (uint8_t)buffer->blue_shift,
  };

  /* One line per style. The baseline moves down by each font's own line height,
   * and metrics come from the size, so every font spaces itself. */
  enum font_result result = FONT_OK;
  font_fixed baseline = MARGIN * FIXED_ONE;
  for (int i = 0; i < STYLE_COUNT && result == FONT_OK; ++i) {
    struct font_metrics metrics;
    result = font_get_metrics(app->sizes[i], &metrics);
    if (result == FONT_OK) {
      baseline += metrics.line_advance;
      result = font_draw_xrgb8888(app->sizes[i], &surface, MARGIN * FIXED_ONE, baseline,
          styles[i].label, strlen(styles[i].label), TEXT_RGB);
    }
  }

  /* The pointer line is drawn by hand. The first style is monospaced, so the
   * numbers do not shift sideways as they change. */
  if (result == FONT_OK) {
    char text[64];
    snprintf(text, sizeof(text), "pointer %lld, %lld", (long long)app->x, (long long)app->y);
    result = draw_by_hand(app, app->sizes[0], MARGIN, (int64_t)buffer->height - MARGIN, text);
  }
  if (result != FONT_OK) {
    /* run() can only return a call status, so name the real cause here. */
    fprintf(stderr, "font-min: drawing failed: %s\n", font_result_string(result));
    return CALL_UNAVAILABLE;
  }

  /* Submitting hands the finished slot to the presenter, and the very first
   * submit is what makes the program's graphics visible. The reply names the
   * slot to draw into next; never write a slot again before a reply returns it. */
  struct display_submit_reply reply;
  enum call_status status = display_submit(app->display, app->slot, &reply);
  if (status == CALL_OK) {
    app->slot = reply.next;
    app->dirty = false;
  }
  return status;
}

/* The pointer's geometry carries a generation, which changes when the screen
 * is resized. The frame slots are tied to a generation, so a program on a new
 * one must ask the display for a fresh set before it draws again. */
static enum call_status refresh_geometry(struct app *app)
{
  enum call_status status = pointer_geometry(app->pointer, &app->geometry);
  if (status != CALL_OK || app->geometry.generation == app->buffer.generation) {
    return status;
  }
  status = display_replace(app->display, app->geometry.generation, &app->buffer);
  if (status == CALL_BUSY) {
    /* The screen changed again while we were asking. The display still reports
     * a resize we have not caught up with, so the next wait retries. */
    return CALL_OK;
  }
  if (status != CALL_OK) {
    return status;
  }
  /* Replacement returns fresh slots with nothing drawn: start from slot 0. It
   * also gives the pointer a new mapping identity, so read the geometry again. */
  app->slot = 0;
  app->dirty = true;
  return pointer_geometry(app->pointer, &app->geometry);
}

static enum call_status drain_pointer(struct app *app)
{
  struct pointer_event event;
  enum call_status status;
  /* POLL returns the next queued event without blocking, and TIMED_OUT when
   * the queue is empty. Read until then so no event is left behind. */
  while ((status = pointer_read(app->pointer, POINTER_READ_POLL, &event)) == CALL_OK) {
    app->dirty = true;
    if (event.type == POINTER_GEOMETRY_CHANGED) {
      status = refresh_geometry(app);
      if (status != CALL_OK) {
        return status;
      }
    }
    /* Positions are only meaningful in the geometry they were measured in.
     * An event from another generation or mapping is stale; skip it. */
    if (event.generation == app->geometry.generation &&
        event.mapping_identity == app->geometry.mapping_identity) {
      app->x = event.x;
      app->y = event.y;
    }
  }
  return status == CALL_TIMED_OUT ? CALL_OK : status;
}

/* Sets quit when Escape was pressed. */
static enum call_status drain_keyboard(handle_t keyboard, bool *quit)
{
  struct keyboard_event event;
  enum call_status status;
  /* Key events are also reported with action KEY_RELEASE, KEY_REPEAT, focus
   * changes and KEY_STATE_RESET. This program holds no key state, so a lost
   * release cannot leave it stuck and only a fresh press matters. */
  while ((status = keyboard_read(keyboard, KEYBOARD_READ_POLL, &event)) == CALL_OK) {
    if (event.action == KEY_PRESS && event.key == KEY_ESCAPE) {
      *quit = true;
    }
  }
  return status == CALL_TIMED_OUT ? CALL_OK : status;
}

static enum call_status run(struct app *app)
{
  enum call_status status = refresh_geometry(app);
  if (status != CALL_OK) {
    return status;
  }
  /* Start in the middle until the first pointer event reports a position. */
  app->x = (int64_t)app->geometry.width / 2;
  app->y = (int64_t)app->geometry.height / 2;
  app->dirty = true;

  bool quit = false;
  while (!quit) {
    status = drain_keyboard(app->keyboard, &quit);
    if (status == CALL_OK) {
      status = drain_pointer(app);
    }
    if (status != CALL_OK) {
      return status;
    }
    if (quit) {
      break;
    }
    if (app->dirty) {
      status = present(app);
      if (status != CALL_OK) {
        return status;
      }
    }

    /* Sleep until the pointer or keyboard has an event, or the display was
     * resized from the generation we hold, instead of polling. READABLE only
     * says an event is queued; the POLL reads above still consume it. */
    struct wait_interest interests[] = {
      {app->pointer, WAIT_READABLE, 0},
      {app->keyboard, WAIT_READABLE, 0},
      {app->display, WAIT_RESIZED, app->buffer.generation},
    };
    uint64_t ready[3];
    uint64_t now;
    if (!now_ns(&now)) {
      return CALL_UNAVAILABLE;
    }
    /* Deadlines may be at most 30 seconds ahead. Nothing needs the timeout:
     * on TIMED_OUT the loop just waits again. */
    status = wait_many(interests, 3, now + IDLE_WAIT_NS, ready);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    /* ERROR means the input session was lost, so there is nothing to wait for. */
    if ((ready[0] | ready[1] | ready[2]) & WAIT_ERROR) {
      return CALL_UNAVAILABLE;
    }
    if (ready[2] & WAIT_RESIZED) {
      status = refresh_geometry(app);
      if (status != CALL_OK) {
        return status;
      }
    }
  }
  return CALL_OK;
}

int main(void)
{
  struct app app = {
    .display = find_grant("display"),
    .keyboard = find_grant("keyboard"),
    .pointer = find_grant("pointer"),
  };
  if (app.display == HANDLE_INVALID || app.keyboard == HANDLE_INVALID ||
      app.pointer == HANDLE_INVALID) {
    fputs("font-min: needs display, keyboard and pointer grants\n", stderr);
    return 1;
  }

  /* Open the fonts before taking the display, so a missing file is reported on
   * the normal terminal instead of behind a graphics screen. They are closed
   * last, after the display, which is the reverse of the order we took them. */
  if (!open_fonts(&app)) {
    close_fonts(&app);
    return 1;
  }

  /* Sessions are exclusive and belong to this process, not to the handles.
   * Acquire the display first: the pointer is only available to the owner of
   * the space's graphics. Release in the opposite order, on every exit path;
   * if we die without doing so, process exit releases them anyway. */
  enum call_status status = display_acquire(app.display, &app.buffer);
  if (status == CALL_OK) {
    status = keyboard_acquire(app.keyboard);
    if (status == CALL_OK) {
      status = pointer_acquire(app.pointer);
      if (status == CALL_OK) {
        status = run(&app);
        pointer_release(app.pointer);
      }
      keyboard_release(app.keyboard);
    }
    display_release(app.display);
  }
  close_fonts(&app);

  if (status != CALL_OK) {
    /* Without a working mouse, ACQUIRE reports UNAVAILABLE. */
    fprintf(stderr, "font-min: failed (status %u)\n", (unsigned)status);
    return 1;
  }
  return 0;
}
