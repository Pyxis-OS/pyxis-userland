#include <clock.h>
#include <display.h>
#include <keyboard.h>
#include <startup.h>
#include <stdio.h>
#include <string.h>
#include <wait.h>

#define ITERATION_LIMIT 512
#define FRAME_INTERVAL_NS (1000000000ull / 30)
#define MAX_ELAPSED_NS 250000000ull
#define MIN_VIEW_WIDTH 0.000000000001
#define MAX_VIEW_WIDTH 16.0

struct view {
  double real;
  double imaginary;
  double width;
  double aspect;
};

struct controls {
  struct view view;
  bool held[KEY_COUNT];
  bool focused;
  bool quit;
  bool redraw;
  uint64_t updated_at;
  uint64_t observed_generation;
};

static bool moving(const struct controls *controls)
{
  return controls->held[KEY_RIGHT] != controls->held[KEY_LEFT] ||
      controls->held[KEY_DOWN] != controls->held[KEY_UP] ||
      controls->held[KEY_EQUAL] != controls->held[KEY_MINUS];
}

static enum call_status advance_view(struct controls *controls, handle_t clock)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }

  uint64_t elapsed = now - controls->updated_at;
  controls->updated_at = now;
  if (!controls->focused || !moving(controls) || elapsed == 0) {
    return CALL_OK;
  }

  /* A slow frame must not turn a held key into a large jump. */
  if (elapsed > MAX_ELAPSED_NS) {
    elapsed = MAX_ELAPSED_NS;
  }
  double seconds = (double)elapsed / 1000000000.0;
  struct view *view = &controls->view;
  int horizontal = controls->held[KEY_RIGHT] - controls->held[KEY_LEFT];
  int vertical = controls->held[KEY_DOWN] - controls->held[KEY_UP];
  int zoom = controls->held[KEY_EQUAL] - controls->held[KEY_MINUS];

  view->real += horizontal * view->width * seconds * 0.6;
  view->imaginary += vertical * view->width * view->aspect * seconds * 0.6;
  double scale = 1.0 + seconds * 1.5;
  if (zoom > 0) {
    view->width /= scale;
  } else if (zoom < 0) {
    view->width *= scale;
  }
  if (view->width < MIN_VIEW_WIDTH) {
    view->width = MIN_VIEW_WIDTH;
  } else if (view->width > MAX_VIEW_WIDTH) {
    view->width = MAX_VIEW_WIDTH;
  }
  controls->redraw = true;
  return CALL_OK;
}

static enum call_status receive_input(struct controls *controls,
                                     handle_t keyboard, handle_t clock,
                                     uint64_t flags)
{
  struct keyboard_event event;
  enum call_status status = keyboard_read(keyboard, flags, &event);
  if (status != CALL_OK) {
    return status;
  }

  if (event.action == KEY_FOCUS_GAINED || event.action == KEY_FOCUS_LOST ||
      event.action == KEY_STATE_RESET) {
    memset(controls->held, 0, sizeof(controls->held));
    controls->focused = (event.flags & KEYBOARD_EVENT_FOCUSED) != 0;
    controls->redraw = true;
    return clock_now(clock, &controls->updated_at);
  }

  /* Account for the old held state before applying this transition. Repeats
   * don't drive movement; elapsed time does. */
  status = advance_view(controls, clock);
  if (status != CALL_OK) {
    return status;
  }
  if (event.key < KEY_COUNT) {
    if (event.action == KEY_PRESS && controls->focused) {
      controls->held[event.key] = true;
      controls->quit = event.key == KEY_ESCAPE;
    } else if (event.action == KEY_RELEASE) {
      controls->held[event.key] = false;
    }
  }
  return CALL_OK;
}

static enum call_status poll_input(struct controls *controls,
                                  handle_t keyboard, handle_t clock)
{
  while (!controls->quit) {
    enum call_status status = receive_input(controls, keyboard, clock,
                                           KEYBOARD_READ_POLL);
    if (status == CALL_TIMED_OUT) {
      return CALL_OK;
    }
    if (status != CALL_OK) {
      return status;
    }
  }
  return CALL_OK;
}

/* Replacement happens before a row pointer is formed. A successful swap
 * aborts the old frame so its view and pixel palette are never reused. */
static enum call_status receive_events(struct display_buffer *buffer,
    struct controls *controls, handle_t display, handle_t keyboard, handle_t clock,
    uint64_t deadline, bool *replaced)
{
  *replaced = false;
  struct wait_interest interests[] = {
    {.handle = display, .events = WAIT_RESIZED,
     .observed_generation = controls->observed_generation},
    {.handle = keyboard, .events = WAIT_READABLE},
  };
  uint64_t events[2];
  enum call_status status = wait_many(interests, 2, deadline, events);
  if (status == CALL_TIMED_OUT) {
    return CALL_OK;
  }
  if (status != CALL_OK) {
    return status;
  }
  if (events[0] & WAIT_RESIZED) {
    struct display_size_reply size;
    status = display_size(display, &size);
    if (status != CALL_OK) {
      return status;
    }
    controls->observed_generation = size.generation;
    /* Allocation failure preserves the old frame. Do not retry this generation
     * on every render checkpoint; the next resize supplies another attempt. */
    if (display_replace(display, size.generation, buffer) == CALL_OK) {
      controls->view.aspect = (double)buffer->height / (double)buffer->width;
      controls->redraw = true;
      *replaced = true;
    }
  }
  if ((events[0] | events[1]) & WAIT_ERROR) {
    return CALL_UNAVAILABLE;
  }
  return events[1] & WAIT_READABLE ? poll_input(controls, keyboard, clock) : CALL_OK;
}

