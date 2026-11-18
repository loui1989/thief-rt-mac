/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

// $Header: r:/t2repos/thief2/src/render/objlight.c,v 1.46 2000/02/24 23:42:46 mahk Exp $
// code for multiple light source lighting

#include <lg.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <r3d.h>
#include <mprintf.h>

#include <mlight.h>
#include <portal.h>
#include <wr.h>
#include <rendprop.h>
#include <osystype.h>
#include <objlight.h>
#include <objpos.h>
#include <elitprop.h>
#include <osysbase.h>
#include <iobjsys.h>
#include <objnotif.h>
#include <appagg.h>
#include <missrend.h>
#include <objshape.h>

// for new lighting calc:
#include <objcast.h>

ObjID highlit_obj=OBJ_NULL;

///////////////////////////////////////
// multiple light system/setup

float objlight_highlight_level=0.35f;

// XXX should only be used in debug
BOOL g_bAllObjsUnlit=FALSE;

int num_light, num_dyn;
mls_multi_light light_data[MAX_STATIC];
mls_multi_light light_this[32];
static BOOL dynamic_light_overflow_warned;

#define SUNLIGHT_INDEX 0
#define FIRST_LIGHT (SUNLIGHT_INDEX + 1)// reserve one index for sunlight

// how many feet away sunlight is considered to be for object lighting
#define SUNLIGHT_DISTANCE 125.0f

// Thief 2 reserves object-light slot zero for sunlight.  Thief 1/Gold has no
// such reservation: WR 0.23 may store an ordinary static light in slot zero
// and cells may reference it.  Only give slot zero the Thief 2 meaning when
// this mission actually enables sunlight.
static BOOL mission_uses_sunlight(void)
{
   return GetMissionRenderParams()->use_sun;
}


#ifdef DARK_RT
static void objlight_gl_reset(void);
#endif
void reset_object_lighting(void)
{
#ifdef DARK_RT
   objlight_gl_reset();
#endif
   num_light=FIRST_LIGHT;
   // Mission transitions can rebuild static lighting before the render
   // loop's first BeginFrame reset.  Do not carry dynamic object lights from
   // the menu or previous mission into that initial update.
   num_dyn=0;
   dynamic_light_overflow_warned = FALSE;
}

void reset_dynamic_lighting(void)
{
   reset_dynamic_lights();
   num_dyn = 0;
}

extern int hack_red, hack_green, hack_blue;
float light_scale=32.0;
#ifdef DARK_RT
// The OpenGL renderer's real-time lights: a moving light's brightness,
// colour (hack_red.., 0..256) and reach, as add_dynamic_light was given them
static struct { float bright, radius; int rgb[3]; } dyn_gl[MAX_DYNAMIC];
extern int g_RenderGLWorldPositions;
#endif
void add_dynamic_light(Location *loc, float bright, float clamp_radius)
{
#ifdef DARK_RT
   // (lit in real time by the OpenGL renderer instead of into the lightmaps)
   if (!g_RenderGLWorldPositions)
#endif
   portal_add_simple_dynamic_light(bright, 0, loc, clamp_radius);
   if (num_dyn < MAX_DYNAMIC) {
#ifdef DARK_RT
      dyn_gl[num_dyn].bright = bright;
      dyn_gl[num_dyn].radius = clamp_radius;
      dyn_gl[num_dyn].rgb[0] = hack_red;
      dyn_gl[num_dyn].rgb[1] = hack_green;
      dyn_gl[num_dyn].rgb[2] = hack_blue;
#endif
      light_data[num_light + num_dyn].loc = loc->vec;
#ifdef RGB_LIGHTING
      bright /= 255;
      light_data[num_light + num_dyn].bright.x = hack_red*bright/light_scale;
      light_data[num_light + num_dyn].bright.y = hack_green*bright/light_scale;
      light_data[num_light + num_dyn].bright.z = hack_blue*bright/light_scale;
#else
      light_data[num_light + num_dyn].bright = bright/light_scale;
#endif
      ++num_dyn;
   }
   else
   {
      // The portal lightmap has already received this light.  Only the
      // fixed-size object-light list is full, so keep rendering instead of
      // aborting the mission.  NewDark retains 32 object-light slots and
      // likewise treats additional simultaneous lights as a capped effect.
      if (!dynamic_light_overflow_warned)
      {
         Warning(("Object dynamic-light capacity (%d) exceeded; "
                  "additional lights affect terrain only\n", MAX_DYNAMIC));
         dynamic_light_overflow_warned = TRUE;
      }
   }
}

