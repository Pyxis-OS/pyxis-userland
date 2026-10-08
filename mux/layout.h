#ifndef MUX_LAYOUT_H
#define MUX_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

#define MUX_PANES 8
#define MUX_NODES (MUX_PANES * 2 - 1)
#define MUX_MIN_COLUMNS 12
#define MUX_MIN_ROWS 4

enum mux_axis { MUX_LEFT_RIGHT, MUX_TOP_BOTTOM };
enum mux_layout_kind { MUX_BSP, MUX_EQUAL };

struct mux_rect { size_t x, y, width, height; };
struct mux_node {
  bool used;
  int pane; /* A leaf's pane slot, or -1 for a split. */
  int first, second;
  enum mux_axis axis;
};
struct mux_layout {
  struct mux_node nodes[MUX_NODES];
  enum mux_layout_kind kind;
  enum mux_axis equal_axis;
};

void mux_layout_init(struct mux_layout *layout, int pane);
bool mux_layout_split(struct mux_layout *layout, int focused, int pane, enum mux_axis axis);
void mux_layout_remove(struct mux_layout *layout, int pane);
/* False leaves the result unspecified: the caller presents only focused pane.
 * Rectangles include a one-row pane heading, but exclude split dividers. */
bool mux_layout_place(const struct mux_layout *layout, size_t columns, size_t rows,
    struct mux_rect rectangles[MUX_PANES]);
int mux_layout_neighbor(const struct mux_rect rectangles[MUX_PANES], int focused,
    enum mux_axis axis, bool forward);

#endif
