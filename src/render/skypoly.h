// The enhanced sky's polygons, drawn so the path tracer's sky can take each
// one whole (render_rt.h RenderRTSkyPoly): the cube faces it is drawn into
// otherwise each kept their own clipped piece, and the pieces met with gaps
// (dotted lines along the cube's edges across the sky).
#pragma once

#ifdef DARK_RT
#include <render_rt.h>
#define SKY_DRAW_POLY(n, p) \
   do { RenderRTSkyPoly((n), (void **)(p)); r3_draw_poly((n), (p)); RenderRTSkyPoly(0, 0); } while (0)
#else
#define SKY_DRAW_POLY(n, p) r3_draw_poly((n), (p))
#endif