#ifdef RGB_LIGHTING
int add_object_light(mxs_vector *where, rgb_vector *bright, float radius)
#else
int add_object_light(mxs_vector *where, float bright, float radius)
#endif
{
   if (num_light < MAX_STATIC - MAX_DYNAMIC) {
      light_data[num_light].loc = *where;
#ifdef RGB_LIGHTING
      light_data[num_light].bright.x = bright->x/light_scale;
      light_data[num_light].bright.y = bright->y/light_scale;
      light_data[num_light].bright.z = bright->z/light_scale;
#else
      light_data[num_light].bright = bright/light_scale;
#endif
      light_data[num_light].inner = -1.0;
      light_data[num_light].radius = radius;
      return num_light++;
   }
   else
   {
      CriticalMsg2("Out of Lights (at %d allowed %d)",num_light,MAX_STATIC-MAX_DYNAMIC);
      return -1;
   }
}

#ifdef RGB_LIGHTING
int add_object_spotlight(mxs_vector *where, rgb_vector *bright, mxs_vector *dir, float inner, float outer, float radius)
#else
int add_object_spotlight(mxs_vector *where, float bright, mxs_vector *dir, float inner, float outer, float radius)
#endif
{
   if (num_light < MAX_STATIC - MAX_DYNAMIC) {
      light_data[num_light].loc = *where;
#ifdef RGB_LIGHTING
      light_data[num_light].bright.x = bright->x/light_scale;
      light_data[num_light].bright.y = bright->y/light_scale;
      light_data[num_light].bright.z = bright->z/light_scale;
#else
      light_data[num_light].bright = bright/light_scale;
#endif
      light_data[num_light].dir   = *dir;
      light_data[num_light].inner = inner;
      light_data[num_light].outer = outer;
      light_data[num_light].radius = radius;
      return num_light++;
   } else
      return -1;
}


#ifdef RGB_LIGHTING //zb
void set_object_light(int light_index, float bright, rgb_vector* p_color)
#else
void set_object_light(int light_index, float bright)
#endif
{
   if (light_index == -1)
      return;

#ifdef RGB_LIGHTING
   //hack the color

   light_data[light_index].bright.x = p_color->x/light_scale;
   light_data[light_index].bright.y = p_color->y/light_scale;
   light_data[light_index].bright.z = p_color->z/light_scale;
   /* gray:
   light_data[light_index].bright.x = bright/light_scale;
   light_data[light_index].bright.y = bright/light_scale;
   light_data[light_index].bright.z = bright/light_scale;
   */
#else
   light_data[light_index].bright = bright/light_scale;
#endif
}

BOOL is_object_light_defined(int handle)
{
   return handle<num_light;
}

/////////////////////////////////////////////////////////
//
//  cache raycasts from objects to lights
//

BOOL location_sees_light(Location *loc, int light_id)
{
   Location dummy;
   Location p;
   if (light_id == SUNLIGHT_INDEX && mission_uses_sunlight()) {
      return portal_loc_has_sunlight(loc);
   } else {
      MakeLocationFromVector(&p, &light_data[light_id].loc);

      if (PortalRaycast(loc, &p, &dummy, 0))
      {
         return ObjRaycastC(loc, &p, &dummy, TRUE,
                        0, 1, g_pObjShadowProp, FALSE) == kObjCastNone;
      }
      else
         return FALSE;
   }
}



struct
{
   int    cell_id;     // last place we saw this object
   ulong  shadows[3];  // bits of shadow casts; 0 = shadowed, 1 = visible
} shadow_cache[HACK_MAX_OBJ];

ulong *objGetShadows(ObjID obj)
{
   Location *loc= &ObjPosGet(obj)->loc;
   int cell = CellFromLoc(loc);
   Assert_(gMaxObjID <= HACK_MAX_OBJ);

   if (shadow_cache[obj].cell_id != cell)
   {
      int *shadPtr=&shadow_cache[obj].shadows[0];
      int i,k,n,inner_frac;
      ushort *lt = WR_CELL(cell)->light_indices;
      n = *lt++;
      for (i=0; i<n; i+=32, shadPtr++)
      {
         int shadows=0;
         if (i+32<=n)
            inner_frac=32;
         else
            inner_frac=n&0x1f;
         for (k=0; k<inner_frac; ++k)
            if (location_sees_light(loc, lt[i+k]))
               shadows |= (1 << k);
         *shadPtr=shadows;
      }
      shadow_cache[obj].cell_id = cell;
   }
   return shadow_cache[obj].shadows;
}

void objShadowUncache(ObjID obj)
{
   Assert_(obj < HACK_MAX_OBJ && obj >= 0);
   shadow_cache[obj].cell_id = -1;
}

void ObjShadowClearCache(void)
{
   int i;
   for (i=0; i < HACK_MAX_OBJ; ++i)
      objShadowUncache(i);
}

