# stb_truetype

`stb_truetype.h` version 1.26, from nothings/stb commit
`2c980bb59875b0d32144a71867fbdebb2f77cd20`.

Upstream archive:
<https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz>

The archive was obtained through the owner's download mirror, with SHA-256:
`9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515`.
`LICENSE` is copied unchanged. Pyxis uses its MIT licence option (Alternative A).

## Local adaptations

The header retains upstream formatting. Local changes are limited to:

- Check CFF shape allocation before writing vertices. The native scratch
  adapter sweeps retained allocations after a failed output pass.
- Return when the version 2 rasterizer's heap scanline allocation fails.
- Remove the selected version 2 active-edge allocation assertion; retain its
  existing null return. Version 1 remains unmodified at that site.
- Return an unsupported result for composite matching-point placement through
  the private `STBTT_unsupported` hook, freeing accumulated vertices.
- Check composite vertex-count addition before signed overflow. Saturate CFF
  vertex and flattened-point counters at the signed integer limit, rejecting
  that count before allocation through the private `STBTT_limit` hook. Point
  insertion now owns its count increment so subdivision cannot wrap it.

## Adapter boundary

Only `libfont/stb_adapter.c` compiles the implementation, with private static
symbols and rasterizer version 2. It calls font initialization, cmap lookup, vertical/horizontal metrics,
kerning, glyph bounds, shape extraction and direct rasterization; it does not
use stb's bitmap-allocation, atlas, packing or signed-distance-field interfaces.

Initialization and metrics/bounds lookup do not allocate. Shape extraction
allocates TT vertices and composite append buffers, or CFF vertices. Rasterization
allocates contour lengths, flattened points, sorted edges, wide scanlines and
active-edge heap chunks. The adapter hooks every allocation/free, charging live
bytes including its aligned allocation header against the caller's scratch
budget. An exceeded budget or arithmetic limit reports `FONT_LIMIT`; a failed
allocator reports `FONT_NO_MEMORY`. The first failure is sticky for the call;
partial/blank output is discarded by the caller. The adapter also sweeps any
remaining tracked allocations before returning. The retained font info never
keeps a pointer to that per-call allocation context.

The wrapper recognizes outer SFNT/TTC records and supported cmap formats on a
best-effort basis. It does not make stb safe for hostile input: table-internal
offsets, recursive composite structures and CFF decoding retain upstream's
trusted-font assumptions, including upstream assertions on malformed data.
Allocation budgets bound heap scratch, not parser execution time or stack use.
