// $Header: x:/prj/tech/libsrc/lgd3d/RCS/texture.h 1.6 1998/02/02 12:52:06 KEVIN Exp $

#ifndef __TEXTURE_H
#define __TEXTURE_H

#include <darklimit.h>

// Handles of the hardware texture manager: every resident texture -- terrain
// at each mip level, models, animation frames, lightmap pages -- takes one.
// At 1024 a large fan mission ran out, and the manager then released and
// reloaded textures every frame (models flickering, colours swapping under
// the path tracer, whose textures are named per load).  Only arrays of this
// many pointers depend on it.
#define LGD3D_MAX_TEXTURES 8192

#endif