static void ObjListener(ObjID obj, eObjNotifyMsg msg, void* data)
{
   if (msg == kObjNotifyDelete && OBJ_IS_CONCRETE(obj))
      objShadowUncache(obj);
}
static sObjListenerDesc g_ObjListenerDesc =
{
   ObjListener, 0,
};

static void ObjPosListener(ObjID obj, const ObjPos* pos, void* data)
{
   objShadowUncache(obj);
}

static tObjListenerHandle g_ObjListenerHandle = 0;

// Reref all "dirty" objects & call callbacks
void ObjShadowInit(void)
{
   IObjectSystem *ios = AppGetObj(IObjectSystem);
   g_ObjListenerHandle = IObjectSystem_Listen(ios, &g_ObjListenerDesc);
   ObjPosListen(ObjPosListener, NULL);
   ObjShadowClearCache();
}

void ObjShadowTerm(void)
{
   IObjectSystem *ios = AppGetObj(IObjectSystem);
   IObjectSystem_Unlisten(ios, g_ObjListenerHandle);
}

/////////////////////////////////////////////////////////
//
//  evaluate lighting at location / on object

extern float portal_evaluate_spotlight(mxs_vector *loc, mxs_vector *light_loc, mxs_vector *light_dir, float inner, float outer);

// return FALSE to mean out of light space, else TRUE to mean keep going
static BOOL _apply_one_location_light(int i, Location *loc, int *n)
{
   if (light_data[i].radius &&
       mx_dist2_vec(&loc->vec, &light_data[i].loc) > light_data[i].radius*light_data[i].radius)
      return TRUE;
   if (light_data[i].inner != -1.0)
   {
      float scale = portal_evaluate_spotlight(&loc->vec,
                 &light_data[i].loc, &light_data[i].dir,
                 light_data[i].inner, light_data[i].outer);
      if (scale != 0)
      {
         light_this[*n] = light_data[i];
#ifdef RGB_LIGHTING
         mx_scaleeq_vec(&light_this[*n].bright, scale);
#else
         light_this[*n].bright *= scale;
#endif
         (*n)++;   // @TODO: see if this is the best way to do the N inc
      }
   }
   else
      light_this[(*n)++] = light_data[i];
   return (*n)<32;
}


static void setup_location_lighting(Location *loc, mxs_vector *newlight, int *shadPtr,float radius)
{
   int i, n, num, k, inner_frac;
   int cell = CellFromLoc(loc);
   ushort *lt;
   int shadows;

   if(cell<0||cell>=wr_num_cells)
   {
      Warning(("Invalid location for lighting: %g,%g,%g\n",loc->vec.x,loc->vec.y,loc->vec.z));
      return;
   }
   if (cell==CELL_INVALID) return;

   lt = WR_CELL(cell)->light_indices;

   num = num_light + num_dyn;

   // Put sunlight next to the object.  Slot zero is an ordinary static light
   // in Thief 1/Gold missions, so it must remain untouched when sunlight is
   // disabled.
   if (mission_uses_sunlight())
      mx_scale_add_vec(&light_data[SUNLIGHT_INDEX].loc, &loc->vec,
                       &portal_sunlight_norm, SUNLIGHT_DISTANCE + radius);

   // first iterate over non-dynamic lights
   for (n=0, i=0; i <= lt[0]; i += 32, shadPtr++)
   {
      if (i+32<=lt[0])
         inner_frac=32;
      else
         inner_frac=lt[0]&0x1f;
      for (k=1, shadows=*shadPtr; k <= inner_frac; ++k, shadows >>= 1)
         if (shadows & 1)
            if (!_apply_one_location_light(lt[i+k],loc,&n))
               goto got_lights;
   }

   for (i=num_light; i < num; ++i)
   {
      Location p, hit;
      MakeLocationFromVector(&p, &light_data[i].loc);
      if (PortalRaycast(loc, &p, &hit, 0))
      {
         light_this[n++] = light_data[i];
         if (n == 32) break;
      }
   }

   // @QUESTION: hmmmm. why is this always 2.0?
got_lights:
   if (n==32)
      Warning(("Hey, 32 lights at %g %g %g\n",loc->vec.x,loc->vec.y,loc->vec.z));
   ml_multi_set_lights_for_object(n, light_this, &loc->vec, radius); // obj radius
}

