#include "layout.h"
#include <stdint.h>
#include <string.h>

void mux_layout_init(struct mux_layout *layout, int pane)
{
  *layout = (struct mux_layout){.kind = MUX_BSP};
  layout->nodes[0] = (struct mux_node){.used = true, .pane = pane};
}

bool mux_layout_split(struct mux_layout *layout, int focused, int pane, enum mux_axis axis)
{
  int leaf = -1, first = -1, second = -1;
  for (int i = 0; i < MUX_NODES; ++i) {
    if (layout->nodes[i].used && layout->nodes[i].pane == focused) {
      leaf = i;
    } else if (!layout->nodes[i].used) {
      if (first < 0) {
        first = i;
      } else {
        second = i;
      }
    }
  }
  if (leaf < 0 || first < 0 || second < 0) {
    return false;
  }
  layout->nodes[first] = (struct mux_node){.used = true, .pane = focused};
  layout->nodes[second] = (struct mux_node){.used = true, .pane = pane};
  layout->nodes[leaf] = (struct mux_node){.used = true, .pane = -1,
      .first = first, .second = second, .axis = axis};
  layout->equal_axis = axis;
  return true;
}

void mux_layout_remove(struct mux_layout *layout, int pane)
{
  int leaf = -1;
  for (int i = 0; i < MUX_NODES; ++i) {
    if (layout->nodes[i].used && layout->nodes[i].pane == pane) {
      leaf = i;
      break;
    }
  }
  if (leaf < 0) {
    return;
  }
  if (!leaf) {
    layout->nodes[0].used = false;
    return;
  }
  for (int i = 0; i < MUX_NODES; ++i) {
    struct mux_node *parent = &layout->nodes[i];
    if (!parent->used || parent->pane >= 0 ||
        (parent->first != leaf && parent->second != leaf)) {
      continue;
    }
    int sibling = parent->first == leaf ? parent->second : parent->first;
    *parent = layout->nodes[sibling];
    layout->nodes[sibling].used = false;
    layout->nodes[leaf].used = false;
    return;
  }
}

static struct mux_rect minimum(const struct mux_layout *layout, int node)
{
  const struct mux_node *part = &layout->nodes[node];
  if (part->pane >= 0) {
    return (struct mux_rect){.width = MUX_MIN_COLUMNS, .height = MUX_MIN_ROWS + 1};
  }
  struct mux_rect a = minimum(layout, part->first), b = minimum(layout, part->second);
  if (part->axis == MUX_LEFT_RIGHT) {
    return (struct mux_rect){.width = a.width + b.width + 1,
        .height = a.height > b.height ? a.height : b.height};
  }
  return (struct mux_rect){.height = a.height + b.height + 1,
      .width = a.width > b.width ? a.width : b.width};
}

static void place_tree(const struct mux_layout *layout, int node, struct mux_rect rect,
    struct mux_rect rectangles[MUX_PANES])
{
  const struct mux_node *part = &layout->nodes[node];
  if (part->pane >= 0) {
    rectangles[part->pane] = rect;
    return;
  }
  struct mux_rect a = rect, b = rect;
  struct mux_rect a_min = minimum(layout, part->first), b_min = minimum(layout, part->second);
  bool horizontal = part->axis == MUX_LEFT_RIGHT;
  size_t available = (horizontal ? rect.width : rect.height) - 1;
  size_t first = available / 2;
  size_t required_a = horizontal ? a_min.width : a_min.height;
  size_t required_b = horizontal ? b_min.width : b_min.height;
  if (first < required_a) {
    first = required_a;
  }
  if (available - first < required_b) {
    first = available - required_b;
  }
  if (horizontal) {
    a.width = first;
    b.width = available - first;
    b.x += first + 1;
  } else {
    a.height = first;
    b.height = available - first;
    b.y += first + 1;
  }
  place_tree(layout, part->first, a, rectangles);
  place_tree(layout, part->second, b, rectangles);
}

static void leaves(const struct mux_layout *layout, int node, int panes[MUX_PANES], size_t *count)
{
  const struct mux_node *part = &layout->nodes[node];
  if (part->pane >= 0) {
    panes[(*count)++] = part->pane;
  } else {
    leaves(layout, part->first, panes, count);
    leaves(layout, part->second, panes, count);
  }
}

bool mux_layout_place(const struct mux_layout *layout, size_t columns, size_t rows,
    struct mux_rect rectangles[MUX_PANES])
{
  memset(rectangles, 0, sizeof(struct mux_rect) * MUX_PANES);
  if (!layout->nodes[0].used) {
    return true;
  }
  if (layout->kind == MUX_BSP) {
    struct mux_rect needed = minimum(layout, 0);
    if (columns < needed.width || rows < needed.height) {
      return false;
    }
    place_tree(layout, 0, (struct mux_rect){.width = columns, .height = rows}, rectangles);
    return true;
  }
  int panes[MUX_PANES];
  size_t count = 0;
  leaves(layout, 0, panes, &count);
  bool horizontal = layout->equal_axis == MUX_LEFT_RIGHT;
  size_t extent = horizontal ? columns : rows;
  size_t min_extent = horizontal ? MUX_MIN_COLUMNS : MUX_MIN_ROWS + 1;
  size_t other = horizontal ? rows : columns;
  if (extent < count * min_extent + count - 1 ||
      other < (horizontal ? MUX_MIN_ROWS + 1 : MUX_MIN_COLUMNS)) {
    return false;
  }
  size_t available = extent - count + 1, start = 0;
  for (size_t i = 0; i < count; ++i) {
    size_t length = available / count + (i < available % count);
    rectangles[panes[i]] = horizontal ?
        (struct mux_rect){.x = start, .width = length, .height = rows} :
        (struct mux_rect){.y = start, .width = columns, .height = length};
    start += length + 1;
  }
  return true;
}

int mux_layout_neighbor(const struct mux_rect rectangles[MUX_PANES], int focused,
    enum mux_axis axis, bool forward)
{
  const struct mux_rect *origin = &rectangles[focused];
  size_t x = origin->x * 2 + origin->width, y = origin->y * 2 + origin->height;
  size_t best = SIZE_MAX;
  int selected = focused;
  for (int i = 0; i < MUX_PANES; ++i) {
    const struct mux_rect *candidate = &rectangles[i];
    if (i == focused || !candidate->width) {
      continue;
    }
    size_t cx = candidate->x * 2 + candidate->width;
    size_t cy = candidate->y * 2 + candidate->height;
    size_t along = axis == MUX_LEFT_RIGHT ? cx : cy;
    size_t source = axis == MUX_LEFT_RIGHT ? x : y;
    if (forward ? along <= source : along >= source) {
      continue;
    }
    size_t across = axis == MUX_LEFT_RIGHT ? (cy > y ? cy - y : y - cy) :
        (cx > x ? cx - x : x - cx);
    size_t distance = (along > source ? along - source : source - along) + across * 2;
    if (distance < best) {
      best = distance;
      selected = i;
    }
  }
  return selected;
}
