/* pointer-min: the smallest complete Pyxis pointer program.
 *
 * It draws a square that follows the pointer, turns it green while the left
 * button is held and quits on Escape. It does not lock the pointer, warp it,
 * replace the cursor image or read the wheel; mousetest shows those.
 *
 * Build it with the host SDK (userspace/Makefile builds it as pointer-min) or
 * inside Pyxis, where the image ships this file as boot://share/examples/:
 *
 *   tcc boot://share/examples/pointer-min.c -o pointer-min.pxe
 *   ./pointer-min.pxe
 *
 * Run it from a local shell. The program only works on a space that was given
 * the display, keyboard and pointer grants, and with a mouse present.
 */
#include <display.h>
#include <environment.h>
#include <keyboard.h>
#include <pointer.h>
#include <startup.h>
#include <wait.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SQUARE_SIZE 32
#define IDLE_WAIT_NS UINT64_C(1000000000)

struct app {
  handle_t display, keyboard, pointer;
  struct display_buffer buffer; /* The mapped frame slots and their layout. */
  uint64_t slot;                /* The slot we may draw into next. */
  struct pointer_geometry geometry;
  int64_t x, y;                 /* Pointer position in the current geometry. */
  uint32_t buttons;             /* Buttons held, as POINTER_BUTTON_* bits. */
  bool dirty;                   /* The frame needs redrawing. */
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
static uint32_t pack_rgb(const struct display_buffer *buffer, uint8_t red, uint8_t green,
    uint8_t blue)
{
  return (uint32_t)red << buffer->red_shift | (uint32_t)green << buffer->green_shift |
      (uint32_t)blue << buffer->blue_shift;
}

static void fill_rect(const struct app *app, int64_t left, int64_t top, int64_t width,
    int64_t height, uint32_t color)
{
  const struct display_buffer *buffer = &app->buffer;
  uint8_t *pixels = (uint8_t *)display_slot_address(buffer, app->slot);
  int64_t right = left + width < (int64_t)buffer->width ? left + width : (int64_t)buffer->width;
  int64_t bottom = top + height < (int64_t)buffer->height ? top + height : (int64_t)buffer->height;
  for (int64_t y = top < 0 ? 0 : top; y < bottom; ++y) {
    /* Rows are pitch bytes apart, which can exceed width * 4. */
    uint32_t *row = (uint32_t *)(pixels + (uint64_t)y * buffer->pitch);
    for (int64_t x = left < 0 ? 0 : left; x < right; ++x) {
      row[x] = color;
    }
  }
}

static enum call_status present(struct app *app)
{
  const struct display_buffer *buffer = &app->buffer;
  uint32_t background = pack_rgb(buffer, 0x20, 0x20, 0x28);
  uint32_t color = app->buttons & POINTER_BUTTON_LEFT ?
      pack_rgb(buffer, 0x2e, 0xa0, 0x43) : pack_rgb(buffer, 0x38, 0x7a, 0xd6);

  /* A slot still holds the frame it last showed, so every redraw must cover
   * every pixel. Clear the whole slot, then draw the square on top. */
  fill_rect(app, 0, 0, (int64_t)buffer->width, (int64_t)buffer->height, background);
  fill_rect(app, app->x - SQUARE_SIZE / 2, app->y - SQUARE_SIZE / 2, SQUARE_SIZE, SQUARE_SIZE,
      color);

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

static void apply_pointer_event(struct app *app, const struct pointer_event *event)
{
  /* Anything except plain input (focus lost or gained, enter and leave, a
   * geometry change, or a reset after the kernel dropped events) can mean a
   * release was missed. Treat every button as up until a fresh press arrives. */
  if (event->type != POINTER_INPUT) {
    app->buttons = 0;
  }
  app->dirty = true;
}

static enum call_status drain_pointer(struct app *app)
{
  struct pointer_event event;
  enum call_status status;
  /* POLL returns the next queued event without blocking, and TIMED_OUT when
   * the queue is empty. Read until then so no event is left behind. */
  while ((status = pointer_read(app->pointer, POINTER_READ_POLL, &event)) == CALL_OK) {
    apply_pointer_event(app, &event);
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
      if (event.type == POINTER_INPUT) {
        app->buttons = event.buttons;
      }
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
    fputs("pointer-min: needs display, keyboard and pointer grants\n", stderr);
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

  if (status != CALL_OK) {
    /* Without a working mouse, ACQUIRE reports UNAVAILABLE. */
    fprintf(stderr, "pointer-min: failed (status %u)\n", (unsigned)status);
    return 1;
  }
  return 0;
}