void setup_object_lighting(ObjID o, mxs_vector *newlight, float bright)
{
   sExtraLightProp *extra_light;
   const sMissionRenderParams *parms;
   Location *loc= &ObjPosGet(o)->loc;
#ifdef DBG_ON
   mld_multi_unlit=(ObjRenderType(o)==kRenderUnlit)||g_bAllObjsUnlit;
#else
   mld_multi_unlit=(ObjRenderType(o)==kRenderUnlit);
#endif
   mld_multi_hilight=(o==highlit_obj)?objlight_highlight_level:0;

   mld_multi_ambient_only = FALSE;

   // get info for level
   parms = GetMissionRenderParams();
   mx_copy_vec(&mld_multi_rgb_ambient, &parms->ambient_light);
   if (parms->use_sun)
      mx_copy_vec(&light_data[SUNLIGHT_INDEX].bright, &parms->sun_scaled_rgb);

   if (ObjExtraLightGet(o, &extra_light)) {
#ifdef RGB_LIGHTING
      int i;
      for (i = 0; i < 3; ++i) {
         mld_multi_rgb_ambient.el[i] += extra_light->m_fValue;
         if ((mld_multi_rgb_ambient.el[i] + mld_multi_hilight) > 1.0)
            mld_multi_rgb_ambient.el[i] = 1.0f - mld_multi_hilight;
         if (!extra_light->m_bAdditive)
            mld_multi_ambient_only = TRUE;
      }
#else
      mld_multi_ambient += extra_light->m_fValue;
      if ((mld_multi_ambient + mld_multi_hilight) > 1.0)
         mld_multi_ambient = 1.0 - mld_multi_hilight;
      if (!extra_light->m_bAdditive)
         mld_multi_ambient_only = TRUE;
#endif

#ifndef SHIP
      if (extra_light->m_fValue < -1.0 || extra_light->m_fValue > 1.0)
         mprintf("Extra light on object %d not in range -1 to 1: %g\n",
                 o, extra_light->m_fValue);
#endif // ~SHIP
   }

#ifdef DARK_RT
   // An effect's model glowing (a crystal, an elemental's core, a fireball),
   // as the path tracer has it: with the OpenGL renderer's real-time lights,
   // its light lifted by its glow (else a water crystal was faint)
   if (g_RenderGLWorldPositions) {
      extern float RenderGLObjectGlow(unsigned int obj);
      const float glow = RenderGLObjectGlow((unsigned int)o);
      if (glow > 0.0f) {
#ifdef RGB_LIGHTING
         int i;
         for (i = 0; i < 3; ++i)
            mld_multi_rgb_ambient.el[i] = __min(mld_multi_rgb_ambient.el[i] + 0.45f * glow, 1.0f - mld_multi_hilight);
#else
         mld_multi_ambient = __min(mld_multi_ambient + 0.45f * glow, 1.0f - mld_multi_hilight);
#endif
      }
   }
#endif
   if (!mld_multi_unlit && CellFromLoc(loc) != CELL_INVALID)
      setup_location_lighting(loc,newlight,objGetShadows(o),ObjGetRadius(o));
}

// apply light index i to an obj at location loc
static float _apply_one_light(int i, Location *loc)
{
   float scale = 1.0;

   if (i == SUNLIGHT_INDEX && mission_uses_sunlight()) {
      mx_scale_add_vec(&light_data[SUNLIGHT_INDEX].loc, &loc->vec,
                       &portal_sunlight_norm, SUNLIGHT_DISTANCE);
      return ml_multi_light_on_object(&light_data[i], &loc->vec);
   }

   if (light_data[i].radius &&
       mx_dist2_vec(&loc->vec, &light_data[i].loc) > light_data[i].radius*light_data[i].radius)
      return 0.0;
   if (light_data[i].inner != -1.0)
      scale = portal_evaluate_spotlight(&loc->vec,
                   &light_data[i].loc,  &light_data[i].dir,
                    light_data[i].inner, light_data[i].outer);
   return scale*ml_multi_light_on_object(&light_data[i], &loc->vec);
}

//
// wsf: new for this function: check for objects, too. We do this for a limited # of objects, however.
//
//
float compute_object_lighting(ObjID o)
{
   Location dummy;
   Position* pos = ObjPosGet(o);
   Location *loc = &pos->loc;
   int cell = CellFromLoc(loc);
   int shadows, *shadPtr;
   int i, num, k, inner_frac; // yea, lets loop factor ourselves
   ushort *lt;
   float total=0;

   if (cell==CELL_INVALID) return 0.00;

   lt = WR_CELL(cell)->light_indices;
   num = num_light + num_dyn;

   // first iterate over non-dynamic lights
   shadPtr = objGetShadows(o);
   for (i=0; i <= lt[0]; i += 32, shadPtr++)
   {
      if (i+32<=lt[0])
         inner_frac=32;
      else
         inner_frac=lt[0]&0x1f;
      for (k=1, shadows=*shadPtr; k <= inner_frac; ++k, shadows >>= 1)
         if (shadows & 1)
            total+=_apply_one_light(lt[i+k],loc);
   }

   // now the dynamic ones
   for (i=num_light; i < num; ++i)
   {
      Location p;
      MakeLocationFromVector(&p, &light_data[i].loc);

      if (PortalRaycast(loc, &p, &dummy, 0))
      {
         if (ObjRaycastC(loc, &p, &dummy, TRUE,
                        0, 0, g_pObjShadowProp, FALSE) == kObjCastNone)
            total += ml_multi_light_on_object(&light_data[i], &loc->vec);
      }
   }

   return total;
}