static unsigned escape_iterations(double real, double imaginary)
{
  double x = 0.0;
  double y = 0.0;
  unsigned iteration = 0;

  while (x * x + y * y <= 4.0 && iteration < ITERATION_LIMIT) {
    double next_x = x * x - y * y + real;
    y = 2.0 * x * y + imaginary;
    x = next_x;
    ++iteration;
  }
  return iteration;
}

static uint32_t pixel_color(const struct display_buffer *buffer, uint32_t rgb)
{
  return ((rgb >> 16) & 0xff) << buffer->red_shift |
         ((rgb >> 8) & 0xff) << buffer->green_shift |
         (rgb & 0xff) << buffer->blue_shift;
}

static enum call_status render(struct display_buffer *buffer,
                               struct controls *controls,
                               handle_t display, handle_t keyboard, handle_t clock)
{
  const uint32_t palette[] = {
    0x487fd4, 0x76a8f2, 0x269d9a, 0x52c4c0, 0x52aa60, 0x80d080,
    0xad9b49, 0xc7b461, 0xc26265, 0xe48383, 0xaf5bd1, 0xd58bf0,
    0xb4bcca, 0xdfe5ee,
  };
  uint32_t colors[sizeof(palette) / sizeof(palette[0])];
  for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
    colors[i] = pixel_color(buffer, palette[i]);
  }

  /* Input may change the next view while this frame is being drawn. */
  struct view view = controls->view;
  controls->redraw = false;
  double step = view.width / (double)buffer->width;
  double real_left = view.real - view.width / 2.0;
  double imaginary_top = view.imaginary - view.width * view.aspect / 2.0;

  for (size_t row = 0; row < buffer->height; ++row) {
    if (row % 8 == 0) {
      bool replaced;
      enum call_status status = receive_events(buffer, controls, display, keyboard,
          clock, 0, &replaced);
      if (status != CALL_OK || replaced || controls->quit || !controls->focused) {
        return status;
      }
    }

    volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)
        (buffer->address + row * buffer->pitch);
    double imaginary = imaginary_top + ((double)row + 0.5) * step;

    for (size_t column = 0; column < buffer->width; ++column) {
      double real = real_left + ((double)column + 0.5) * step;
      unsigned iteration = escape_iterations(real, imaginary);
      pixels[column] = iteration == ITERATION_LIMIT ? 0 :
          colors[iteration % (sizeof(colors) / sizeof(colors[0]))];
    }
  }
  return CALL_OK;
}

static enum call_status explore(struct display_buffer *buffer,
    handle_t display, handle_t keyboard, handle_t clock)
{
  struct controls controls = {
    .view = {
      .real = -0.75,
      .width = 3.5,
      .aspect = (double)buffer->height / (double)buffer->width,
    },
    .redraw = true,
    .observed_generation = buffer->generation,
  };
  enum call_status status = clock_now(clock, &controls.updated_at);
  if (status != CALL_OK) {
    return status;
  }

  while (!controls.quit) {
    if (!controls.focused || (!controls.redraw && !moving(&controls))) {
      uint64_t now;
      status = clock_now(clock, &now);
      if (status != CALL_OK || now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
        return status != CALL_OK ? status : CALL_LIMIT;
      }
      bool replaced;
      status = receive_events(buffer, &controls, display, keyboard, clock,
          now + WAIT_MAX_WAIT_NS, &replaced);
      if (status != CALL_OK) {
        return status;
      }
    }
    bool replaced;
    status = receive_events(buffer, &controls, display, keyboard, clock, 0, &replaced);
    if (status != CALL_OK || controls.quit) {
      return status;
    }
    status = advance_view(&controls, clock);
    if (status != CALL_OK) {
      return status;
    }
    if (!controls.focused) {
      continue;
    }

    uint64_t frame_started = controls.updated_at;
    if (controls.redraw) {
      status = render(buffer, &controls, display, keyboard, clock);
      if (status != CALL_OK || controls.quit) {
        return status;
      }
    }
    if (controls.focused && moving(&controls)) {
      if (frame_started > UINT64_MAX - FRAME_INTERVAL_NS) {
        return CALL_LIMIT;
      }
      status = receive_events(buffer, &controls, display, keyboard, clock,
          frame_started + FRAME_INTERVAL_NS, &replaced);
      if (status != CALL_OK) {
        return status;
      }
    }
  }
  return CALL_OK;
}

int main(void)
{
  handle_t display = startup_resource("display");
  handle_t keyboard = startup_resource("keyboard");
  handle_t clock = startup_resource("clock");
  if (display == HANDLE_INVALID || keyboard == HANDLE_INVALID ||
      clock == HANDLE_INVALID) {
    fputs("mandelbrot: display, keyboard and clock resources are required\n", stderr);
    return 1;
  }

  struct display_buffer buffer;
  enum call_status status = display_acquire(display, &buffer);
  if (status != CALL_OK) {
    fprintf(stderr, "mandelbrot: cannot acquire display (status %u)\n", (unsigned)status);
    return 1;
  }

  status = keyboard_acquire(keyboard);
  bool keyboard_owned = status == CALL_OK;
  if (keyboard_owned) {
    /* Selecting the single buffer first lets periodic presentation show progress.
     * The presenter may read pixels while we write them; tearing is allowed. */
    status = display_present(display);
    if (status == CALL_OK) {
      status = explore(&buffer, display, keyboard, clock);
    }
  }

  enum call_status keyboard_released = keyboard_owned ?
      keyboard_release(keyboard) : CALL_OK;
  enum call_status display_released = display_release(display);
  if (status != CALL_OK || keyboard_released != CALL_OK ||
      display_released != CALL_OK) {
    fprintf(stderr, "mandelbrot: status %u, keyboard release %u, display release %u\n",
        (unsigned)status, (unsigned)keyboard_released, (unsigned)display_released);
    return 1;
  }
  return 0;
}
