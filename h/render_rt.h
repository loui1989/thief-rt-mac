#ifndef __RENDER_RT_H
#define __RENDER_RT_H

// Ray-traced rendering boundary (RTGL1 path tracer, macOS/Metal).
//
// The engine still draws through lgd3d exactly as for the OpenGL renderer.
// When the ray tracer is active the platform renderer additionally receives:
//
//  * the main view's camera, once per rendered scene;
//  * the complete world representation (every cell's polygons, in world
//    space), whenever a mission's geometry changes, plus the current
//    texture for each world texture id every frame;
//  * the static/animated/dynamic object lights, every frame;
//  * object, AI and effect polygons as the engine draws them, with world-space
//    positions recovered from r3d's camera transform.
//
// Everything else lgd3d draws (the HUD, inventory models, menus) is composited
// over the path-traced image as before.  All functions are no-ops unless
// RenderRTActive() returns nonzero.

#include <stdint.h>
#include <render_backend.h>

#ifdef __cplusplus
extern "C" {
#endif

enum eRenderRTWorldFlags {
  kRenderRTWorldSky = 1,          // sky surface: show the sky through it
  kRenderRTWorldWater = 2,        // medium boundary (water surface)
  kRenderRTWorldFullbright = 4,   // RENDER_DOESNT_LIGHT: self-lit
  kRenderRTWorldTransparent = 8   // texture has transparent texels
};

int RenderRTActive(void);

// Main view. Dark world space: z is up. fwd/left/up are the camera axes,
// zoom is r3d's (1.0 = 90 degrees horizontal), width/height the 3D canvas.
void RenderRTSceneBegin(const float pos[3], const float fwd[3],
                        const float left[3], const float up[3], float zoom,
                        int width, int height);
void RenderRTSceneEnd(void);

// While nonzero (the main view is being drawn), lgd3d hands polygons to
// RenderRTDrawWorld instead of rasterizing them.
int RenderRTCaptureDraws(void);
// Nonzero while vertices lgd3d builds belong in the ray-traced scene: the
// main view, outside suppressed regions. Sampled per vertex, because lgd3d
// batches polygons and submits them later.
extern int g_RenderRTCaptureVertices;
// The main view starts suppressed: world surfaces, sky and water come from
// the world representation. The portal renderer lifts it while drawing
// objects. Returns the previous setting, for restoring.
int RenderRTSuppressCapture(int suppress);
// xyz: count world-space positions matching the vertices. Texture, blend and
// alpha-test state are those last set through RenderBackend*.
void RenderRTDrawWorld(int primitive, const sRenderBackendVertex *vertices,
                       const float *xyz, int count);

// The object the following RenderRTDrawWorld triangles belong to. An object
// seen unchanged in consecutive frames is kept, and drawn while out of view
// (as long as RenderRTGameObjectPresent says it is still where it was), so
// lamps, furniture and doors keep casting shadows whatever the camera sees.
// flags: kRenderRTObjectWhole, the object is drawn in full, unclipped by the
// view (a second draw in the same frame, for another cell it reaches into,
// adds nothing); kRenderRTObjectNoShadow, it casts no shadow (a camera-facing
// bitmap: a glow, a light cone -- turning with the view, its shadow would too);
// kRenderRTObjectFirstPerson, the player's own arm, weapon or arrow, drawn
// before the camera: seen by it, but in no mirror, glass or water (his body
// double holds what he holds there).
enum { kRenderRTObjectWhole = 1, kRenderRTObjectNoShadow = 2, kRenderRTObjectFirstPerson = 4 };
void RenderRTObjectBegin(uint32_t obj, int flags);
void RenderRTObjectEnd(void);
// a model the game is about to draw, unchanged a while: kept as it was, the
// game skips drawing it (1)
int RenderRTObjectReuse(uint32_t obj);
// Nonzero if an object that was not drawn this frame should be drawn for the
// ray tracer: it is not kept from an earlier frame, or it has moved since.
int RenderRTObjectWanted(uint32_t obj);
// Implemented by the game: nonzero, with its position and facing, if the
// object still exists in the world (not destroyed, picked up or contained).
int RenderRTGameObjectPose(uint32_t obj, float pose[6]);
// Implemented by the game, called before RenderRTSceneEnd: draws the objects
// near the camera the view did not (RenderRTObjectWanted decides which).
void RenderRTGameDrawExtraObjects(void);
// Garrett's body double (plycbllm.cpp), drawn each frame for the ray
// tracer: unseen by the camera, but casting his shadow and seen in
// reflections. RenderRTPlayerBodyActive: whether there is to be one (the
// setting, and the ray tracer running); RenderRTPlayerBodyWanted, as it is
// drawn: nonzero to draw it.
int RenderRTPlayerBodyActive(void);
int RenderRTPlayerBodyWanted(uint32_t obj);
// what the double holds that its model has not (his blackjack), drawn as it is
int RenderRTPlayerBodyItemWanted(uint32_t obj);
// Implemented by the game: the object's name, for the inspector.
void RenderRTGameObjectName(uint32_t obj, char *buf, int size);
// Implemented by the game: a particle group's on/off (a flame doused): 1 on,
// 0 off, -1 not a particle group (or gone).
int RenderRTGameParticleLit(uint32_t obj);
// Implemented by the game: whether the object is a creature (its limbs move
// where the object itself stands still).
int RenderRTGameObjectIsCreature(uint32_t obj);
// The game's objects within radius (feet) of a point (Dark units), one line
// each, for the inspector.
void RenderRTGameDescribeNearby(const float at[3], float radius,
                                void (*line)(const char *text, void *ctx), void *ctx);

// Nonzero while the camera is under water.
void RenderRTCameraMedium(int underwater);
// the frame's fog: colour 0-255, distance (feet) at which it is opaque, 0 none
void RenderRTFog(const float rgb[3], float distFeet);
// the image (its grs_bitmap) the next backend texture is made from: its file
// and material file name its material maps (lgd3d's modern texture path)
void RenderRTTextureSource(const void *bitmap);

// World representation. Returns nonzero if geometry for this serial must be
// (re)submitted between RenderRTWorldBegin and RenderRTWorldEnd.
int RenderRTWorldWanted(uint32_t serial);
void RenderRTWorldBegin(uint32_t serial);
// the cell the following polygons are in (the ray tracer leaves far rooms out)
void RenderRTWorldCell(int cell);
void RenderRTWorldPolygon(int textureId, int flags, int count,
                          const float *xyz, const float *uv);
// the way the next polygon faces (its plane's normal: toward the room it is
// seen from), before RenderRTWorldPolygon
void RenderRTWorldPolygonFacing(float nx, float ny, float nz);
// the next polygon lies under water, this far (feet) under its surface: hazed
// by it, seen from above it (an untextured water surface is not drawn: the
// game draws none -- the water's haze over what is under it is all it shows)
void RenderRTWorldPolygonUnderWater(float depth);
// the next polygon: a water surface seen from under it (its cell is the water)
void RenderRTWorldPolygonUnderside(void);
// the mission's water colour (0-255 each), for that haze
void RenderRTWorldWaterColor(int r, int g, int b);
// The game's own light on the next polygon (its lightmap: the light the light
// gem goes by), before RenderRTWorldPolygon: where its lightmap lies in page
// `page` of the atlas, a u,v per corner (0..1 of the page). The ambient glow
// follows it (rtgl1.cpp). RenderRTLightmapWanted: whether to make the atlas
// (the path tracer running, and taking it). RenderRTLightmapRect: a lightmap's
// pixels (RGBA, the game's light 0..255), at x,y of a page -- at the world's
// making, and again as the game's animated lights change it.
int RenderRTLightmapWanted(void);
void RenderRTWorldPolygonLight(int page, int count, const float *lmuv);
void RenderRTLightmapRect(int page, int x, int y, int w, int h, const unsigned char *rgba);
#define RENDER_RT_LIGHTMAP_PAGE 1024
#define RENDER_RT_LIGHTMAP_PAGES 24
void RenderRTWorldEnd(void);
// Every frame: the backend texture (RenderBackendCreateTexture handle)
// currently showing for a world texture id (animated textures change).
void RenderRTWorldTexture(int textureId, void *texture);

// Lights, every frame between SceneBegin and SceneEnd. rgb is Dark's light
// brightness (0..~255 per channel), radius 0 means unlimited. For a spot
// light dir is set and inner/outer are cosines; inner < 0 for point lights.
// The player's light for the light gem and the guards, by the path tracer's
// lights and shadows: RenderRTDetectionWanted says whether to use it (the
// setting, and the ray tracer running); RenderRTPointLighting gives it at a
// point (Dark units) in the game's own scale (as compute_object_lighting),
// returning 0 if it cannot. RenderRTNoteLighting shows both on the counter.
int RenderRTDetectionWanted(void);
// Time the game spends on the ray tracer's own preparations (finding the
// lights, their walking distances, models out of view): on the counter.
void RenderRTPrepBegin(void);
// The frame rate setting: the fewest milliseconds a frame takes are 1000 /
// this (60, 100 or 120); 0 when not set.
int RenderRTFrameCap(void);
// The OpenGL renderer's, in play: the screen's refresh rate with v-sync on,
// 240 with it off; 0 out of play or with the ray tracer.
int RenderGLFrameCap(void);
// Nonzero once after the panel's "Skip to next mission (test)" was chosen:
// the game wins the mission (its "win_mission" command) on its next frame.
int RenderRTTakeSkipMission(void);
void RenderRTPrepEnd(void);
// Within the preparations: the time since the last mark (or their start) is
// put to this part of them (0 the world, 1 the lights found, 2 their reach,
// 3 the objects' lights, 4 models out of view), for the performance log.
void RenderRTPrepMark(int part);
int RenderRTPointLighting(const float pos[3], float *value);
void RenderRTNoteLighting(float original, float traced);

// The level's ambient light (0..1 per channel), once a scene.
void RenderRTAmbient(float r, float g, float b);
// a Thief 2 mission's sunlight (its render parameters): toward the sun, its
// colour (0..1) and its brightness on the game's scale; on 0: none
void RenderRTSun(int on, const float toSun[3], const float rgb[3], float brightness);

// reaches: the level's lighting has it shining on a cell near or in view.
void RenderRTLight(uint32_t id, const float pos[3], const float rgb[3],
                   float radius, const float dir[3], float inner, float outer,
                   int reaches, float walk);
// walk: metres from the camera walking through the openings, -1 beyond reach,
// -2 not known (the light is in no cell)

// The mission's enhanced sky (clouds, stars, moon) for the path tracer: the
// game draws it, one face a frame, into the six faces of a cube round the
// viewer (rendloop.c), and the path tracer keeps what each face drew.
// RenderRTSkyFaceWanted: the face to draw now (0-5), or -1; its canvas is
// RenderRTSkyFaceSize() square. corners: the face's view pyramid, world
// space (r3_get_view_pyr_vecs: upper left, upper right, lower right, lower
// left), so each screen point drawn is a direction again.
int RenderRTSkyFaceWanted(void);
int RenderRTSkyFaceSize(void);
void RenderRTSkyFaceBegin(int face, const float corners[12]);
void RenderRTSkyFaceEnd(void);
// While a face is drawn: the polygon the sky is drawing now (src/render/
// skypoly.h), its points as the sky made them (r3s_point, in r3's clipping
// space), before the face clipped them -- taken whole, in 3D, by the one
// face its middle is in (n 0: done). And which of the sky's parts is being
// drawn (skyrend.cpp's components, in their order), for the path tracer's
// sky to draw them in that order whichever face kept each polygon.
void RenderRTSkyPoly(int n, void **points);
void RenderRTSkyLayer(int layer);
// (The old star sky is drawn the same way: its stars.) The old textured sky,
// every frame while the mission has it (portal.c): a face of the background
// cube round the viewer (0 top, 1 bottom, 2-5 round), its corners (Dark
// units, round the origin), their texture coordinates (0..1 across the
// texture) and its texture (RenderBackendCreateTexture handle).
void RenderRTSkyBoxFace(int face, void *texture, const float xyz[12], const float uv[8]);

#ifdef __cplusplus
}
#endif

#endif