// Guards seeing the player's shadow (a setting of the port, off unless
// chosen: shadow_alert, in either renderer's panel).
int g_PlayerShadowAlert = 0;

// Where an object's shadow falls: the light that lights it most (the
// game's own lights, as compute_object_lighting takes them -- the lights of
// its cell that reach it, the moving ones by a raycast), straight on from
// it away from that light to the first wall or floor (within 20 ft). Its
// strength is that light on the object (compute_object_lighting's scale).
// FALSE when nothing lights it or its shadow falls on nothing near.
BOOL compute_object_shadow_spot(ObjID o, mxs_vector *spot, float *strength)
{
   Location dummy;
   Position* pos = ObjPosGet(o);
   Location *loc;
   int cell, shadows, *shadPtr;
   int i, num, k, inner_frac;
   ushort *lt;
   float best = 0.0f;
   mxs_vector from;

   if (!pos)
      return FALSE;
   loc = &pos->loc;
   cell = CellFromLoc(loc);
   if (cell == CELL_INVALID)
      return FALSE;
   lt = WR_CELL(cell)->light_indices;
   num = num_light + num_dyn;
   shadPtr = objGetShadows(o);
   for (i = 0; i <= lt[0]; i += 32, shadPtr++)
   {
      if (i + 32 <= lt[0])
         inner_frac = 32;
      else
         inner_frac = lt[0] & 0x1f;
      for (k = 1, shadows = *shadPtr; k <= inner_frac; ++k, shadows >>= 1)
         if (shadows & 1)
         {
            const float v = _apply_one_light(lt[i + k], loc);
            if (v > best)
            {
               best = v;
               from = light_data[lt[i + k]].loc;
            }
         }
   }
   for (i = num_light; i < num; ++i)
   {
      Location p;
      MakeLocationFromVector(&p, &light_data[i].loc);
      if (PortalRaycast(loc, &p, &dummy, 0) &&
          ObjRaycastC(loc, &p, &dummy, TRUE, 0, 0, g_pObjShadowProp, FALSE) == kObjCastNone)
      {
         const float v = ml_multi_light_on_object(&light_data[i], &loc->vec);
         if (v > best)
         {
            best = v;
            from = light_data[i].loc;
         }
      }
   }
   if (best <= 0.02f)
      return FALSE;
   {
      mxs_vector dir, end;
      Location a, b, hit;
      float len;
      mx_sub_vec(&dir, &loc->vec, &from);
      len = mx_mag_vec(&dir);
      if (len < 0.01f)
         return FALSE;
      mx_scaleeq_vec(&dir, 1.0f / len);
      mx_scale_add_vec(&end, &loc->vec, &dir, 20.0f);
      MakeLocationFromVector(&a, &loc->vec);
      MakeLocationFromVector(&b, &end);
      if (PortalRaycast(&a, &b, &hit, 0))
         return FALSE;   // (on nothing near)
      mx_scale_add_vec(spot, &hit.vec, &dir, -0.5f);
   }
   *strength = best;
   return TRUE;
}


#ifndef SHIP
#include <objedit.h>
#include <stdlib.h>  // qsort, min?
#include <memall.h>
#include <dbmem.h>   // must be last header! 

typedef struct {
   float       val;
   ObjID       obj; // if we can get it
   int         l_idx;
   mxs_vector *vec;
} sObjLightSort;

#define MAX_LIGHTS_PER 96
static sObjLightSort seen_lights[MAX_LIGHTS_PER];
static int seen_index;

static float _add_seen(float val, ObjID obj, int idx, mxs_vector *loc)
{
   if (val>0.0)
   {
      seen_lights[seen_index].val = val;
      seen_lights[seen_index].obj = obj;
      seen_lights[seen_index].l_idx = idx;
      seen_lights[seen_index].vec = loc;
      if (++seen_index>=MAX_LIGHTS_PER)
      {
         Warning(("Hey! out of static light space to think about\n"));
         --seen_index;
      }
   }
   return val;
}

