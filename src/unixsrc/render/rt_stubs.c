// The ray tracer's entry points that the engine calls outside DARK_RT
// (rendloop.c, plycbllm.cpp): a build without the path tracer (Linux, or a
// Mac without RTGL1) has none of them.
#include <render_rt.h>

int RenderRTActive(void) { return 0; }
int RenderRTPlayerBodyActive(void) { return 0; }
int RenderRTSkyFaceWanted(void) { return -1; }
int RenderRTSkyFaceSize(void) { return 0; }
void RenderRTSkyFaceBegin(int face, const float corners[12]) { (void)face; (void)corners; }
void RenderRTSkyFaceEnd(void) {}