static int compare_seen(const void *p, const void *q)
{
   sObjLightSort *a = (sObjLightSort *)p;
   sObjLightSort *b = (sObjLightSort *)q;

   if (a->val>b->val)
      return -1;
   return a->val<b->val;
}

static void _show_seen(int how_many)
{
   int i;
   qsort(seen_lights,seen_index,sizeof(sObjLightSort),compare_seen);
   for (i=0; i<__min(how_many,seen_index); i++)
   {
      sObjLightSort *s=&seen_lights[i];
      char buf[256];
      sprintf(buf,"Contributor %2.2d: val %f idx %d ",i, s->val, s->l_idx);
      if (s->obj)
         sprintf(buf+strlen(buf),"is obj %s\n",ObjEditName(seen_lights[i].obj));
      else
         sprintf(buf+strlen(buf),"is at %f %f %f\n",s->vec->x,s->vec->y,s->vec->z);
      mprintf(buf);
   }
}

float blame_object_lighting(ObjID o)
{
   Position* pos = ObjPosGet(o);
   Location *loc = &pos->loc;
   int cell = CellFromLoc(loc);
   int shadows, *shadPtr;
   int i, num, k, inner_frac;
   ushort *lt;
   float total=0;

   if (cell==CELL_INVALID) return 0.00;

   seen_index=0;

   lt = WR_CELL(cell)->light_indices;
   num = num_light + num_dyn;

   // first iterate over non-dynamic lights
   shadPtr = objGetShadows(o);

   for (i=0; i <= lt[0]; i += 32, shadPtr++)
   {
      if (i+32<=lt[0])
         inner_frac=32;
      else
         inner_frac=lt[0]&0x1f;
      for (k=1, shadows=*shadPtr; k <= inner_frac; ++k, shadows >>= 1)
         if (shadows & 1)
         {
            int j=lt[i+k];
            total+=_add_seen(_apply_one_light(j,loc),OBJ_NULL,j,&light_data[j].loc);
         }
   }

   // now the dynamic ones
   for (i=num_light; i < num; ++i)
   {
      Location p, hit;
      MakeLocationFromVector(&p, &light_data[i].loc);
      if (PortalRaycast(loc, &p, &hit, 0))
         total += _add_seen(ml_multi_light_on_object(&light_data[i], &loc->vec),
                            OBJ_NULL, i, &light_data[i].loc);
   }

   _show_seen(8);

   if (total - compute_object_lighting(o) > 0.05)
      mprintf("Light values dont match, BUG! (compute got %g)\n",compute_object_lighting(o));
   mprintf("Got lighting %g from %d vis lights, %d total lights\n",100.0*total,seen_index,lt[0]);

   return total;
}
#endif


#ifdef DARK_RT
#include <render_rt.h>
extern int portal_rt_light_relevant(int light);
extern float portal_rt_path_distance(int cell, const mxs_vector *pos);
// Hand the object-light table to the ray tracer: every static and animated
// light (their current brightness, as the game set it this frame) and this
// frame's dynamic lights.  Brightness goes back to Dark's 0..255 scale.
void objlight_rt_submit(void)
{
   int i, first = mission_uses_sunlight() ? FIRST_LIGHT : 0;
   {
      // the level's ambient light, which the game adds to every lightmap and
      // model: a room whose lights are all out is dim, not black
      const sMissionRenderParams *parms = GetMissionRenderParams();
      if (parms)
         RenderRTAmbient(parms->ambient_light.x, parms->ambient_light.y,
                         parms->ambient_light.z);
      // and a Thief 2 mission's sun, which its lightmaps have outdoors
      if (parms && parms->use_sun) {
         const float to[3] = { -parms->sunlight_vector.x, -parms->sunlight_vector.y,
                               -parms->sunlight_vector.z };
         const float rgb[3] = { parms->sun_rgb.x, parms->sun_rgb.y, parms->sun_rgb.z };
         RenderRTSun(1, to, rgb, parms->sun_b);
      } else
         RenderRTSun(0, NULL, NULL, 0.0f);
   }
   for (i = first; i < num_light + num_dyn && i < MAX_STATIC; ++i)
   {
      const mls_multi_light *l = &light_data[i];
      float pos[3], rgb[3], dir[3];
#ifdef RGB_LIGHTING
      rgb[0] = l->bright.x * light_scale;
      rgb[1] = l->bright.y * light_scale;
      rgb[2] = l->bright.z * light_scale;
#else
      rgb[0] = rgb[1] = rgb[2] = l->bright * light_scale;
#endif
      // (an unlit one too: the ray tracer leaves it out, its inspector lists it)
      pos[0] = l->loc.x; pos[1] = l->loc.y; pos[2] = l->loc.z;
      dir[0] = l->dir.x; dir[1] = l->dir.y; dir[2] = l->dir.z;
      {
         // the light's cell, found again only when it moves
         static int cell_of[MAX_STATIC];
         static mxs_vector cell_at[MAX_STATIC];
         static unsigned char known[MAX_STATIC];
         float walk;
         if (!known[i] || cell_at[i].x != l->loc.x || cell_at[i].y != l->loc.y ||
             cell_at[i].z != l->loc.z) {
            Location loc;
            MakeLocationFromVector(&loc, &l->loc);
            cell_of[i] = CellFromLoc(&loc);
            cell_at[i] = l->loc;
            known[i] = 1;
         }
         // (a light in no cell, inside a wall: -2, the ray tracer measures it straight)
         walk = cell_of[i] < 0 ? -2.0f : portal_rt_path_distance(cell_of[i], &l->loc);
         RenderRTLight(i >= num_light ? 0x10000u + (uint32_t)(i - num_light) : (uint32_t)i,
                       pos, rgb, l->radius, dir, l->inner, l->outer,
                       i >= num_light || portal_rt_light_relevant(i),
                       walk == -2.0f ? -2.0f : walk < 0.0f ? -1.0f : walk * 0.3048f);
      }
   }
}
#endif

#ifdef DARK_RT
/* ----- the OpenGL renderer's real-time lights ----------------------------

   Like NewDark's hardware renderer: the animated lights (lamps that flicker
   or are switched on and off) and the moving ones (a torch carried, a fire
   arrow) light the world per pixel, with shadows, their own light taken out
   of the lightmaps (portal_light_realtime: the lightmaps they reach built
   again without it). The nearest few animated lights only; the others stay
   in the lightmaps as they always were.
*/
#include <render_backend.h>
extern unsigned char portal_light_realtime[2048];
extern uchar *portal_anim_light_intensity;
extern int ObjAnimLightTable(int *index, float *max_bright, float *radius,
                             float *inner_radius, int *owner, int max);
extern void ObjAnimLightRefresh(int light_index);

#define GL_ANIM_MAX 2048
static int gl_table_n = -1;           // the animated lights (-1: not known yet)
static int gl_table_index[GL_ANIM_MAX];
static float gl_table_max[GL_ANIM_MAX], gl_table_radius[GL_ANIM_MAX], gl_table_inner[GL_ANIM_MAX];
static int gl_table_owner[GL_ANIM_MAX];
static unsigned char gl_table_fire[GL_ANIM_MAX];
extern void RenderRTGameObjectKind(unsigned int obj, char *arch, int an, char *model, int mn);

// a flame's light, by its object's archetype or model (a torch, a candle, a
// brazier, a lantern, a fireplace)
static int gl_light_is_fire(int owner)
{
   static const char *words[] = { "torch", "fire", "flame", "candle", "brazier", "lantern",
                                  "hearth", "campf", NULL };
   char arch[128], model[64], both[200];
   int i;
   if (owner <= 0)
      return 0;
   RenderRTGameObjectKind((unsigned int)owner, arch, sizeof(arch), model, sizeof(model));
   snprintf(both, sizeof(both), "%s %s", arch, model);
   for (i = 0; both[i]; ++i)
      both[i] = (char)tolower((unsigned char)both[i]);
   if (strstr(both, "elec"))
      return 0;
   for (i = 0; words[i]; ++i)
      if (strstr(both, words[i]))
         return 1;
   return 0;
}
static int gl_table_lights = -1;      // num_light when it was made

static void objlight_gl_reset(void)
{
   // a new mission: its lightmaps are built afresh
   memset(portal_light_realtime, 0, sizeof(portal_light_realtime));
   gl_table_n = -1;
   gl_table_lights = -1;
}

extern void portal_gl_verify_light(int li, const float pos[3], float B, float R, float inner);
static void objlight_gl_set(int i, int on)
{
   if (i < 0 || i >= 2048 || !portal_light_realtime[i] == !on)
      return;
   if (on && getenv("DARK_GL_LIGHTS_VERIFY")) {
      int k;
      for (k = 0; k < gl_table_n; ++k)
         if (gl_table_index[k] == i) {
            const float p[3] = { light_data[i].loc.x, light_data[i].loc.y, light_data[i].loc.z };
            portal_gl_verify_light(i, p, 8.0f * gl_table_max[k], gl_table_radius[k], gl_table_inner[k]);
         }
   }
   portal_light_realtime[i] = on ? 1 : 0;
   ObjAnimLightRefresh(i);
}

typedef struct { int k; float score; } gl_pick;
static int gl_pick_cmp(const void *a, const void *b)
{
   float d = ((const gl_pick *)a)->score - ((const gl_pick *)b)->score;
   return d < 0 ? -1 : d > 0 ? 1 : 0;
}

void objlight_gl_submit(const mxs_vector *cam)
{
   static gl_pick picks[GL_ANIM_MAX];
   static sRenderBackendLight out[64];
   int want = RenderBackendDynamicLightsWanted();
   int k, n = 0, count = 0;
   if (want <= 0) {
      float c[3] = { cam->x, cam->y, cam->z };
      for (k = 0; k < 2048; ++k)
         objlight_gl_set(k, 0);
      RenderBackendSetDynamicLights(NULL, 0, c);
      return;
   }
   if (want > 32)
      want = 32;
   if (gl_table_n < 0 || gl_table_lights != num_light) {
      gl_table_n = ObjAnimLightTable(gl_table_index, gl_table_max, gl_table_radius,
                                     gl_table_inner, gl_table_owner, GL_ANIM_MAX);
      gl_table_lights = num_light;
      for (k = 0; k < gl_table_n; ++k)
         gl_table_fire[k] = (unsigned char)gl_light_is_fire(gl_table_owner[k]);
   }
   // the nearest animated lights (by how far their reach is from the
   // camera), those already real-time kept a little longer
   for (k = 0; k < gl_table_n; ++k) {
      int i = gl_table_index[k];
      const mls_multi_light *l;
      float dx, dy, dz, reach;
      if (i < 0 || i >= num_light || i >= 2048)
         continue;
      l = &light_data[i];
      dx = l->loc.x - cam->x; dy = l->loc.y - cam->y; dz = l->loc.z - cam->z;
      reach = gl_table_radius[k] > 0.0f ? gl_table_radius[k] : 40.0f;
      picks[n].k = k;
      picks[n].score = (float)sqrt(dx * dx + dy * dy + dz * dz) - reach -
                       (portal_light_realtime[i] ? 15.0f : 0.0f);
      ++n;
   }
   qsort(picks, n, sizeof(picks[0]), gl_pick_cmp);
   for (k = 0; k < n; ++k) {
      int j = picks[k].k, i = gl_table_index[j];
      int on = count < want && picks[k].score < 30.0f;
      objlight_gl_set(i, on);
      if (on) {
         const mls_multi_light *l = &light_data[i];
         sRenderBackendLight *o = &out[count++];
         float m = gl_table_max[j];
         o->pos[0] = l->loc.x; o->pos[1] = l->loc.y; o->pos[2] = l->loc.z;
#ifdef RGB_LIGHTING
         o->color[0] = l->bright.x * light_scale / m;
         o->color[1] = l->bright.y * light_scale / m;
         o->color[2] = l->bright.z * light_scale / m;
#else
         o->color[0] = o->color[1] = o->color[2] = l->bright * light_scale / m;
#endif
         o->bright = 8.0f * m;
         o->radius = gl_table_radius[j];
         o->inner_radius = gl_table_inner[j];
         o->dir[0] = l->dir.x; o->dir[1] = l->dir.y; o->dir[2] = l->dir.z;
         o->spot_inner = l->inner;
         o->spot_outer = l->outer;
         o->kind = 0;
         o->id = i;
         o->moving = 0;
         o->owner = (uint32_t)gl_table_owner[j];
         o->flicker = gl_table_fire[j];
         o->size = 0.0f;
      }
   }
   // the moving lights (their own light already left out of the lightmaps)
   for (k = 0; k < num_dyn && k < MAX_DYNAMIC && count < 64; ++k) {
      const mls_multi_light *l = &light_data[num_light + k];
      sRenderBackendLight *o = &out[count++];
      o->pos[0] = l->loc.x; o->pos[1] = l->loc.y; o->pos[2] = l->loc.z;
      o->color[0] = dyn_gl[k].rgb[0] / 256.0f;
      o->color[1] = dyn_gl[k].rgb[1] / 256.0f;
      o->color[2] = dyn_gl[k].rgb[2] / 256.0f;
      o->bright = 8.0f * dyn_gl[k].bright;
      o->radius = dyn_gl[k].radius;
      o->inner_radius = 0.0f;
      o->dir[0] = o->dir[1] = 0.0f; o->dir[2] = -1.0f;
      o->spot_inner = -2.0f;
      o->spot_outer = -2.0f;
      o->kind = 1;
      o->id = 100000 + k;
      o->moving = 1;
      o->owner = 0;
      o->flicker = 0;
      o->size = 0.25f;
   }
   {
      float c[3] = { cam->x, cam->y, cam->z };
      RenderBackendSetDynamicLights(out, count, c);
   }
}
#endif
