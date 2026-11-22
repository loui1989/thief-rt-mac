/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

//  $Header: r:/t2repos/thief2/src/portal/portal.c,v 1.140 2000/02/24 23:41:41 mahk Exp $
//
//  PORTAL
//
//  dynamic portal/cell-based renderer

#ifndef SHIP
#include <stdio.h>
#endif

#include <string.h>
#include <math.h>

#include <lg.h>
#include <dev2d.h>
#include <r3d.h>
#include <g2pt.h>
#include <mprintf.h>
#include <config.h>
#include <timer.h>
#include <lgd3d.h>
#include <uigame.h>

#include <wrbsp.h>
#include <portal_.h>
#include <portdraw.h>
#include <portclip.h>
#include <portsky.h>
#include <pt_clut.h>
#include <animlit.h>
#include <portwatr.h>
#include <bspsphr.h>
#include <osysbase.h>
#include <wrlimit.h>
#include <darklimit.h>

#include <refsys.h>

#include <profile.h>

#include <texmem.h>

#include <wrdbrend.h> // to get access to cell texutre
#include <memall.h>
extern int ResImageMaterialTile(const void *bitmap, float *su, float *sv);
extern int ResImageTerrainScale(const void *bitmap, int *w, int *h);
#include <fogzone.h>
#include <dbmem.h>   // must be last header! 

#ifndef SHIP
// write out a log of our portal traversal
static FILE *trav_file;
#endif

  // enable this to collect per-scene statistics
#define STATS_ON

  // do some internal checking
//#define VALIDATE

//#define VALIDATE_UNIQUENESS_OF_PORTALS

#if defined(DBG_ON) || defined(PROFILE_ON)
  #define STATIC
#else
  #define STATIC static
#endif

  // max object fragments visible onscreen at once
// In hardware mode an object is rendered once, but the portal walk still
// allocates one visibility record for every visible cell reference to it.
// A wide editor viewport can therefore exhaust the original 4096 records
// even though there are far fewer than 4096 distinct objects in the mission.
#define MAX_VISIBLE_OBJECTS DARK_LIMIT_RUNTIME_VISIBLE_OBJECTS

  // does lighting compute light maps or at vertices?
  // We probably don't really support vertex lighting anymore
#define LIGHT_MAP

#ifdef DBG_ON
  bool debug_cell_traversal;
  #define CELL_DEBUG(x)  if (debug_cell_traversal) x; else
#else
  #define CELL_DEBUG(x)
#endif


/////////////////////    globals    /////////////////////////

  // default callback for turning texture id into texture
extern grs_bitmap *make_texture_map(int d);
r3s_texture (*portal_get_texture)(int d) = make_texture_map;
ulong portal_fog_color[3];
float portal_fog_dist = 0;
// this frame's fog: the mission's, or the fog zone the camera is in (fogzone.h)
static float portal_frame_fog_rgb[3];
static float portal_frame_fog_dist;
BOOL portal_fog_on = TRUE;
ulong fog_r3_color;
float fog_dist_modifier = .05f;
extern mxs_vector portal_sunlight;


  // default callbacks
static void render_object(ObjID obj, uchar *clut, ulong fragment)
{
}

static void dummy_sfx(int cell)
{
}

static BOOL object_visible(ObjID o1)
{
   return FALSE;
}

static BOOL object_blocks(ObjID o1, ObjID o2)
{
   return FALSE;
}

void (*portal_render_object)(ObjID, uchar *, ulong fragment) = render_object;
BOOL (*portal_object_visible)(ObjID) = object_visible;
BOOL (*portal_object_blocks)(ObjID, ObjID) = object_blocks;

void (*portal_sfx_callback)(int cell) = dummy_sfx;
void (*portal_pre_draw_callback)(void) = NULL;

bool portal_allow_object_splitting = TRUE;

unsigned char pt_medium_entry_clut[256];
unsigned char pt_medium_exit_clut[256];
unsigned char pt_medium_haze_clut[256];

unsigned char pt_motion_haze_clut[256];


mxs_vector portal_camera_loc;

/////////////////////////////////////////////////////////////////////
//
//  statistics

#ifdef DBG_ON
  #ifndef STATS_ON
  #define STATS_ON
  #endif
#endif

#ifdef STATS_ON

 #define STAT(x)  (x)

 static int stat_num_port;
 static int stat_num_port_visit;
 static int stat_num_port_traverse;
 static int stat_num_cell;
 static int stat_num_cell_visit;
 static int stat_num_cell_explored;
 static int stat_num_facing;
 static int stat_num_objects;
 static int stat_num_visible_objects;
 static int stat_num_terrsplit_objects;
 static int stat_num_resplit_objects;
 static int stat_num_hidden_objects;
 static int stat_num_cell_tests;
 static int stat_num_partial_objects;
 int stat_num_polys_clipped_away;
 int stat_num_poly_raw;
 int stat_num_poly_considered;
 int stat_num_poly_drawn;
 int stat_num_lit_pixels;
 int stat_num_source_pixels;
 int stat_num_spans_drawn;
 int stat_num_spans_clamped;
 int stat_num_dest_pixels;
 int stat_num_backface_tests;
 int stat_num_traverse_ms;
 int stat_num_render_ms;
 int stat_num_sort_ms;
 int stat_num_object_ms;
 int stat_num_drawn_pixels;
 int stat_num_clipped_pixels;
 int stat_num_transp_pixels;
 int stat_max_undrawn_pixels;
 int stat_max_undrawn_polys;
 int stat_max_total_polys;

#else

 #define STAT(x)

#endif

#define STAT_INC(x)  STAT(++stat_num_##x)

///// TODO: move externs into h file

extern ClipData *PortalGetClipInfo(PortalCell *, PortalPolygonCore *, int voff, ClipData *);
extern uchar compute_water_clut(mxs_real, mxs_real);
extern mxs_real compute_portal_z(void);
extern void duv_set_size(int x, int y);
extern void portal_preload_lightmaps(int cell);
extern void draw_region(int);
extern void draw_region_lgd3d(int);
extern void draw_region_lightmap_only(int);
extern int  pick_region(PortalCell *, int x, int y);
extern bool portal_raycast_light(PortalCell *, Location *loc, uchar perm);
extern bool portal_raycast_sunlight(PortalCell *r, uchar perm);
extern bool portal_nonraycast_light(PortalCell *, Location *loc, uchar perm);
extern void portal_dynamic_light(PortalCell *, Location *loc);
extern void portal_dynamic_dark(PortalCell *, Location *loc);
extern void render_background_hack();
extern void background_hack_cleanup(void);
extern void PortalSetLightInfo(Location *l, float br, uchar start);
extern bool dynamic_light;
extern void light_region(int);
//extern void spotlight_region(int);
extern void init_portal_light(void);
extern void init_portal_shading(int dark, int light);
extern void init_background_hack(void);
extern BOOL g_lgd3d;

///// examine all portals in region /////

void add_region(int, ClipData *c, PortalCell *src, PortalPolygonCore *port);

static BOOL z_test;
static float test_radius;

static BOOL poly_passes_ztest(PortalCell *r, PortalPolygonCore *p, int v)
{
   // check that at least one z value is close enough
   int i,n;
   uchar *vl = r->vertex_list + v;
   n = p->num_vertices;
   for (i=0; i < n; ++i)
      if (cur_ph[vl[i]].p.z < test_radius)
         return TRUE;
   return FALSE;   // poly is too far away
}

static bool flashbomb_through_door_hack=FALSE;
  // explore through the portal
void explore_portals(PortalCell *r)
{  PROF
   int n = r->num_portal_polys;
   PortalPolygonCore *p = r->portal_poly_list;
   int v = r->portal_vertex_list;
   void *clip = CLIP_DATA(r);

   // If this cell is in a door, or something silly like that, we
   // won't explore from it.
   if ((r->flags & CELL_BLOCKS_VISION) && !flashbomb_through_door_hack) {
      END_PROF;
      return;
   }
   
   /*//zb
   if (g_lgd3d && portal_fog_on && portal_fog_dist && (r->flags & CELL_FOG)
    && (mx_dist_vec(&portal_camera_loc, &r->sphere_center) - r->sphere_radius
      > portal_fog_dist)) {
      r->flags |= CELL_FOGGED_OUT;
      END_PROF;
      return;
   }
   */

   cur_ph = POINTS(r);

   r3_start_block();
   r3_set_clipmode(NEED_CLIP(r) ? R3_CLIP : R3_NO_CLIP);
   while (n--) {
      STAT_INC(port_visit);

#ifndef SHIP
      if (trav_file)
         fprintf(trav_file, "  Consider portal %d:", r->num_portal_polys-n+1);
#endif
      if (check_surface_visible(r, p, v)) {   // if we can see through portal
         ClipData *portal_extent;             // get 2d extents of portal
         portal_extent = PortalGetClipInfo(r, p, v, clip);
         if (portal_extent) {                 // clip against cell's visibility
            STAT_INC(port_traverse);          // if visible, go through it
#ifndef SHIP
            if (trav_file)
               fprintf(trav_file, "  cell %d visible\n", p->destination);
#endif
            if (!z_test || poly_passes_ztest(r, p, v)) {
               add_region(p->destination, portal_extent, r, p);
                 // add_region may change our clipmode, so reset it
               r3_set_clipmode(NEED_CLIP(r) ? R3_CLIP : R3_NO_CLIP);
            } else {
               PortalClipFree(portal_extent);
            }
         } else {
#ifndef SHIP
            if (trav_file)
               fprintf(trav_file, "  obscured/offscreen\n");
#endif
         }
      } else {
#ifndef SHIP
         if (trav_file)
            fprintf(trav_file, "  backfaced\n");
#endif
      }
      v += p->num_vertices;
      ++p;
   }
   r3_end_block();
   END_PROF;
}

bool hack_fragment[MAX_VISIBLE_OBJECTS]; // temporary hack
#define FRAGMENT(x)   Assert_(hack_fragment[x])
#define CELL(x)       Assert_(!hack_fragment[x])

///// do object processing for region /////

#define VISOBJ_NULL      (-1)



bool obj_dealt[HACK_MAX_OBJ];  // HACK: need real object dealt flags
bool obj_split[HACK_MAX_OBJ];  // HACK: need real object split flags
bool obj_hide[HACK_MAX_OBJ];   // HACK: need real object hide flags

// only need this for objects which have dealt != 0
PortalCell *obj_first_cell[HACK_MAX_OBJ]; // HACK: argh what a waste

ObjVisibleID obj_fragment_list[HACK_MAX_OBJ];

#define MAX_NEED_TESTING  (512)

ObjVisible vis_objs[MAX_VISIBLE_OBJECTS];  // 8K
ObjVisibleID num_visible_objects;
ObjID obj_need_testing[MAX_NEED_TESTING]; // max visible objects, not fragments
int need_testing_count;

STATIC
int get_visobj(ObjID o, int fragment)
{
   ObjVisibleID his;
   if (num_visible_objects >= MAX_VISIBLE_OBJECTS)
      return VISOBJ_NULL;
   his = num_visible_objects++;
   vis_objs[his].obj = o;
hack_fragment[his] = TRUE;
   vis_objs[his].fragment = fragment;
   return his;
}

STATIC
void queue_object_fragment_in_cell(PortalCell *r, int visobj)
{
   if (visobj < 0) return;

#ifdef STATS_ON
   ++stat_num_visible_objects;
#endif

   vis_objs[visobj].next_visobj = OBJECTS(r);
   OBJECTS(r) = visobj;
}

STATIC
void queue_object_fragment_for_obj(PortalCell *r, int visobj)
{
   int o;
   if (visobj < 0) return;

   o = vis_objs[visobj].obj;
hack_fragment[visobj] = FALSE;
   vis_objs[visobj].cell = r;
   vis_objs[visobj].next_visobj = obj_fragment_list[o];
   obj_fragment_list[o] = visobj;
}

// first time we encounter an object not split
void deal_with_unsplit_object(PortalCell *r, ObjID o)
{
   int n = get_visobj(o, OBJ_NO_SPLIT);
   queue_object_fragment_in_cell(r, n);
   obj_fragment_list[o] = n;
   obj_first_cell[o] = r;

   // add to the list of unsplit objects which might need splitting
   if (need_testing_count < MAX_NEED_TESTING)
      obj_need_testing[need_testing_count++] = o;
   else
      Warning(("Too many objects to test for splitting!\n"));
}

// other times we encounter an object not split
void deal_with_object_leftover(PortalCell *r, ObjID o)
{
   queue_object_fragment_for_obj(r, get_visobj(o, OBJ_NO_SPLIT));
}

// first time we encounter an object to be split
void deal_with_splitting_object(PortalCell *r, ObjID o)
{
   queue_object_fragment_in_cell(r, get_visobj(o, OBJ_SPLIT_FIRST));
   obj_fragment_list[o] = VISOBJ_NULL;
}

// other times we encounter an object to be split
void deal_with_split_object(PortalCell *r, ObjID o)
{
   queue_object_fragment_in_cell(r, get_visobj(o, OBJ_SPLIT_OTHER));
}

BOOL poly_greater(PortalCell *r, PortalPolygonCore *p, int v, int axis,
   float value)
{
   int i,n = p->num_vertices;
   uchar *vl = r->vertex_list + v;

   for (i=0; i < n; ++i)
      if (r->vpool[*vl++].el[axis] < value)
         return FALSE;
   return TRUE;
}

BOOL poly_less(PortalCell *r, PortalPolygonCore *p, int v, int axis,
   float value)
{
   int i,n = p->num_vertices;
   uchar *vl = r->vertex_list + v;

   for (i=0; i < n; ++i)
      if (r->vpool[*vl++].el[axis] > value)
         return FALSE;
   return TRUE;
}

BOOL poly_overlap_2d(PortalCell *r, PortalPolygonCore *p, int v, fix *min2d, fix *max2d)
{
   int ccode = 0, n = p->num_vertices, i, code_2d;
   uchar *vl = r->vertex_list + v;
   r3s_point *plist = POINTS(r);

   // generate the polygon clip code
   for (i=0; i < n; ++i)
      ccode |= plist[vl[i]].ccodes;

   code_2d = 255;

   if (!ccode) {
      // if we don't need to clip, test it directly by generating new ccode,
      // this time we want a ccode_and

      for (i=0; i < n; ++i) {
         r3s_point *pt = plist + vl[i];
         fix xy,yx;
         xy = pt->grp.sx + pt->grp.sy;
         yx = pt->grp.sx - pt->grp.sy;
         if (pt->grp.sx > min2d[0]) code_2d &= ~1;
         if (pt->grp.sx < max2d[0]) code_2d &= ~2;
         if (pt->grp.sy > min2d[1]) code_2d &= ~4;
         if (pt->grp.sy < max2d[1]) code_2d &= ~8;
         if (xy > min2d[2]) code_2d &= ~16;
         if (xy < max2d[2]) code_2d &= ~32;
         if (yx > min2d[3]) code_2d &= ~64;
         if (yx < max2d[3]) code_2d &= ~128;
      }
   } else {
      // clip the polygon
      r3s_phandle vlist[32], *nvl;
      for (i=0; i < n; ++i)
         vlist[i] = plist + vl[i];
      n = r3_clip_polygon(n, vlist, &nvl);
      for (i=0; i < n; ++i) {
         r3s_point *pt = nvl[i];
         fix xy,yx;
         xy = pt->grp.sx + pt->grp.sy;
         yx = pt->grp.sx - pt->grp.sy;
         if (pt->grp.sx > min2d[0]) code_2d &= ~1;
         if (pt->grp.sx < max2d[0]) code_2d &= ~2;
         if (pt->grp.sy > min2d[1]) code_2d &= ~4;
         if (pt->grp.sy < max2d[1]) code_2d &= ~8;
         if (xy > min2d[2]) code_2d &= ~16;
         if (xy < max2d[2]) code_2d &= ~32;
         if (yx > min2d[3]) code_2d &= ~64;
         if (yx < max2d[3]) code_2d &= ~128;
      }
   }
   if (code_2d != 0)
      return FALSE;  // we trivially do not overlap the bounding octagon

   // we don't trivially miss the rectangle
   // in this case, we should probably do a non-trivial test (clip
   // the polygon by the bounding octagon, see if result is non-empty)
   // better yet, we should use the convex bounding volume of
   // the object, by computing the silhouette edges of the
   // bounding box... oh well

   // we could at least do a more thorough test of whether
   // the polygon is in front/behind the object, say by using
   // the object's bounding planes

   return TRUE;
}

extern BOOL bbox_intersects_plane(mxs_vector *bbox_min, mxs_vector *bbox_max,
          PortalPlane *p);

// return TRUE if the object can be drawn in front of this cell
BOOL object_in_front(PortalCell *r, mxs_vector *mn, mxs_vector *mx,
             fix *mn2d, fix *mx2d)
{
   int i,n,v=0;
   ulong plane_check;     // have we tested this plane
   ulong plane_result;    // the result for this plane
   PortalPolygonCore *p = r->poly_list;

   // any rendered portal polygon will force an explicit split, so ignore those
   n = r->num_polys - r->num_portal_polys;

   plane_check = 0;
   plane_result = 0;

   r3_start_block();  // for clipping I guess

   for (i=0; i < n; ++i) {
      int plane = p->planeid;
      BOOL result;
      if (plane_check & (1 << plane))
         result = plane_result & (1 << plane);
      else {
         PortalPlane *p = &r->plane_list[plane];
         float dist = mx_dot_vec(&portal_camera_loc, &p->normal)
                      + p->plane_constant;
         if (dist >= 0 && bbox_intersects_plane(mn, mx, p)) {
            result = TRUE;
            plane_result |= 1 << plane;
         } else {
            result = FALSE;
         }

         plane_check |= 1 << plane;
      }

      if (result) {
         // check for an axially aligned split plane which reveals
         // that the polygon is on the opposite side of the object
         // from the viewer
         // maybe we should explicitly build the bbox of the poly?
         // this way we get to early out...

#define pcl       portal_camera_loc
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

         if (poly_less(r, p, v, 0, MIN(pcl.x,mn->x))) goto behind;
         if (poly_greater(r, p, v, 0, MAX(pcl.x,mx->x))) goto behind;
         if (poly_less(r, p, v, 1, MIN(pcl.y,mn->y))) goto behind;
         if (poly_greater(r, p, v, 1, MAX(pcl.y,mx->y))) goto behind;
         if (poly_less(r, p, v, 2, MIN(pcl.z,mn->z))) goto behind;
         if (poly_greater(r, p, v, 2, MAX(pcl.z,mx->z))) goto behind;
#undef pcl
#undef MAX
#undef MIN

         if (poly_overlap_2d(r, p, v, mn2d, mx2d))
         {
            r3_end_block();
            return FALSE;
         }
      }
     behind:
      v += p->num_vertices;
      ++p;
   }

   r3_end_block();
   return TRUE;
}

extern void portal_obj_bounds(ObjID obj, mxs_vector **mn, mxs_vector **mx,
           mxs_vector **center, float *radius, fix *mn2d, fix *mx2d);

bool visible_in_cell(PortalCell *r, fix *mn2d, fix *mx2d)
{
   return PortalClipOverlap(CLIP_DATA(r), mn2d, mx2d);
}

// the list of cells to draw
int active_regions[MAX_ACTIVE_REGIONS];
int r_sorted_count, r_total_count;

// bitfields for visible cells--we depend on the app to clear these per
// frame so that if we draw three scenes in a frame we'll mark the cells
// for all of them
uchar portal_cell_rendered[1 + MAX_REGIONS / 8];

bool portal_object_complete_test=TRUE;

void split_object(ObjID o)
{
   int v,next;
   v = obj_fragment_list[o];
   if (v < 0 || v >= MAX_VISIBLE_OBJECTS)
      return;

   // for all fragments except the last in the chain, move them
   // to the appropriate cell and set their fragment # to the split
   // value.  The last item in the chain (the nearest to the viewer)
   // is already in the right cell, and just needs fragment #

   next = vis_objs[v].next_visobj;
   while (next >= 0 && next < MAX_VISIBLE_OBJECTS && vis_objs[next].obj == o) {
CELL(v);
      queue_object_fragment_in_cell(vis_objs[v].cell, v);
hack_fragment[v] = TRUE;
      vis_objs[v].fragment = OBJ_SPLIT_OTHER;

      v = next;
      next = vis_objs[v].next_visobj;
   }

hack_fragment[v] = TRUE;
   vis_objs[v].fragment = OBJ_SPLIT_FIRST;
   obj_fragment_list[o] = VISOBJ_NULL;
}

extern int mCellRefSys;
BOOL test_object_cells_visible(ObjID obj)
{
   ObjRefID first = OBJ_FIRST_REF(obj, mCellRefSys);
   ObjRefID ref;
   if (!first)
      return TRUE;
   ref = first;
   do {
      ObjRef *p = OBJREFID_TO_PTR(ref);
      int cell = * (int *) &p->bin;
      if (!WR_CELL(cell)->render_data)
         return FALSE;
      ref = p->next_of_obj;
   } while (ref != first);
   return TRUE;
}

void check_object_split(ObjID o)
{
   int v,next;
   mxs_vector *mn,*mx,*center;
   fix mn2d[4], mx2d[4];
   float radius;

   if (!portal_object_visible(o)) {
      obj_hide[o] = TRUE;
#ifdef STATS_ON
         ++stat_num_hidden_objects;
#endif
      return;
   }

   portal_obj_bounds(o, &mn, &mx, &center, &radius, mn2d, mx2d);

   v = obj_fragment_list[o];
   next = vis_objs[v].next_visobj;

   if (next == VISOBJ_NULL || vis_objs[next].obj != o) {
      if (!visible_in_cell(obj_first_cell[o], mn2d, mx2d)) {
         obj_hide[o] = TRUE;
#ifdef STATS_ON
         ++stat_num_hidden_objects;
#endif
         return;
      }
   } else if (!visible_in_cell(obj_first_cell[o], mn2d, mx2d)) {
   // iterate over all the cells the object is in,
   // and see if it's visible in any

      obj_hide[o] = TRUE;

      // check all but the last entry
      while (next != VISOBJ_NULL && vis_objs[next].obj == o) {
CELL(v);
         if (visible_in_cell(vis_objs[v].cell, mn2d, mx2d)) {
            obj_hide[o] = FALSE;
            break;
         }
         v = next;
         next = vis_objs[v].next_visobj;
      }

      if (obj_hide[o] == TRUE) {
#ifdef STATS_ON
         ++stat_num_hidden_objects;
#endif
         return;
      }

      // reset current search location to how it was before we looped

      v = obj_fragment_list[o];
      next = vis_objs[v].next_visobj;
   }

   {
      int far_index, near_index, i;
      BOOL partial = !test_object_cells_visible(o);

      if (next == VISOBJ_NULL || vis_objs[next].obj != o) {
         // only in one cell
         if (!partial)
            return;
      } else {
         CELL(v);
      }

      near_index = obj_first_cell[o]->render_data->sorted_index;

      // now, the object didn't get hidden, so check if it's
      // fully visible
      if (partial) {
#ifdef STATS_ON
         ++stat_num_partial_objects;
#endif
         far_index = r_sorted_count;
      } else {
        far_index = ((PortalCell *) vis_objs[v].cell)->render_data->sorted_index;
      }

      // now test all cells except furthest
      for (i=near_index; i < far_index; ++i) {
#ifdef STATS_ON
         ++stat_num_cell_tests;
#endif
         if (!object_in_front(wr_cell[active_regions[i]], mn, mx, mn2d, mx2d))
            break;
      }

      if (i == far_index)
      {
         return;
      }
   }

   // we get here if the object needs splitting

   STAT(++stat_num_terrsplit_objects);
   split_object(o);
}

// check if objects need to be split due to terrain
void check_for_object_splitting(void)
{
   int n=need_testing_count,i;

   for (i=0; i < n; ++i)
      check_object_split(obj_need_testing[i]);
}

// check if objects need to be split due to terrain
void check_for_object_hiding(void)
{
   int v,next;
   mxs_vector *mn,*mx,*center;
   fix mn2d[4], mx2d[4];
   float radius;

   int n=need_testing_count,i;

   for (i=0; i < n; ++i) {
      ObjID obj = obj_need_testing[i];
      // check if the game really thinkgs it's visible
      if (portal_object_visible(obj)) {
         // check if it's visible in any of the cells
         portal_obj_bounds(obj, &mn, &mx, &center, &radius, mn2d, mx2d);

         v = obj_fragment_list[obj];
         next = vis_objs[v].next_visobj;

         if (visible_in_cell(obj_first_cell[obj], mn2d, mx2d))
            continue;

         // check all but the last entry
         while (next != VISOBJ_NULL && vis_objs[next].obj == obj) {
CELL(v);
            if (visible_in_cell(vis_objs[v].cell, mn2d, mx2d))
               goto cont;  // continue out two levels
            v = next;
            next = vis_objs[v].next_visobj;
         }
      }

      obj_hide[obj] = TRUE;
#ifdef STATS_ON
      ++stat_num_hidden_objects;
#endif
     cont:
      ;
   }
}

// check all objects (even non-visible refs) in this cell;
// if they are visible and not already split, and their
// frontmost cell is > front, check if object obj blocks view
// of them; if it does, force it to split

// cell is a cell#; front is a sorted index

BOOL force_object_splits(int cell, int front, int obj)
{
   PortalCell *r = wr_cell[cell];
   ObjRefID id = * (int *) (&r->refs);

   while (id) {
      ObjRef *p = OBJREFID_TO_PTR(id);
      ObjID o = p->obj;

      // is object o being drawn?
      if (obj_dealt[o] && !obj_hide[o]) {
         // does it matter if o blocked obj?
         // if o blocks obj, but obj gets drawn "in front", then bad;
         // that happens if FRONT(o) is < front,
         // or if it's split

         if (obj_fragment_list[o] == VISOBJ_NULL ||
              front < obj_first_cell[o]->render_data->sorted_index) {
            if (portal_object_blocks(o, obj)) {
               STAT(++stat_num_resplit_objects);
               split_object(obj);
               return TRUE;
            }
         }
      }
      id = p->next_in_bin;
   }
   return FALSE;
}

// check if objects need to be split due to other objects
void check_for_extra_object_splitting(void)
{
   int i;

   for(i=0; i < r_sorted_count; ++i) {
      // consider all objects rendered in this cell

      int cell = active_regions[i];
      PortalCell *r = wr_cell[cell];
      int v,next,o;

      if (!r->render_data)                       // @TBD (toml 09-02-98): attempted band-aid. needs real investigation
      {
         mprintf("ECTS: !r->render_data\n");
         continue;
      }

      v = OBJECTS(r);
      while (v != VISOBJ_NULL) {
        // grab the next field before we mess with it
        // this actually SHOULD be totally safe without
        // it, because I'm so zany with this goofy data
        // structure, but better safe than sorry
        next = vis_objs[v].next_visobj;

        o = vis_objs[v].obj;
        if (!obj_hide[o] && vis_objs[v].fragment == OBJ_NO_SPLIT) {
           // determine frontmost & backmost cells
           int far_index, near_index, j;

           // if more than one cell
           if (vis_objs[obj_fragment_list[o]].next_visobj != VISOBJ_NULL &&
               vis_objs[vis_objs[obj_fragment_list[o]].next_visobj].obj == o) {
CELL(obj_fragment_list[o]);
              far_index = ((PortalCell *) vis_objs[obj_fragment_list[o]].cell)->
                                                   render_data->sorted_index;
              near_index = i;
              for (j=near_index; j <= far_index; ++j)
                 if (force_object_splits(active_regions[j], near_index, o))
                    break;
           }
        }
        v = next;
      }
   }
}

void process_objects(PortalCell *r)
{
   ObjRefID id = * (int *) (&r->refs);

   while (id) {
      ObjRef *p = OBJREFID_TO_PTR(id);
      ObjID o = p->obj;

      if (!obj_dealt[o]) {
         if (portal_allow_object_splitting && obj_split[o])
            deal_with_splitting_object(r, o);
         else
            deal_with_unsplit_object(r,o);

         obj_dealt[o] = TRUE;
         obj_hide[o] = FALSE;
#ifdef STATS_ON
         ++stat_num_objects;
#endif
      } else
         if (portal_allow_object_splitting && obj_split[o])
            deal_with_split_object(r, o);
         else
            deal_with_object_leftover(r, o);

      id = p->next_in_bin;
   }
}

void init_process_objects(void)
{
   num_visible_objects=0;
   need_testing_count = 0;
}

///////////////////// the rendering pipeline ///////////////////////

#define IS_VISITED(n)     (VISIT(WR_CELL(n)))
#define CLEAR_VISITED(n)  (VISIT(WR_CELL(n))=FALSE)
#define SET_VISITED(n)    (VISIT(WR_CELL(n))=TRUE)

#define AVERAGE_PORTALS_PER_CELL    8

PortalCellRenderData rdata[MAX_ACTIVE_REGIONS];

// new sorting algorithm is in sort.txt in the renderer docs

static ushort outgoing_portals[MAX_ACTIVE_REGIONS * AVERAGE_PORTALS_PER_CELL];
static int outgoing;

bool has_portal_to(int from, PortalCell *dest)
{  PROF
   PortalCell *src = WR_CELL(from);
   int i, n, k;
   bool result;

   if (!src->render_data) {
      END_PROF;
      return FALSE;
   }

   result = FALSE;

   n = NUM_OUTGOING(src),
   k = FIRST_OUTGOING(src);
   for (i=0; i < n; ++i)
      if (WR_CELL(outgoing_portals[k+i]) == dest) {
#ifdef VALIDATE_UNIQUENESS_OF_PORTALS
         for(++i; i < n; ++i)
            if (WR_CELL(outgoing_portals[k+i]) == dest) {
               Warning(("has_portal_to: more than one portal between two cells!\n"));
            }
#endif // ~VALIDATE_UNIQUENESS_OF_PORTALS
         result = TRUE;
         break;
      }

   END_PROF;
   return result;
}

  // add the destinations to the outgoing portal list
void examine_portals(PortalCell *r)
{  PROF
   int n = r->num_portal_polys;
   PortalPolygonCore *p = r->portal_poly_list;
   int v = r->portal_vertex_list;
   r3s_point *old_ph = cur_ph;
   PortalCellRenderData *z = r->render_data;

   // don't bother putting us on the 0count list

   z->num_outgoing_portals = 0;
   z->num_unexplored_entries = 0;
   z->outgoing_portal_offset = outgoing;

   cur_ph = POINTS(r);

   while (n--) {
      // check if the destination already points to us; we have
      // to avoid inconsistency in the backface check

      if (has_portal_to(p->destination, r)) {
         if (!IS_VISITED(p->destination)) {
            ++z->num_unexplored_entries;
         }
      } else if (check_surface_visible(r, p, v)) {
         if (outgoing < MAX_ACTIVE_REGIONS * AVERAGE_PORTALS_PER_CELL) {
            ++z->num_outgoing_portals;
            outgoing_portals[outgoing++] = p->destination;
            CELL_DEBUG(mprintf("  outgoing to %d\n", p->destination));
         } else
            mprintf("examine_portals: Overflowed outgoing_portals table.\n");
      }

      // it might be that the destination didn't point to us, and
      // we didn't point to it, if we're right on the plane.  But
      // that should be ok.  Rounding error could also make that
      // happen, but we've guaranteed that we won't point to each
      // other and cause a little local cycle

      v += p->num_vertices;
      ++p;
   }

   cur_ph = old_ph;
   END_PROF;
}

// now, during the front-to-back pass, the
//   above list is topologically sorted
//   with a selection sort.  This means that
//   at any given point in time, the first
//   n elements are sorted and the rest aren't,
//   and then some more are appended on the
//   end as we go, woo woo

// for every active region, we need
// some clipping data

int r_rdata;

void validate_incoming_count(int n, char *where)
{
   int i,k=0;
   for (i=0; i < r_rdata; ++i) {
      if (has_portal_to(active_regions[i], WR_CELL(n)))
         if (!IS_VISITED(active_regions[i]))
            ++k;
   }
   if (k != NUM_INCOMING(WR_CELL(n)))
      Error(1, "%s: Invalid NUM_INCOMING: is %d, should be %d\n", where,
         NUM_INCOMING(WR_CELL(n)), k);
}

void portal_validate_lists(char *where)
{
   int i;

   for (i=0; i < r_sorted_count; ++i)
      if (!IS_VISITED(active_regions[i]))
         Error(1, "!IS_VISITED entry out of place in %s\n", where);
   for (; i < r_total_count; ++i)
      if (IS_VISITED(active_regions[i]))
         Error(1, "IS_VISITED entry out of place in %s\n", where);

   for (i=0; i < r_total_count; ++i)
      validate_incoming_count(active_regions[i], where);
}

bool skip_clip;

bool setup_cell(PortalCell *r)
{  PROF
   int i;
   float m;
   if (r_rdata == MAX_ACTIVE_REGIONS) {
#ifdef DBG_ON
      mprintf("Scene complexity high, increase MAX_ACTIVE_REGIONS\n");
#endif // ~DBG_ON
      END_PROF;
      return TRUE;
   }

   r3_set_clipmode(R3_CLIP);

   STAT_INC(cell);
   STAT(stat_num_port += r->num_portal_polys);

   r->render_data = &rdata[r_rdata++];

   CLIP_DATA(r) = 0;
   VISIT(r) = 0;
   ZWATER(r) = 0;
   OBJECTS(r) = VISOBJ_NULL;
   CLUT(r).clut_id = 0;
   CLUT(r).clut_id2 = 0;
   CLUT(r).next = 0;

   r3d_ccodes_or = 0;

   POINTS(r) = Malloc(r->num_vertices * sizeof(r3s_point));
   r3_transform_block(r->num_vertices, POINTS(r), r->vpool);
   NEED_CLIP(r) = skip_clip ? r3d_ccodes_or : 1;

   m = 0;

   for (i=0; i < r->num_vertices; ++i)
      m += POINTS(r)[i].p.z;
   DIST(r) = m / r->num_vertices;

   examine_portals(r);

   END_PROF;
   return FALSE;
}

void free_cell(PortalCell *r)
{
   if (CLIP_DATA(r))
      PortalClipFree(CLIP_DATA(r));
   if (POINTS(r))
      Free(POINTS(r));

   //free_render_data(r->render_data);
   r->render_data = 0;
}

void mark_outgoing_portals(PortalCell *r)
{  PROF
   int i,n = NUM_OUTGOING(r), k = FIRST_OUTGOING(r);
#ifdef DBG_ON
   if (VISIT(r)) Error(1, "Tried to mark outgoing from a sorted cell.\n");
#endif

   for (i=0; i < n; ++i) {
      PortalCell *out = WR_CELL(outgoing_portals[k+i]);
      if (out->render_data)
         ++NUM_INCOMING(out);
   }
   END_PROF;
}

void unmark_outgoing_portals(PortalCell *r)
{  PROF
   int i, n = NUM_OUTGOING(r), k = FIRST_OUTGOING(r);
#ifdef DBG_ON
   if (!VISIT(r)) Error(1, "Tried to unmark outgoing from an unsorted cell.\n");
#endif
   for (i=0; i < n; ++i) {
      PortalCell *out = WR_CELL(outgoing_portals[k+i]);
      if (out->render_data)
         --NUM_INCOMING(out);
   }
   END_PROF;
}

// we've uncovered a region, add it to the list
//   or expand its clipping info if already there
void add_region(int n, ClipData *c, PortalCell *src, PortalPolygonCore *p)
{  PROF
   PortalCell *dest = WR_CELL(n);
   STAT_INC(cell_visit);

   if (!dest->render_data) {
#ifndef SHIP
      if (trav_file)
         fprintf(trav_file, "    First time in %d.\n", n);
#endif
      CELL_DEBUG(mprintf("Expand new cell %d\n", n));
      if (setup_cell(dest)) {
         END_PROF;
         return;
      }
      CLEAR_VISITED(n);
      CLIP_DATA(dest) = c;
      CLUT(dest) = CLUT(src);
      CELL_FLAGS(dest) = CELL_FLAGS(src) | dest->flags;
      if (p->clut_id)
      {
         uchar clut, m1, m2, clut2;

         // first determine what medium we're emerging from and going into
         m1 = src->medium;
         m2 = dest->medium;

         // check if the source medium has a clut
         clut = pt_medium_exit_clut[m1];
         if (!clut)
         {
            int mot = src->motion_index;
            if (mot > 0)
               clut = pt_motion_haze_clut[mot];

            if (!clut)
               clut = pt_medium_haze_clut[m1];

            if (clut)
               clut += compute_water_clut(ZWATER(src), compute_portal_z());
         }

         // check if the dest medium has an entry clut
         clut2 = pt_medium_entry_clut[m2];

         // if only one clut, make clut1 = it
         // if two cluts, swap them, because order is reversed
         if (clut2) {
            if (!clut) {
               clut = clut2;
               clut2 = 0;
            } else {
               uchar temp = clut;
               clut = clut2;
               clut2 = temp;
            }
         }

         if (clut) {
            CLUT(wr_cell[n]).next = CLUT(src).clut_id ? &CLUT(src) : 0;
            CLUT(wr_cell[n]).clut_id = clut;
            CLUT(wr_cell[n]).clut_id2 = clut2;
         }

         // check if the dest medium hazes
         if (pt_medium_haze_clut[m2])
            ZWATER(wr_cell[n]) = compute_portal_z();
      }
      active_regions[r_total_count++] = n;
   } else {
      PortalClipUnion(CLIP_DATA(dest), c);
      PortalClipFree(c);
      CELL_DEBUG(mprintf("Revisit cell %d\n", n));

      // if we portal from air into water, merge z value
      // so we keep nearest, which matches well when we move camera under water
      if (p->clut_id) {
         mxs_real z = compute_portal_z();
         // BUG: if you can get here one way through water and one way not,
         // this comparison is totally bogus
         if (z < ZWATER(dest))
            ZWATER(dest) = z;
      }

      // TODO: we should merge the clut info!

      if (IS_VISITED(n)) {
         // move this place off the visited list back onto the
         // unvisited list, because we might explore it differently,
         // and to force correct sorting.
         int i=0;
         while (active_regions[i] != n)
            ++i;

         memmove(&active_regions[i], &active_regions[i+1],
            sizeof(active_regions[0])*(r_sorted_count-i-1));
         active_regions[--r_sorted_count] = n;
         CLEAR_VISITED(n);
      } else {
         END_PROF;
         return; // don't mark outgoing portals, they already are!
      }
   }

   if (!IS_VISITED(n))
      mark_outgoing_portals(wr_cell[n]);
   END_PROF;
}

/////////////////////// select region /////////////////////////


// Take all the cells on the list, and pick one which
// has nothing unselected in front of it

int select_next_region(void)
{  PROF
   int i, n;
   float mn;

   n = -1;
      mn = 1e20f;

   for (i = r_sorted_count; i < r_total_count; ++i) {
      if (NUM_INCOMING(wr_cell[active_regions[i]]) > 0)
         continue;
      if (DIST(wr_cell[active_regions[i]]) < mn) {
         mn = DIST(wr_cell[active_regions[i]]);
         n = i;
      }
   }

   if (n == -1) {
      for (i = r_sorted_count; i < r_total_count; ++i) {
         if (DIST(wr_cell[active_regions[i]]) < mn) {
            mn = DIST(wr_cell[active_regions[i]]);
            n = i;
         }
      }
      if (n == -1) n = r_sorted_count;
      Warning(("Didn't find any 0-incoming cells, used %d!\n", NUM_INCOMING(wr_cell[active_regions[n]])));
   }

   END_PROF;
   return n;
}

///////////////// main pipeline loop //////////////////////////

void initialize_first_region(int n)
{
   // this needs to be the location of the camera
   PortalCell *cell = WR_CELL(n);

   r3_start_block();
   if (setup_cell(cell))
      Error(1, "initialize_first_region: no free cells?!!.\n");

   mark_outgoing_portals(cell);

   r3_end_block();

   active_regions[0] = n;
   r_total_count = 1;
   r_sorted_count = 0;

   CLIP_DATA(cell) = PortalClipRectangle(0,0, grd_bm.w, grd_bm.h);
   CLUT(cell).clut_id = 0;
   CLUT(cell).next = 0;
   CELL_FLAGS(cell) = cell->flags;
}

bool render_backward;

long pt_get_time(void) { return 0; }
long (*portal_get_time)(void) = pt_get_time;
long (*portal_get_frame_time)(void) = pt_get_time;

void (*portal_anim_light_callback)(long time_change_millisec);
void (*portal_anim_medium_callback)(long time_change_millisec);

extern int portal_sky_id;

static BOOL bRenderSky = FALSE;

void portal_traverse_scene(int cell)
{  PROF
   int off,n, count = 0;
   PortalCell *pCell;
   int i,j;

   bRenderSky = FALSE;

#ifdef STATS_ON
   stat_num_traverse_ms = portal_get_time();
#endif

   r_rdata = 0;  // clear count of used cells
   outgoing = 0; // clear count of used outgoing portal list

     // initialize the list of unexplored regions
     // pass in the region containing the camera
   initialize_first_region(cell);

     // now, as long as there are unexplored regions
   while (r_sorted_count < r_total_count) {

#ifdef VALIDATE
      portal_validate_lists("before select");
#endif

        // find the next region to explore (roughly front-to-back)
      off = select_next_region();
      n = active_regions[off];

#ifndef SHIP
      if (trav_file)
         fprintf(trav_file, "Expand cell %d:\n", n);
#endif
      STAT_INC(cell_explored);
      CELL_DEBUG(mprintf("%d onto sorted list\n", n));

        // swap it to the end of the sorted list
      active_regions[off] = active_regions[r_sorted_count];
      active_regions[r_sorted_count++] = n;

        // mark it as visited
      SET_VISITED(n);

        // decrement NUM_INCOMING counts of reachable cells
      unmark_outgoing_portals(wr_cell[n]);

        // and now explore the portals out of it, this adds new regions to
        // the active region list (and may even move some back from the sorted
        // area to the unsorted area)
      explore_portals(wr_cell[n]);

        // Avoid an infinite loop.  The original 1024-step guard predates the
        // extended active-region capacity and truncates valid large NewDark
        // scenes.  Bound the work by the extended active-region capacity;
        // revisits consume that budget too, preventing malformed portal
        // cycles from stalling an entire frame.
      if (++count >= MAX_ACTIVE_REGIONS) {
#ifndef SHIP
         if (trav_file)
            fprintf(trav_file, "Apparent cell cycle!\n");
#endif
         mprintf("Apparent cell cycle in portal_traverse_scene\n");
         mprintf(" cell %d, center %g %g %g\n", n, WR_CELL(n)->sphere_center.x, WR_CELL(n)->sphere_center.y, WR_CELL(n)->sphere_center.z);
         r_sorted_count = r_total_count;
         break;
      }
   }

// Traverse active cells to see if any sky is visible:
   for (i = r_sorted_count-1; (i >= 0) && !bRenderSky; i--)
   {
      pCell = WR_CELL(active_regions[i]);
      for (j = 0; (j < pCell->num_render_polys) && !bRenderSky; j++)
         if ((!pCell->render_list[j].texture_id) ||
             ((pCell->render_list[j].texture_id >= BACKHACK_IDX) &&
              (pCell->render_list[j].texture_id < TEXMEM_RESERVED_LIMIT)))
            bRenderSky = TRUE;
   }

#ifdef STATS_ON
   stat_num_traverse_ms = portal_get_time() - stat_num_traverse_ms;
#endif

   CELL_DEBUG(debug_cell_traversal = FALSE);
   END_PROF;
}

int last_r_count;
bool project_space=TRUE;

#ifdef STATS_ON
static void zero_stats(void);
#endif

extern void PortalMovePointInsideCell(Location *loc);
static Position adjusted;

static void portal_start_3d(Position *pos)
{
   adjusted = *pos;
   pos = &adjusted;
   if (pos->loc.cell != CELL_INVALID)
       PortalMovePointInsideCell(&pos->loc);

   r3_start_frame();
   r3_set_view_angles(&pos->loc.vec, &pos->fac, R3_DEFANG);
   if (project_space)
      r3_set_space(R3_PROJECT_SPACE);
   else
      r3_set_space(R3_CLIPPING_SPACE);
   portal_camera_loc = pos->loc.vec;
}

static void portal_end_3d(void)
{
   r3_end_frame();
}


/* ----- /-/-/-/-/-/-/-/-/ <<< (((((( /\ )))))) >>> \-\-\-\-\-\-\-\-\ ----- *\

   It's possible this wants to be in the host program, since all we're
   doing is calling two external functions using the time we get from
   another external function.  On the other hand, if there's every
   any reason to call these before or after anything else in Portal,
   we'll be able to.

\* ----- \-\-\-\-\-\-\-\-\ <<< (((((( \/ )))))) >>> /-/-/-/-/-/-/-/-/ ----- */
static void update_terrain_animation(void)
{
   long delta_time_ms = portal_get_frame_time();

   if (portal_anim_medium_callback)
      (*portal_anim_medium_callback)(delta_time_ms);

   if (portal_anim_light_callback)
      (*portal_anim_light_callback)(delta_time_ms);
}


/* ----- /-/-/-/-/-/-/-/-/ <<< (((((( /\ )))))) >>> \-\-\-\-\-\-\-\-\ ----- *\

   The visualization tools need to render back-to-front.

\* ----- \-\-\-\-\-\-\-\-\ <<< (((((( \/ )))))) >>> /-/-/-/-/-/-/-/-/ ----- */
bool portal_render_back_to_front()
{
   bool front_to_back;

#ifndef SHIP
   if (g2pt_span_clip && !(draw_solid_by_MIP_level
                         | draw_solid_by_cell
                         | draw_solid_wireframe))
      front_to_back = TRUE;
   else
      front_to_back = FALSE;
#else  // ~SHIP
   front_to_back = g2pt_span_clip;
#endif // ~SHIP
   return !(front_to_back ^ render_backward);
}


void draw_lightmap_points(int);
bool show_lightmap;
extern int cache_mem_this_frame;


// When a leaf is going to be rendered, we mark it and its
//  ancestors so that when we traverse the BSP tree, we can
//  cull out any subtrees that have no leaves to be rendered.
void markup_bsp(uint node_index)
{
   uint parent_index = wrParentIndex(&g_wrBspTree[node_index]);

   wrBspMark(&g_wrBspTree[node_index]);

   while ((parent_index != WRBSP_INVALID) && !wrBspIsMarked(&g_wrBspTree[parent_index]))
   {
      node_index = parent_index;
      parent_index = wrParentIndex(&g_wrBspTree[node_index]);

      wrBspMark(&g_wrBspTree[node_index]);
   }
}

// Traverse the tree, unmarking all nodes and leaves.
void unmark_bsp(uint node_index)
{
   wrBspNode *pCurNode = &g_wrBspTree[node_index];

   if (!wrBspIsMarked(pCurNode))
      return;

   wrBspUnmark(pCurNode);

   if (!wrBspIsLeaf(pCurNode))
   {
      if (wrInsideIndex(pCurNode) != WRBSP_INVALID)
         unmark_bsp(wrInsideIndex(pCurNode));

      if (wrOutsideIndex(pCurNode) != WRBSP_INVALID)
         unmark_bsp(wrOutsideIndex(pCurNode));
   }
}

// First unmark any leaves and nodes that were marked, then
//  mark all the to-be-rendered leaves and nodes.
void setup_bsp()
{
   int i;

   unmark_bsp(0);

   for (i=0; i<r_sorted_count; ++i)
      markup_bsp(WR_CELL(active_regions[i])->leaf_index);
}

// rebuild active_regions so it's in front-to-back BSP sorted order
int sort_via_bsp(uint node_index, mxs_vector *pos, int index)
{
   wrBspNode *pCurNode = &g_wrBspTree[node_index];

   // If it's a node, traverse in the proper order and unmark it
   if (!wrBspIsLeaf(pCurNode))
   {
      BOOL outside_first;

      if (wrBspIsReversed(pCurNode))
      {
         if ((-mx_dot_vec(&pCurNode->plane->normal, pos) - pCurNode->plane->plane_constant) < 0)
            outside_first = TRUE;
         else
            outside_first = FALSE;
      }
      else
      {
         if ((mx_dot_vec(&pCurNode->plane->normal, pos) + pCurNode->plane->plane_constant) < 0)
            outside_first = TRUE;
         else
            outside_first = FALSE;
      }

      if (outside_first)
      {
         if ((wrOutsideIndex(pCurNode) != WRBSP_INVALID) && wrBspIsMarked(&g_wrBspTree[wrOutsideIndex(pCurNode)]))
            index = sort_via_bsp(wrOutsideIndex(pCurNode), pos, index);
         if ((wrInsideIndex(pCurNode) != WRBSP_INVALID) && wrBspIsMarked(&g_wrBspTree[wrInsideIndex(pCurNode)]))
            index = sort_via_bsp(wrInsideIndex(pCurNode), pos, index);
      }
      else
      {
         if ((wrInsideIndex(pCurNode) != WRBSP_INVALID) && wrBspIsMarked(&g_wrBspTree[wrInsideIndex(pCurNode)]))
            index = sort_via_bsp(wrInsideIndex(pCurNode), pos, index);
         if ((wrOutsideIndex(pCurNode) != WRBSP_INVALID) && wrBspIsMarked(&g_wrBspTree[wrOutsideIndex(pCurNode)]))
            index = sort_via_bsp(wrOutsideIndex(pCurNode), pos, index);
      }
   // Otherwise it's a leaf, so make it has been marked
   }
   else
   if (wrBspIsMarked(pCurNode))
   {
      active_regions[index] = pCurNode->cell_id;
      return index + 1;
   }
   return index;
}

// set bitfield flags for those cells which are visible
static void _portal_mark_visible_cells()
{
   int i, cell;
   for (i = 0; i < r_sorted_count; ++i) {
      cell = active_regions[i];
      portal_cell_rendered[cell >> 3] |= (1 << (cell & 7));
   }
}


// clear our array which indicates which cells are visible--we rely
// on the app to call this once per frame since we may draw more than
// one scene in a frame
void portal_unmark_visible_cells()
{
   memset(portal_cell_rendered, 0, 1 + MAX_REGIONS / 8);
}


// check whether any cell in a list has been marked as visible
static bool _portal_cell_list_visible(int *cell_list, int num_cells)
{
   int *cell = cell_list;
   int *end = cell + num_cells;

   while (cell != end) {
      if (portal_cell_visible(*cell))
         return TRUE;
      ++cell;
   }

   return FALSE;
}


bool portal_write_traversal;
bool portal_obj_fixup = TRUE;

void (*portal_post_render_cback)(void);
void (*portal_render_overlays_cback)(void);
extern int num_water_polys;
extern int portal_hack_blend;
extern bool background_setup;
extern bool punt_hardware_lighting;
extern void portal_do_blendtest(void);
extern BOOL g_zbuffer;

extern BOOL portal_multitexture;

BOOL portal_test_blend;
BOOL portal_true_two_pass = TRUE;

BOOL (*pSkyRenderCallback)(void) = 0;
BOOL bRenderNewSky = FALSE;

/* ----- The level's surfaces for the sound (acoustics.cpp) ---------------- *\

   Steam Audio traces sound through the level's own walls: every polygon of
   every cell, but the sky's (sound goes out there) and the plain openings
   between cells. Water surfaces are given too (flag 1): sound bounces off
   them. Vertices in feet, the world's axes; texture: the polygon's
   r3s_texture (its name tells what the surface is made of), or NULL.
   Returns a number that changes when the cells do; with no fn, only that.

\* ----------------------------------------------------------------------- */
unsigned portal_sound_world(void (*fn)(void *user, int n, const float *xyz,
                                       void *texture, int flags), void *user)
{
   unsigned serial = 2166136261u;
   int c;
   serial = (serial ^ (unsigned)wr_num_cells) * 16777619u;
   for (c = 0; c < wr_num_cells; ++c) {
      PortalCell *cell = wr_cell[c];
      serial = (serial ^ (unsigned)(uintptr_t)cell) * 16777619u;
      if (cell)
         serial = (serial ^ (unsigned)(cell->num_polys | (cell->num_vertices << 8))) * 16777619u;
   }
   if (!fn)
      return serial;
   for (c = 0; c < wr_num_cells; ++c) {
      PortalCell *r = WR_CELL(c);
      int p, voff = 0, first_portal;
      if (!r)
         continue;
      first_portal = r->num_polys - r->num_portal_polys;
      for (p = 0; p < r->num_render_polys; voff += r->poly_list[p].num_vertices, ++p) {
         float xyz[3 * 256];
         const int n = r->poly_list[p].num_vertices;
         const int tid = r->render_list[p].texture_id;
         const int water = p >= first_portal;
         int i;
         if (n < 3 || n > 256)
            continue;
         if (!water && (tid == 0 || (tid >= BACKHACK_IDX && tid < TEXMEM_RESERVED_LIMIT)))
            continue;   /* the sky */
         for (i = 0; i < n; ++i) {
            const mxs_vector *w = &r->vpool[r->vertex_list[voff + i]];
            xyz[i*3] = w->x; xyz[i*3+1] = w->y; xyz[i*3+2] = w->z;
         }
         fn(user, n, xyz, (tid > 0 && tid < BACKHACK_IDX && portal_get_texture) ? (void *)portal_get_texture(tid) : NULL,
            water ? 1 : 0);
      }
   }
   return serial;
}

#ifdef DARK_RT
/* ----- Ray tracer (RTGL1) feeds ----------------------------------------- *\

   The path tracer needs the whole level, not just the cells this view
   traverses, so the world representation is handed over complete whenever
   the mission's cells change.  Texture ids are resolved to the backend
   texture showing for them every frame, which keeps animated textures
   animated.  The view's own surface drawing is suppressed from capture;
   objects drawn by the portal renderer are captured by lgd3d.

\* ----------------------------------------------------------------------- */
#include <render_rt.h>
#include <stdlib.h>
extern void *lgd3d_rt_texture_for_bitmap(grs_bitmap *bm);
extern void *RenderBackendCreateTexture(int width, int height, const void *rgba, int rowBytes);
extern void *portal_rt_water_texture(r3s_texture tex);
extern float hw_water_tex_scale;
extern int GetWaterBankForFlow(int idx);             // h2ocolor.cpp
extern const void *GetWaterColorForBank(int bank);   // (an sRGBA: its rgb bytes first)
extern void objlight_rt_submit(void);

static bool portal_rt_is_sky(int texture_id)
{
   return texture_id == 0 ||
      (texture_id >= BACKHACK_IDX && texture_id < TEXMEM_RESERVED_LIMIT);
}

static uint32 portal_rt_world_serial(void)
{
   uint32 serial = 2166136261u;
   int i;
   serial = (serial ^ (uint32)wr_num_cells) * 16777619u;
   for (i = 0; i < wr_num_cells; ++i) {
      PortalCell *cell = wr_cell[i];
      serial = (serial ^ (uint32)(uintptr_t)cell) * 16777619u;
      if (cell)
         serial = (serial ^ (uint32)(cell->num_polys | (cell->num_vertices << 8))) * 16777619u;
   }
   return serial;
}

#define PORTAL_RT_MAX_TEXTURE_IDS 65536
static unsigned char portal_rt_texture_seen[PORTAL_RT_MAX_TEXTURE_IDS];
static int portal_rt_max_texture_id = -1;

// The mission's old textured sky (the sky mode "textures", no enhanced sky):
// the background cube round the camera (portbg.c) with the sky's six faces
// (texture ids 250-255: top, bottom, then the four round), mapped as the
// hardware renderer maps them (ptdrawhw.c), for the path tracer's sky.
// Every frame (their textures can change), only in that mode: in the star
// mode (portal_sky_id is the star hack's) the stars are drawn instead.
static void portal_rt_submit_skybox(void)
{
#define PORTAL_RT_BG_DIST 1200.0f
   static const signed char corner[8][3] = {
      { 1, 1, 1}, {-1, 1, 1}, {-1,-1, 1}, { 1,-1, 1},
      { 1, 1,-1}, {-1, 1,-1}, {-1,-1,-1}, { 1,-1,-1}
   };
   extern uchar background_vertex_list[24];
   extern PortalPolygonRenderInfo background_render_list[6];
   extern BOOL SkyRendererUseEnhanced(void);
   int f, i;
   // (the enhanced sky sets the textures mode too, but draws itself)
   if (portal_sky_id == BACKHACK_IDX || SkyRendererUseEnhanced())
      return;
   for (f = 0; f < 6; ++f) {
      PortalPolygonRenderInfo *ri = &background_render_list[f];
      r3s_texture tex = portal_get_texture(ri->texture_id);
      mxs_vector v[4], *anchor;
      mxs_real u2, v2, t[8], umin, vmin, w, h;
      float xyz[12], uv[8];
      if (!tex || tex->w <= 0 || tex->h <= 0)
         continue;
      for (i = 0; i < 4; ++i) {
         const signed char *k = corner[background_vertex_list[f * 4 + i]];
         v[i].x = k[0] * PORTAL_RT_BG_DIST;
         v[i].y = k[1] * PORTAL_RT_BG_DIST;
         v[i].z = k[2] * PORTAL_RT_BG_DIST;
      }
      anchor = &v[ri->texture_anchor < 4 ? ri->texture_anchor : 0];
      u2 = mx_mag2_vec(&ri->tex_u);
      v2 = mx_mag2_vec(&ri->tex_v);
      if (u2 <= 0.0f || v2 <= 0.0f)
         continue;
      // As ptdrawhw.c maps the sky: the face is the texture once (not in
      // 64-texel units, as a wall), each way from half a texel in from its
      // edge to half a texel in from the other. Offset by whole repeats to
      // 0..1 (the cube's own offsets put some faces at -1..0).
      umin = vmin = 1e9f;
      for (i = 0; i < 4; ++i) {
         mxs_vector delta;
         mx_sub_vec(&delta, &v[i], anchor);
         t[i*2]   = mx_dot_vec(&delta, &ri->tex_u) / u2 + ri->u_base / 4096.0f;
         t[i*2+1] = mx_dot_vec(&delta, &ri->tex_v) / v2 + ri->v_base / 4096.0f;
         if (t[i*2] < umin) umin = t[i*2];
         if (t[i*2+1] < vmin) vmin = t[i*2+1];
      }
      umin = (mxs_real)floor(umin + 0.5f);
      vmin = (mxs_real)floor(vmin + 0.5f);
      w = (mxs_real)tex->w;
      h = (mxs_real)tex->h;
      for (i = 0; i < 4; ++i) {
         xyz[i*3] = v[i].x; xyz[i*3+1] = v[i].y; xyz[i*3+2] = v[i].z;
         uv[i*2]   = (t[i*2] - umin) * (w - 1.0f) / w + 0.5f / w;
         uv[i*2+1] = (t[i*2+1] - vmin) * (h - 1.0f) / h + 0.5f / h;
      }
      RenderRTSkyBoxFace(f, lgd3d_rt_texture_for_bitmap(tex), xyz, uv);
   }
#undef PORTAL_RT_BG_DIST
}

// ---- The game's own light, for the path tracer's ambient glow ----------------
// Each lit polygon's lightmap -- the light the game baked for it, the light the
// light gem goes by -- packed into pages (rtgl1.cpp, RenderRTLightmapRect), and
// where it lies there given with the polygon (RenderRTWorldPolygonLight): the
// path tracer's ambient glow follows it, dimmer and cooler where the game has
// it dark. Lightmaps lit by animated lights (switches, torches put out, lamps
// that flicker) are made again when those lights change much.
typedef struct { int cell; short poly; uchar page; ushort x, y; } PortalRtLm;
static PortalRtLm *portal_rt_lm_anim;
static int portal_rt_lm_anim_n, portal_rt_lm_anim_cap;
static uchar *portal_rt_lm_used, *portal_rt_lm_changed;   // the anim lights' brightness, as made
static int portal_rt_lm_used_n;
static int portal_rt_lm_page, portal_rt_lm_x, portal_rt_lm_y, portal_rt_lm_shelf, portal_rt_lm_frame;
static int portal_rt_lm_count, portal_rt_lm_texels;
static int portal_rt_lm_hist[16];   // the lightmaps' brightness (luma 0..255, by 16), for the log

static void portal_rt_lm_reset(void)
{
   portal_rt_lm_page = portal_rt_lm_x = portal_rt_lm_y = portal_rt_lm_shelf = 0;
   portal_rt_lm_anim_n = portal_rt_lm_count = portal_rt_lm_texels = 0;
   memset(portal_rt_lm_hist, 0, sizeof(portal_rt_lm_hist));
   if (portal_anim_light_intensity && num_anim_lights > 0) {
      portal_rt_lm_used = (uchar *)realloc(portal_rt_lm_used, num_anim_lights);
      portal_rt_lm_changed = (uchar *)realloc(portal_rt_lm_changed, num_anim_lights);
      if (portal_rt_lm_used && portal_rt_lm_changed) {
         memcpy(portal_rt_lm_used, portal_anim_light_intensity, num_anim_lights);
         portal_rt_lm_used_n = num_anim_lights;
      } else
         portal_rt_lm_used_n = 0;
   } else
      portal_rt_lm_used_n = 0;
}

// room for a w x h rectangle: its page, or -1 (all pages full)
static int portal_rt_lm_alloc(int w, int h, int *px, int *py)
{
   const int P = RENDER_RT_LIGHTMAP_PAGE;
   if (w > P || h > P)
      return -1;
   if (portal_rt_lm_x + w > P) {
      portal_rt_lm_x = 0;
      portal_rt_lm_y += portal_rt_lm_shelf;
      portal_rt_lm_shelf = 0;
   }
   if (portal_rt_lm_y + h > P) {
      ++portal_rt_lm_page;
      portal_rt_lm_x = portal_rt_lm_y = portal_rt_lm_shelf = 0;
   }
   if (portal_rt_lm_page >= RENDER_RT_LIGHTMAP_PAGES)
      return -1;
   *px = portal_rt_lm_x;
   *py = portal_rt_lm_y;
   portal_rt_lm_x += w;
   if (h > portal_rt_lm_shelf)
      portal_rt_lm_shelf = h;
   return portal_rt_lm_page;
}

#if defined(RGB_LIGHTING) && !defined(RGB_888)
#define PORTAL_RT_LM_MAX 31
#define PORTAL_RT_LM_GET(e, c) ((c) == 0 ? ((e) & 0x1f) : (c) == 1 ? (((e) >> 5) & 0x1f) : (((e) >> 10) & 0x1f))
#elif defined(RGB_LIGHTING)
#define PORTAL_RT_LM_MAX 255
#define PORTAL_RT_LM_GET(e, c) ((e).A[c])
#else
#define PORTAL_RT_LM_MAX 255
#define PORTAL_RT_LM_GET(e, c) (e)
#endif

// A polygon's lightmap as the game has it now (its static light and its
// animated lights at their present brightness, as compute_lightmap adds them),
// RGBA, (w+2) x (h+2): a border of its edge texels, so filtering at its edges
// takes nothing from its neighbours on the page.
static void portal_rt_lm_pixels(PortalCell *r, PortalLightMap *lt, uchar *out)
{
   static int acc[64 * 64 * 3];
   const int w = lt->w, h = lt->h, W = w + 2, row = lt->pixel_row, size = lt->h * lt->pixel_row;
   LightmapEntry *bits = lt->data;
   int x, y, k;
   uint mask = lt->anim_light_bitmask;
   ushort *idx = r->anim_light_index_list;
   for (y = 0; y < h; ++y)
      for (x = 0; x < w; ++x)
         for (k = 0; k < 3; ++k)
            acc[(y * w + x) * 3 + k] = PORTAL_RT_LM_GET(bits[y * row + x], k);
   while (mask && idx && portal_anim_light_intensity) {
      if (mask & 1) {
         const int inten = portal_anim_light_intensity[*idx];
         bits += size;
         if (inten > 0)
            for (y = 0; y < h; ++y)
               for (x = 0; x < w; ++x)
                  for (k = 0; k < 3; ++k)
                     acc[(y * w + x) * 3 + k] += (PORTAL_RT_LM_GET(bits[y * row + x], k) * inten) >> 8;
      }
      ++idx;
      mask >>= 1;
   }
   for (y = -1; y <= h; ++y)
      for (x = -1; x <= w; ++x) {
         const int sx = x < 0 ? 0 : x >= w ? w - 1 : x, sy = y < 0 ? 0 : y >= h ? h - 1 : y;
         uchar *o = out + ((y + 1) * W + (x + 1)) * 4;
         for (k = 0; k < 3; ++k) {
            int v = acc[(sy * w + sx) * 3 + k];
            if (v > PORTAL_RT_LM_MAX)
               v = PORTAL_RT_LM_MAX;
            o[k] = (uchar)(v * 255 / PORTAL_RT_LM_MAX);
         }
         o[3] = 255;
      }
}

// the polygon's lightmap onto a page, and where it lies there for each corner
// (the lightmap-only pass's mapping, ptdrawhw.c: a lightmap texel every quarter
// of a texture unit, from its base); 0 if it has none (or no room is left)
static int portal_rt_lm_polygon(int c, int p, PortalCell *r, PortalPolygonRenderInfo *ri,
                                const mxs_vector *anchor, int voff, int n, float *lmuv, int *page)
{
   static uchar pixels[66 * 66 * 4];
   PortalLightMap *lt;
   mxs_real uvdot, u2, v2, tbu, tbv;
   int i, x, y, pg;
   if (!r->light_list || p >= r->num_render_polys)
      return 0;
   lt = &r->light_list[p];
   if (!lt->data || lt->w == 0 || lt->h == 0 || lt->w > 64 || lt->h > 64)
      return 0;
   uvdot = mx_dot_vec(&ri->tex_u, &ri->tex_v);
   u2 = mx_mag2_vec(&ri->tex_u);
   v2 = mx_mag2_vec(&ri->tex_v);
   if (u2 <= 0.0f || v2 <= 0.0f)
      return 0;
   pg = portal_rt_lm_alloc(lt->w + 2, lt->h + 2, &x, &y);
   if (pg < 0)
      return 0;
   portal_rt_lm_pixels(r, lt, pixels);
   RenderRTLightmapRect(pg, x, y, lt->w + 2, lt->h + 2, pixels);
   {
      int tx, ty;
      for (ty = 1; ty <= lt->h; ++ty)
         for (tx = 1; tx <= lt->w; ++tx) {
            const uchar *o = pixels + (ty * (lt->w + 2) + tx) * 4;
            const int l = (o[0] * 54 + o[1] * 183 + o[2] * 19) >> 8;
            ++portal_rt_lm_hist[l >> 4];
         }
   }
   ++portal_rt_lm_count;
   portal_rt_lm_texels += (lt->w + 2) * (lt->h + 2);
   if (lt->anim_light_bitmask) {
      if (portal_rt_lm_anim_n >= portal_rt_lm_anim_cap) {
         int cap = portal_rt_lm_anim_cap ? portal_rt_lm_anim_cap * 2 : 1024;
         PortalRtLm *grown = (PortalRtLm *)realloc(portal_rt_lm_anim, cap * sizeof(PortalRtLm));
         if (grown) {
            portal_rt_lm_anim = grown;
            portal_rt_lm_anim_cap = cap;
         }
      }
      if (portal_rt_lm_anim_n < portal_rt_lm_anim_cap) {
         PortalRtLm *a = &portal_rt_lm_anim[portal_rt_lm_anim_n++];
         a->cell = c;
         a->poly = (short)p;
         a->page = (uchar)pg;
         a->x = (ushort)x;
         a->y = (ushort)y;
      }
   }
   tbu = portal_wrext_u_base[c] ? portal_wrext_u_base[c][p] : ri->u_base / 4096.0f;
   tbv = portal_wrext_v_base[c] ? portal_wrext_v_base[c][p] : ri->v_base / 4096.0f;
   for (i = 0; i < n; ++i) {
      mxs_vector *w = &r->vpool[r->vertex_list[voff + i]];
      mxs_vector delta;
      mxs_real du, dv, U, V;
      mx_sub_vec(&delta, w, (mxs_vector *)anchor);
      du = mx_dot_vec(&delta, &ri->tex_u);
      dv = mx_dot_vec(&delta, &ri->tex_v);
      if (uvdot == 0.0f) {
         U = tbu + du / u2;
         V = tbv + dv / v2;
      } else {
         const mxs_real denom = 1.0f / (u2 * v2 - uvdot * uvdot);
         U = tbu + v2 * denom * du - denom * uvdot * dv;
         V = tbv + u2 * denom * dv - denom * uvdot * du;
      }
      lmuv[i * 2] = (x + 1.5f + 4.0f * U - lt->base_u) / RENDER_RT_LIGHTMAP_PAGE;
      lmuv[i * 2 + 1] = (y + 1.5f + 4.0f * V - lt->base_v) / RENDER_RT_LIGHTMAP_PAGE;
   }
   *page = pg;
   return 1;
}

// every few frames: the lightmaps lit by animated lights that have changed
// much since they were made (put out, switched, a lamp's flicker swinging far)
static void portal_rt_lm_update(void)
{
   static uchar pixels[66 * 66 * 4];
   int i, any = 0;
   if (portal_rt_lm_anim_n <= 0 || !portal_anim_light_intensity ||
       portal_rt_lm_used_n != num_anim_lights || (++portal_rt_lm_frame & 7))
      return;
   for (i = 0; i < num_anim_lights; ++i) {
      const int a = portal_rt_lm_used[i], b = portal_anim_light_intensity[i];
      const int d = a > b ? a - b : b - a;
      portal_rt_lm_changed[i] = (uchar)(((a == 0) != (b == 0)) || d > 64);
      if (portal_rt_lm_changed[i]) {
         portal_rt_lm_used[i] = (uchar)b;
         any = 1;
      }
   }
   if (!any)
      return;
   for (i = 0; i < portal_rt_lm_anim_n; ++i) {
      const PortalRtLm *a = &portal_rt_lm_anim[i];
      PortalCell *r = (a->cell >= 0 && a->cell < wr_num_cells) ? WR_CELL(a->cell) : NULL;
      PortalLightMap *lt;
      uint mask;
      ushort *idx;
      int hit = 0;
      if (!r || !r->light_list || !r->anim_light_index_list)
         continue;
      lt = &r->light_list[a->poly];
      for (mask = lt->anim_light_bitmask, idx = r->anim_light_index_list; mask && !hit; mask >>= 1, ++idx)
         if ((mask & 1) && *idx < num_anim_lights && portal_rt_lm_changed[*idx])
            hit = 1;
      if (!hit)
         continue;
      portal_rt_lm_pixels(r, lt, pixels);
      RenderRTLightmapRect(a->page, a->x, a->y, lt->w + 2, lt->h + 2, pixels);
   }
}

// A water surface with no texture where the mission has no water texture at
// all (Black Parade's canals and falls: the game draws them as nothing) is
// given one of the mission's own, unused: its green hardware water
// (fam/waterhw/GRin), slowly scrolling in the path tracer -- not what the
// author drew, but it reads as water. The ray tracer's texture, made from
// the PCX once.
#define PORTAL_RT_STAND_IN_WATER 0xffff
static void *portal_rt_stand_in_water;
static int portal_rt_stand_in_tried;

static void *portal_rt_load_pcx(const char *path)
{
   FILE *f = fopen(path, "rb");
   unsigned char head[128], *data = NULL, *rgba = NULL;
   long size;
   int w, h, bpl, x, y, at;
   void *handle = NULL;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size < 128 + 769 || fread(head, 1, 128, f) != 128 || head[0] != 10 || head[2] != 1 ||
       head[3] != 8 || head[65] != 1) {
      fclose(f);
      return NULL;
   }
   w = (head[8] | (head[9] << 8)) - (head[4] | (head[5] << 8)) + 1;
   h = (head[10] | (head[11] << 8)) - (head[6] | (head[7] << 8)) + 1;
   bpl = head[66] | (head[67] << 8);
   data = (unsigned char *)malloc(size - 128);
   if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || bpl < w || !data ||
       fread(data, 1, size - 128, f) != (size_t)(size - 128) || data[size - 128 - 769] != 12) {
      free(data);
      fclose(f);
      return NULL;
   }
   fclose(f);
   rgba = (unsigned char *)malloc(w * h * 4);
   if (rgba) {
      const unsigned char *pal = data + (size - 128 - 768);
      const long end = size - 128 - 769;
      at = 0;
      for (y = 0; y < h; ++y) {
         x = 0;
         while (x < bpl && at < end) {
            int run = 1, v = data[at++];
            if ((v & 0xc0) == 0xc0 && at < end) {
               run = v & 0x3f;
               v = data[at++];
            }
            while (run-- > 0 && x < bpl) {
               if (x < w) {
                  unsigned char *d = rgba + (y * w + x) * 4;
                  d[0] = pal[v * 3];
                  d[1] = pal[v * 3 + 1];
                  d[2] = pal[v * 3 + 2];
                  d[3] = 255;
               }
               ++x;
            }
         }
      }
      handle = RenderBackendCreateTexture(w, h, rgba, w * 4);
      mprintf("RT: untextured water: %s (%dx%d) in its place\n", path, w, h);
   }
   free(rgba);
   free(data);
   return handle;
}

static void *portal_rt_stand_in_water_texture(void)
{
   static const char *const names[] = {"fam/waterhw/GRin.PCX", "fam/waterhw/grin.pcx", "fam/waterhw/GRIN.PCX"};
   const char *fm = getenv("DARK_FM_PATH");
   int i;
   if (portal_rt_stand_in_tried)
      return portal_rt_stand_in_water;
   portal_rt_stand_in_tried = 1;
   for (i = 0; fm && fm[0] && i < (int)(sizeof(names) / sizeof(names[0])) && !portal_rt_stand_in_water; ++i) {
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", fm, names[i]);
      portal_rt_stand_in_water = portal_rt_load_pcx(path);
   }
   return portal_rt_stand_in_water;
}

static void portal_rt_submit_world(void)
{
   uint32 serial;
   int c;

   if (wr_num_cells <= 0)
      return;
   serial = portal_rt_world_serial();
   if (RenderRTWorldWanted(serial)) {
      float xyz[3 * 256], uv[2 * 256];
      // A water surface with no texture (texture 0, as Black Parade's
      // canals) borrows the mission's commonest water texture, drawn as the
      // rest of its water is (through its medium's palette).
      static unsigned short water_count[PORTAL_RT_MAX_TEXTURE_IDS];
      int water_tex = -1;
      memset(water_count, 0, sizeof(water_count));
      for (c = 0; c < wr_num_cells; ++c) {
         PortalCell *r = WR_CELL(c);
         int p;
         if (!r)
            continue;
         for (p = r->num_polys - r->num_portal_polys; p < r->num_render_polys; ++p) {
            const int t = r->render_list[p].texture_id;
            if (!portal_rt_is_sky(t) && t < PORTAL_RT_MAX_TEXTURE_IDS && portal_get_texture(t) &&
                water_count[t] < 65535) {
               ++water_count[t];
               if (water_tex < 0 || water_count[t] > water_count[water_tex])
                  water_tex = t;
            }
         }
      }
      // Each water cell's surface: the height of its openings onto the air
      // above it (or, a cell deep under, of the water cells over it) -- the
      // depth of what is under water, which the path tracer hazes by it,
      // the water's own surface being drawn only where it has a texture
      // (as the game draws it: an untextured one not at all)
      static float *water_top = NULL;
      static int water_top_n = 0;
      if (water_top_n < wr_num_cells) {
         free(water_top);
         water_top = (float *)malloc(sizeof(float) * (wr_num_cells > 0 ? wr_num_cells : 1));
         water_top_n = water_top ? wr_num_cells : 0;
      }
      if (water_top) {
         int pass, any = 0;
         for (c = 0; c < wr_num_cells; ++c) {
            PortalCell *r = WR_CELL(c);
            int p, voff = 0;
            water_top[c] = -1e30f;
            if (!r || r->medium != 2)
               continue;
            ++any;
            for (p = 0; p < r->num_polys; voff += r->poly_list[p].num_vertices, ++p) {
               PortalPolygonCore *pc = &r->poly_list[p];
               PortalCell *d;
               float z = 0;
               int i;
               if (p < r->num_polys - r->num_portal_polys || pc->destination >= wr_num_cells)
                  continue;
               d = WR_CELL(pc->destination);
               if (!d || d->medium == 2 || pc->num_vertices < 3)
                  continue;
               for (i = 0; i < pc->num_vertices; ++i)
                  z += r->vpool[r->vertex_list[voff + i]].z;
               z /= pc->num_vertices;
               if (z > water_top[c])
                  water_top[c] = z;
            }
         }
         if (any) {
            // the water's colour (the mission's, as the game hazes it with)
            for (c = 0; c < wr_num_cells; ++c) {
               PortalCell *r = WR_CELL(c);
               if (r && r->medium == 2) {
                  const unsigned char *rgb =
                     (const unsigned char *)GetWaterColorForBank(GetWaterBankForFlow(r->motion_index));
                  if (rgb)
                     RenderRTWorldWaterColor(rgb[0], rgb[1], rgb[2]);
                  break;
               }
            }
         }
         for (pass = 0; pass < 16 && any; ++pass) {
            int changed = 0;
            for (c = 0; c < wr_num_cells; ++c) {
               PortalCell *r = WR_CELL(c);
               int p;
               if (!r || r->medium != 2)
                  continue;
               for (p = r->num_polys - r->num_portal_polys; p < r->num_polys; ++p) {
                  const int d = r->poly_list[p].destination;
                  if (d < wr_num_cells && WR_CELL(d) && WR_CELL(d)->medium == 2 &&
                      water_top[d] > water_top[c]) {
                     water_top[c] = water_top[d];
                     changed = 1;
                  }
               }
            }
            if (!changed)
               break;
         }
      }
      const int game_light = RenderRTLightmapWanted();
      memset(portal_rt_texture_seen, 0, sizeof(portal_rt_texture_seen));
      portal_rt_max_texture_id = -1;
      RenderRTWorldBegin(serial);
      portal_rt_lm_reset();
      for (c = 0; c < wr_num_cells; ++c) {
         PortalCell *r = WR_CELL(c);
         int p, voff = 0;
         int first_portal;
         if (!r)
            continue;
         first_portal = r->num_polys - r->num_portal_polys;
         RenderRTWorldCell(c);
         for (p = 0; p < r->num_render_polys; voff += r->poly_list[p].num_vertices, ++p) {
            PortalPolygonCore *pc = &r->poly_list[p];
            PortalPolygonRenderInfo *ri = &r->render_list[p];
            int n = pc->num_vertices, i, flags = 0;
            const mxs_vector *anchor;
            r3s_texture tex;
            mxs_real u_scale = 1.0f, v_scale = 1.0f, u_base, v_base;
            mxs_real uvdot, u2, v2;
            int tid = ri->texture_id;
            if (n < 3 || n > 256)
               continue;
            int stand_in = 0;
            if (p >= first_portal && portal_rt_is_sky(tid) && water_tex >= 0)
               tid = water_tex;
            else if (p >= first_portal && portal_rt_is_sky(tid) && portal_rt_stand_in_water_texture())
               stand_in = 1;   // (given as PORTAL_RT_STAND_IN_WATER, below)
            // (a water surface -- a medium boundary -- is never a sky, though
            // its texture number may be 0: Black Parade's canals, drawn as
            // openings onto the sky, invisible. Without a texture of its own,
            // the path tracer's water.)
            if (p >= first_portal)
               flags |= kRenderRTWorldWater;
            else if (portal_rt_is_sky(tid))
               flags |= kRenderRTWorldSky;
            if (pc->flags & RENDER_DOESNT_LIGHT)
               flags |= kRenderRTWorldFullbright;
            tex = ((flags & kRenderRTWorldSky) || portal_rt_is_sky(tid))
                     ? NULL : portal_get_texture(tid);
            if (tex) {
               int tw, th;
               if (ResImageTerrainScale(tex, &tw, &th)) {
                  // an HD replacement, mapped at its original's size (.mtl)
                  u_scale = 64.0f / tw;
                  v_scale = 64.0f / th;
               } else if (tex->type != BMT_FLAT8 && tex->w > 0 && tex->h > 0) {
                  // (wlog is of the row in bytes: twice the width at 16 bits)
                  u_scale = 64.0f / tex->w;
                  v_scale = 64.0f / tex->h;
               } else {
                  u_scale = (mxs_real)ldexp(1.0, 6 - tex->wlog);
                  v_scale = (mxs_real)ldexp(1.0, 6 - tex->hlog);
               }
               if (tex->flags & BMF_TRANS)
                  flags |= kRenderRTWorldTransparent;
            }
            u_scale /= portal_worldrep_texture_scale;
            v_scale /= portal_worldrep_texture_scale;
            u_base = (portal_wrext_u_base[c] ? portal_wrext_u_base[c][p]
                                             : ri->u_base / 4096.0f) * u_scale;
            v_base = (portal_wrext_v_base[c] ? portal_wrext_v_base[c][p]
                                             : ri->v_base / 4096.0f) * v_scale;
            anchor = &r->vpool[r->vertex_list[voff + ri->texture_anchor]];
            uvdot = mx_dot_vec(&ri->tex_u, &ri->tex_v);
            u2 = mx_mag2_vec(&ri->tex_u);
            v2 = mx_mag2_vec(&ri->tex_v);
            for (i = 0; i < n; ++i) {
               mxs_vector *w = &r->vpool[r->vertex_list[voff + i]];
               mxs_vector delta;
               mxs_real du, dv;
               xyz[i*3] = w->x; xyz[i*3+1] = w->y; xyz[i*3+2] = w->z;
               mx_sub_vec(&delta, w, (mxs_vector *)anchor);
               du = mx_dot_vec(&delta, &ri->tex_u);
               dv = mx_dot_vec(&delta, &ri->tex_v);
               if (u2 <= 0.0f || v2 <= 0.0f) {
                  // (no mapping of its own -- a borrowed water texture: laid
                  // flat, a tile every 4 feet)
                  uv[i*2] = (tid != ri->texture_id || stand_in) ? w->x * 0.25f : 0.0f;
                  uv[i*2+1] = (tid != ri->texture_id || stand_in) ? w->y * 0.25f : 0.0f;
               } else if (uvdot == 0.0f) {
                  uv[i*2]   = du * u_scale / u2 + u_base;
                  uv[i*2+1] = dv * v_scale / v2 + v_base;
               } else {
                  mxs_real denom = 1.0f / (u2*v2 - uvdot*uvdot);
                  uv[i*2]   = u_base + (v2*v_scale*denom) * du - (u_scale*denom*uvdot) * dv;
                  uv[i*2+1] = v_base + (u2*u_scale*denom) * dv - (v_scale*denom*uvdot) * du;
               }
            }
            if (flags & kRenderRTWorldWater)
               for (i = 0; i < 2 * n; ++i)
                  uv[i] *= hw_water_tex_scale;   // as ptdrawhw.c draws water
            {
               // the texture's tile_factor (its .mtl), as lgd3d draws it
               float su, sv;
               r3s_texture ttex = portal_get_texture(tid);
               if (ttex && ResImageMaterialTile(ttex, &su, &sv))
                  for (i = 0; i < n; ++i) {
                     uv[i*2] *= su;
                     uv[i*2+1] *= sv;
                  }
            }
            {
               const PortalPlane *pl = &r->plane_list[pc->planeid];
               RenderRTWorldPolygonFacing(pl->normal.x, pl->normal.y, pl->normal.z);
            }
            if (game_light && !(flags & (kRenderRTWorldSky | kRenderRTWorldWater | kRenderRTWorldFullbright)) &&
                !portal_rt_is_sky(tid)) {
               float lmuv[2 * 256];
               int page;
               if (portal_rt_lm_polygon(c, p, r, ri, anchor, voff, n, lmuv, &page))
                  RenderRTWorldPolygonLight(page, n, lmuv);
            }
            if (!(flags & kRenderRTWorldWater) && r->medium == 2 && water_tex < 0 && water_top &&
                water_top[c] > -1e29f) {
               // (how far under the water's surface it lies, in feet)
               float z = 0;
               for (i = 0; i < n; ++i)
                  z += xyz[i * 3 + 2];
               z /= n;
               if (water_top[c] - z > 0.0f)
                  RenderRTWorldPolygonUnderWater(water_top[c] - z);
            }
            if ((flags & kRenderRTWorldWater) && r->medium == 2)
               RenderRTWorldPolygonUnderside();
            RenderRTWorldPolygon(stand_in ? PORTAL_RT_STAND_IN_WATER : tid, flags, n, xyz, uv);
            if (!(flags & kRenderRTWorldSky) && !portal_rt_is_sky(tid) &&
                tid < PORTAL_RT_MAX_TEXTURE_IDS) {
               // 2: a water surface, drawn through its medium's palette
               portal_rt_texture_seen[tid] =
                  (flags & kRenderRTWorldWater) ? 2 : 1;
               if (tid > portal_rt_max_texture_id)
                  portal_rt_max_texture_id = tid;
            }
         }
      }
      RenderRTWorldEnd();
      if (game_light) {
         int total = 0, i, run = 0, pc = 0;
         static const int want[5] = {10, 25, 50, 75, 90};
         int at[5] = {0, 0, 0, 0, 0};
         mprintf("RT: the game's light: %d lightmaps on %d pages (%d%% of the last), %d lit by animated lights\n",
                 portal_rt_lm_count, portal_rt_lm_count ? portal_rt_lm_page + 1 : 0,
                 portal_rt_lm_count ? (portal_rt_lm_y + portal_rt_lm_shelf) * 100 / RENDER_RT_LIGHTMAP_PAGE : 0,
                 portal_rt_lm_anim_n);
         for (i = 0; i < 16; ++i)
            total += portal_rt_lm_hist[i];
         for (i = 0; i < 16 && total > 0; ++i) {
            run += portal_rt_lm_hist[i];
            while (pc < 5 && run * 100 >= want[pc] * total)
               at[pc++] = i * 16 + 15;
         }
         // (luma 0..255: the shader takes under ~10 as dark, over ~77 as lit)
         mprintf("RT: the game's light, how bright: 10%% of texels under %d, 25%% under %d, half under %d, 75%% under %d, 90%% under %d (0..255)\n",
                 at[0], at[1], at[2], at[3], at[4]);
      }
   }
   portal_rt_lm_update();

   // the current frame of every world texture
   for (c = 0; c <= portal_rt_max_texture_id; ++c) {
      if (portal_rt_texture_seen[c]) {
         r3s_texture tex = portal_get_texture(c);
         void *handle = NULL;
         if (tex && portal_rt_texture_seen[c] == 2)
            handle = portal_rt_water_texture(tex);
         if (tex && !handle)
            handle = lgd3d_rt_texture_for_bitmap(tex);
         RenderRTWorldTexture(c, handle);
      }
   }
   if (portal_rt_stand_in_water)
      RenderRTWorldTexture(PORTAL_RT_STAND_IN_WATER, portal_rt_stand_in_water);
   portal_rt_submit_skybox();
}

// Which static lights can matter for this view: the lights the level's
// lighting found shining on the cells around the camera (reached through
// portals, within a radius, in view or not: bounce light and shadows come from
// there too) and on the cells drawn last frame (a lit street seen far away).
// The ray tracer is given only these, so its few candidate lights per pixel
// are not spent on lamps behind the walls.
#define PORTAL_RT_LIGHT_BITS 65536
static unsigned char portal_rt_light_bits[PORTAL_RT_LIGHT_BITS / 8];
static unsigned int *portal_rt_cell_stamp;
static unsigned int *portal_rt_drawn;   // = portal_rt_stamp: drawn last frame
static int portal_rt_drawn_size;
static int portal_rt_cell_stamp_size;
static unsigned int portal_rt_stamp;

static int portal_rt_stat_cells, portal_rt_stat_entries, portal_rt_stat_max;

static void portal_rt_mark_cell_lights(int c)
{
   ushort *lt;
   int k, n;
   if (c < 0 || c >= wr_num_cells || !WR_CELL(c))
      return;
   // animated lights (torches that can be doused, switched lamps) are kept
   // in a list of their own, not in light_indices
   if (WR_CELL(c)->anim_light_index_list)
      for (k = 0; k < WR_CELL(c)->num_anim_lights; ++k) {
         const ushort l = WR_CELL(c)->anim_light_index_list[k];
         portal_rt_light_bits[l >> 3] |= (unsigned char)(1 << (l & 7));
      }
   lt = WR_CELL(c)->light_indices;
   if (!lt)
      return;
   n = lt[0];
   ++portal_rt_stat_cells;
   portal_rt_stat_entries += n;
   if (n > portal_rt_stat_max)
      portal_rt_stat_max = n;
   for (k = 1; k <= n; ++k)
      portal_rt_light_bits[lt[k] >> 3] |= (unsigned char)(1 << (lt[k] & 7));
}

static void portal_rt_find_lights(int start)
{
   static int *queue;
   static int queue_size;
   const float radius = 30.0f / 0.3048f;   // metres, in Dark units
   int head = 0, tail = 0, i, visible = 0;
   static int frames;
   memset(portal_rt_light_bits, 0, sizeof(portal_rt_light_bits));
   portal_rt_stat_cells = portal_rt_stat_entries = portal_rt_stat_max = 0;
   if (wr_num_cells <= 0)
      return;
   if (portal_rt_cell_stamp_size < wr_num_cells) {
      free(portal_rt_cell_stamp);
      free(queue);
      portal_rt_cell_stamp = (unsigned int *)calloc(wr_num_cells, sizeof(unsigned int));
      queue = (int *)malloc(sizeof(int) * wr_num_cells);
      portal_rt_cell_stamp_size = portal_rt_cell_stamp ? wr_num_cells : 0;
      queue_size = wr_num_cells;
      if (!portal_rt_cell_stamp || !queue)
         return;
   }
   if (portal_rt_drawn_size < wr_num_cells) {
      free(portal_rt_drawn);
      portal_rt_drawn = (unsigned int *)calloc(wr_num_cells, sizeof(unsigned int));
      portal_rt_drawn_size = portal_rt_drawn ? wr_num_cells : 0;
   }
   if (++portal_rt_stamp == 0) {
      if (portal_rt_drawn)
         memset(portal_rt_drawn, 0, sizeof(unsigned int) * portal_rt_drawn_size);
      memset(portal_rt_cell_stamp, 0, sizeof(unsigned int) * portal_rt_cell_stamp_size);
      portal_rt_stamp = 1;
   }
   for (i = 0; i < r_sorted_count && i < MAX_ACTIVE_REGIONS; ++i) {
      portal_rt_mark_cell_lights(active_regions[i]);
      if (active_regions[i] >= 0 && active_regions[i] < portal_rt_drawn_size)
         portal_rt_drawn[active_regions[i]] = portal_rt_stamp;
   }
   visible = portal_rt_stat_cells;
   if (start < 0 || start >= wr_num_cells || !WR_CELL(start))
      return;
   queue[tail++] = start;
   portal_rt_cell_stamp[start] = portal_rt_stamp;
   while (head < tail) {
      PortalCell *cell = WR_CELL(queue[head++]);
      PortalPolygonCore *portal = cell->portal_poly_list;
      int n = cell->num_portal_polys;
      portal_rt_mark_cell_lights(queue[head - 1]);
      while (n-- > 0) {
         const int d = portal->destination;
         ++portal;
         if (d < 0 || d >= wr_num_cells || portal_rt_cell_stamp[d] == portal_rt_stamp || !WR_CELL(d))
            continue;
         portal_rt_cell_stamp[d] = portal_rt_stamp;
         {
            const mxs_vector *c = &WR_CELL(d)->sphere_center;
            const float dx = c->x - portal_camera_loc.x, dy = c->y - portal_camera_loc.y,
                        dz = c->z - portal_camera_loc.z;
            const float r = radius + WR_CELL(d)->sphere_radius;
            if (dx * dx + dy * dy + dz * dz > r * r)
               continue;
         }
         if (tail < queue_size)
            queue[tail++] = d;
      }
   }
   if (++frames % 300 == 1) {
      int marked = 0;
      for (i = 0; i < PORTAL_RT_LIGHT_BITS; ++i)
         marked += (portal_rt_light_bits[i >> 3] >> (i & 7)) & 1;
      fprintf(stderr, "RT: light reach: camera cell %d of %d, %d drawn cells, %d cells within 30 m, "
                      "%d lights marked; cells list %.1f lights on average, at most %d\n",
              start, wr_num_cells, visible, tail, marked,
              portal_rt_stat_cells ? (double)portal_rt_stat_entries / portal_rt_stat_cells : 0.0,
              portal_rt_stat_max);
   }
}

// The distance from the camera to each cell walking through the openings
// between cells (Raze RT's light reach, in Dark units), for fading lights out
// smoothly with their distance -- a light more than a few rooms' walk away
// fades out, instead of a fixed number of lights being taken nearest first,
// whose list changed as you walked: lights that came and went right ahead.
static float *portal_rt_cell_dist;      // < 0: not reached
static mxs_vector *portal_rt_cell_entry;
static int portal_rt_dist_size;
static float portal_rt_dist_limit;

static void portal_rt_compute_reach(int start, float limit)
{
   static int *heap;       // cells, by distance (a binary heap)
   static int heap_cap;
   int n = 0, i;
   // The distances change little until the camera has moved: worked out
   // again only when it is in another cell or a foot from where they were
   // last measured (a city's 90 m of portals each frame cost milliseconds).
   static int last_start = -1, last_cells = -1;
   static PortalCell *last_world = NULL;
   static mxs_vector last_cam;
   static float last_limit = -1.0f;
   if (start == last_start && wr_num_cells == last_cells && wr_num_cells > 0 && wr_cell[0] == last_world &&
       limit == last_limit && portal_rt_dist_size >= wr_num_cells &&
       mx_dist2_vec(&portal_camera_loc, &last_cam) < 1.0f)
      return;
   last_start = start;
   last_cells = wr_num_cells;
   last_world = wr_num_cells > 0 ? wr_cell[0] : NULL;
   last_cam = portal_camera_loc;
   last_limit = limit;
   portal_rt_dist_limit = limit;
   if (wr_num_cells <= 0)
      return;
   if (portal_rt_dist_size < wr_num_cells) {
      free(portal_rt_cell_dist);
      free(portal_rt_cell_entry);
      portal_rt_cell_dist = (float *)malloc(sizeof(float) * wr_num_cells);
      portal_rt_cell_entry = (mxs_vector *)malloc(sizeof(mxs_vector) * wr_num_cells);
      portal_rt_dist_size = (portal_rt_cell_dist && portal_rt_cell_entry) ? wr_num_cells : 0;
   }
   if (!portal_rt_dist_size)
      return;
   for (i = 0; i < wr_num_cells; ++i)
      portal_rt_cell_dist[i] = -1.0f;
   if (start < 0 || start >= wr_num_cells || !WR_CELL(start))
      return;
   if (heap_cap < wr_num_cells * 4) {
      free(heap);
      heap_cap = wr_num_cells * 4;
      heap = (int *)malloc(sizeof(int) * heap_cap);
      if (!heap) {
         heap_cap = 0;
         return;
      }
   }
#define RT_HD(k) portal_rt_cell_dist[heap[k]]
   portal_rt_cell_dist[start] = 0.0f;
   portal_rt_cell_entry[start] = portal_camera_loc;
   heap[n++] = start;
   while (n > 0) {
      const int c = heap[0];
      PortalCell *cell = WR_CELL(c);
      PortalPolygonCore *poly;
      int np, v, k;
      // pop
      heap[0] = heap[--n];
      for (k = 0;;) {
         int a = 2 * k + 1, b = a + 1, m = k, t;
         if (a < n && RT_HD(a) < RT_HD(m)) m = a;
         if (b < n && RT_HD(b) < RT_HD(m)) m = b;
         if (m == k) break;
         t = heap[k]; heap[k] = heap[m]; heap[m] = t;
         k = m;
      }
      if (!cell)
         continue;
      poly = cell->portal_poly_list;
      np = cell->num_portal_polys;
      v = cell->portal_vertex_list;
      while (np-- > 0) {
         const int d = poly->destination;
         mxs_vector mid;
         float nd, dx, dy, dz;
         int j;
         mid.x = mid.y = mid.z = 0.0f;
         for (j = 0; j < poly->num_vertices; ++j) {
            const mxs_vector *pt = &cell->vpool[cell->vertex_list[v + j]];
            mid.x += pt->x; mid.y += pt->y; mid.z += pt->z;
         }
         if (poly->num_vertices > 0) {
            mid.x /= poly->num_vertices; mid.y /= poly->num_vertices; mid.z /= poly->num_vertices;
         }
         v += poly->num_vertices;
         ++poly;
         if (d < 0 || d >= wr_num_cells || !WR_CELL(d))
            continue;
         dx = mid.x - portal_rt_cell_entry[c].x;
         dy = mid.y - portal_rt_cell_entry[c].y;
         dz = mid.z - portal_rt_cell_entry[c].z;
         nd = portal_rt_cell_dist[c] + (float)sqrt(dx * dx + dy * dy + dz * dz);
         if (nd > limit)
            continue;
         if (portal_rt_cell_dist[d] >= 0.0f && nd >= portal_rt_cell_dist[d])
            continue;
         portal_rt_cell_dist[d] = nd;
         portal_rt_cell_entry[d] = mid;
         if (n < heap_cap) {
            // push (a cell may be in the heap twice; the later copy is stale but harmless)
            k = n++;
            heap[k] = d;
            while (k > 0 && RT_HD((k - 1) / 2) > RT_HD(k)) {
               const int pk = (k - 1) / 2, t = heap[k];
               heap[k] = heap[pk]; heap[pk] = t;
               k = pk;
            }
         }
      }
   }
#undef RT_HD
}

// A cell's walking distance from the camera (Dark units), 0 if it is drawn on
// screen, < 0 beyond reach (90 m): the ray tracer leaves far rooms out.
float portal_rt_cell_walk(int cell)
{
   if (cell < 0)
      return -1.0f;
   if (cell < portal_rt_drawn_size && portal_rt_drawn[cell] == portal_rt_stamp)
      return 0.0f;
   if (!portal_rt_dist_size || cell >= portal_rt_dist_size)
      return -1.0f;
   return portal_rt_cell_dist[cell];
}

// a point's walking distance from the camera (Dark units), < 0 if beyond reach.
// As Blood RT's: one in a cell drawn on screen counts as far as it is in a
// straight line over 2.5 (a lamp down a long street, seen, is in reach).
float portal_rt_path_distance(int cell, const mxs_vector *pos)
{
   float dx, dy, dz, walk = -1.0f;
   if (!portal_rt_dist_size || cell < 0 || cell >= portal_rt_dist_size)
      return -1.0f;
   if (portal_rt_cell_dist[cell] >= 0.0f) {
      dx = pos->x - portal_rt_cell_entry[cell].x;
      dy = pos->y - portal_rt_cell_entry[cell].y;
      dz = pos->z - portal_rt_cell_entry[cell].z;
      walk = portal_rt_cell_dist[cell] + (float)sqrt(dx * dx + dy * dy + dz * dz);
   }
   if (cell < portal_rt_drawn_size && portal_rt_drawn[cell] == portal_rt_stamp) {
      float seen;
      dx = pos->x - portal_camera_loc.x;
      dy = pos->y - portal_camera_loc.y;
      dz = pos->z - portal_camera_loc.z;
      seen = (float)sqrt(dx * dx + dy * dy + dz * dz) / 2.5f;
      if (walk < 0.0f || seen < walk)
         walk = seen;
   }
   return walk;
}

int portal_rt_light_relevant(int light)
{
   if (light < 0 || light >= PORTAL_RT_LIGHT_BITS)
      return 1;
   return (portal_rt_light_bits[light >> 3] >> (light & 7)) & 1;
}

// ---- the OpenGL renderer's real-time lights ----------------------------------
#include <render_backend.h>
extern void objlight_gl_submit(const mxs_vector *cam);

// The level's solid surfaces for the lights' shadows, once a mission: every
// cell's opaque polygons (not its portals -- water, openings -- which the
// game's own light passes through when it lights the lightmaps)
// The lava's lights (as the path tracer's, rtgl1.cpp build_lava_lights): its
// tops' area summed in squares 4 m a side, a light over each at its middle
extern int portal_lava_kind(int texture_id);
static void portal_gl_lava_lights(void)
{
   enum { kMax = 512 };
   static float sq[kMax * 4];   // x, y, z (area-weighted), area
   static int key[kMax * 2];
   int c, n = 0, i;
   const float side = 13.12f;
   for (c = 0; c < wr_num_cells; ++c) {
      PortalCell *r = WR_CELL(c);
      int p, k, voff = 0, first;
      if (!r || !r->render_list)
         continue;
      first = r->num_polys - r->num_portal_polys;
      for (p = 0; p < r->num_polys; ++p) {
         const int nv = r->poly_list[p].num_vertices;
         if (p >= first && p < r->num_render_polys && nv >= 3 &&
             portal_lava_kind(r->render_list[p].texture_id) == 1) {
            for (k = 2; k < nv; ++k) {
               const mxs_vector *a = &r->vpool[r->vertex_list[voff]];
               const mxs_vector *b = &r->vpool[r->vertex_list[voff + k - 1]];
               const mxs_vector *d = &r->vpool[r->vertex_list[voff + k]];
               mxs_vector u, v, x;
               float area, m[3];
               int kx, ky, j;
               mx_sub_vec(&u, b, a);
               mx_sub_vec(&v, d, a);
               mx_cross_vec(&x, &u, &v);
               area = 0.5f * mx_mag_vec(&x);
               if (area <= 0.0f)
                  continue;
               m[0] = (a->x + b->x + d->x) / 3.0f;
               m[1] = (a->y + b->y + d->y) / 3.0f;
               m[2] = (a->z + b->z + d->z) / 3.0f;
               kx = (int)floorf(m[0] / side);
               ky = (int)floorf(m[1] / side);
               for (j = 0; j < n && (key[j * 2] != kx || key[j * 2 + 1] != ky); ++j)
                  ;
               if (j == n) {
                  if (n >= kMax)
                     continue;
                  key[n * 2] = kx; key[n * 2 + 1] = ky;
                  sq[n * 4] = sq[n * 4 + 1] = sq[n * 4 + 2] = sq[n * 4 + 3] = 0.0f;
                  ++n;
               }
               sq[j * 4] += m[0] * area;
               sq[j * 4 + 1] += m[1] * area;
               sq[j * 4 + 2] += m[2] * area;
               sq[j * 4 + 3] += area;
            }
         }
         voff += nv;
      }
   }
   for (i = 0; i < n; ++i) {
      const float a = sq[i * 4 + 3];
      sq[i * 4] /= a;
      sq[i * 4 + 1] /= a;
      sq[i * 4 + 2] = sq[i * 4 + 2] / a + 1.6f;   // (half a metre over it)
   }
   RenderBackendSetLavaLights(sq, n);
   if (n > 0)
      mprintf("OpenGL: lava: %d lights over it\n", n);
}

static void portal_gl_world_mesh(void)
{
   static int known_cells = -1;
   static void *known_first = NULL, *known_pool = NULL;
   static float known_at[3];
   PortalCell *first = wr_num_cells > 0 ? WR_CELL(0) : NULL;
   int c, tris = 0, at = 0;
   float *xyz, *spheres;
   int *firsts;
   if (!first || !first->vpool)
      return;
   if (known_cells == wr_num_cells && known_first == (void *)first &&
       known_pool == (void *)first->vpool && known_at[0] == first->vpool[0].x &&
       known_at[1] == first->vpool[0].y && known_at[2] == first->vpool[0].z)
      return;
   known_cells = wr_num_cells;
   known_first = first;
   known_pool = first->vpool;
   known_at[0] = first->vpool[0].x; known_at[1] = first->vpool[0].y; known_at[2] = first->vpool[0].z;
   for (c = 0; c < wr_num_cells; ++c) {
      PortalCell *r = WR_CELL(c);
      int p, solid;
      if (!r)
         continue;
      solid = r->num_polys - r->num_portal_polys;
      for (p = 0; p < solid; ++p)
         if (r->poly_list[p].num_vertices >= 3)
            tris += r->poly_list[p].num_vertices - 2;
   }
   xyz = (float *)malloc(sizeof(float) * 9 * (tris > 0 ? tris : 1));
   firsts = (int *)malloc(sizeof(int) * (wr_num_cells + 1));
   spheres = (float *)malloc(sizeof(float) * 4 * wr_num_cells);
   if (!xyz || !firsts || !spheres) {
      free(xyz); free(firsts); free(spheres);
      return;
   }
   for (c = 0; c < wr_num_cells; ++c) {
      PortalCell *r = WR_CELL(c);
      int p, k, solid, voff = 0;
      firsts[c] = at;
      spheres[c * 4] = spheres[c * 4 + 1] = spheres[c * 4 + 2] = spheres[c * 4 + 3] = 0.0f;
      if (!r)
         continue;
      spheres[c * 4] = r->sphere_center.x;
      spheres[c * 4 + 1] = r->sphere_center.y;
      spheres[c * 4 + 2] = r->sphere_center.z;
      spheres[c * 4 + 3] = r->sphere_radius;
      solid = r->num_polys - r->num_portal_polys;
      for (p = 0; p < solid; ++p) {
         int n = r->poly_list[p].num_vertices;
         for (k = 2; k < n; ++k) {
            const mxs_vector *a = &r->vpool[r->vertex_list[voff]];
            const mxs_vector *b = &r->vpool[r->vertex_list[voff + k - 1]];
            const mxs_vector *d = &r->vpool[r->vertex_list[voff + k]];
            float *o = &xyz[at * 9];
            o[0] = a->x; o[1] = a->y; o[2] = a->z;
            o[3] = b->x; o[4] = b->y; o[5] = b->z;
            o[6] = d->x; o[7] = d->y; o[8] = d->z;
            ++at;
         }
         voff += n;
      }
   }
   firsts[wr_num_cells] = at;
   RenderBackendSetWorldShadowMesh(xyz, at, firsts, spheres, wr_num_cells);
   free(xyz); free(firsts); free(spheres);
   portal_gl_lava_lights();
}

// (DARK_GL_LIGHTS_VERIFY) an animated light's own lightmaps against the
// real-time light's reckoning, without shadows: its lit texels, the ratio
void portal_gl_verify_light(int li, const float pos[3], float B, float R, float inner)
{
   int c, n = 0, close = 0, cells = 0, polys = 0, wrext = 0;
   double sum = 0.0, sum2 = 0.0;
   for (c = 0; c < wr_num_cells; ++c) {
      PortalCell *r = WR_CELL(c);
      int k, p, voff = 0, pal = -1;
      if (!r || !r->light_list || !r->anim_light_index_list)
         continue;
      for (k = 0; k < r->num_anim_lights; ++k)
         if (r->anim_light_index_list[k] == li)
            pal = k;
      if (pal < 0)
         continue;
      ++cells;
      for (p = 0; p < r->num_render_polys; ++p) {
         PortalLightMap *lt = &r->light_list[p];
         PortalPolygonRenderInfo *ri = &r->render_list[p];
         const int nv = r->poly_list[p].num_vertices;
         const mxs_vector *anchor = &r->vpool[r->vertex_list[voff + r->render_list[p].texture_anchor]];
         const mxs_vector *nrm = &r->plane_list[r->poly_list[p].planeid].normal;
         voff += nv;
         if (!(lt->anim_light_bitmask & (1u << pal)))
            continue;
         ++polys;
         if (portal_wrext_u_base[c])
            ++wrext;
         {
            int before = 0, b, i, j;
            const int size = lt->h * lt->pixel_row;
            LightmapEntry *layer;
            const float tbu = portal_wrext_u_base[c] ? portal_wrext_u_base[c][p] : ri->u_base / 4096.0f;
            const float tbv = portal_wrext_v_base[c] ? portal_wrext_v_base[c][p] : ri->v_base / 4096.0f;
            for (b = 0; b < pal; ++b)
               if (lt->anim_light_bitmask & (1u << b))
                  ++before;
            layer = lt->data + size * (1 + before);
            for (j = 0; j < lt->h; ++j)
               for (i = 0; i < lt->w; ++i) {
                  const LightmapEntry e = layer[j * lt->pixel_row + i];
#if defined(RGB_LIGHTING) && defined(RGB_888)
                  int have = e.A[0] > e.A[1] ? e.A[0] : e.A[1];
                  if (e.A[2] > have) have = e.A[2];
#elif defined(RGB_LIGHTING)
                  int have = (e & 0x1f) > ((e >> 5) & 0x1f) ? (e & 0x1f) : ((e >> 5) & 0x1f);
                  if (((e >> 10) & 0x1f) > have) have = (e >> 10) & 0x1f;
                  have = have * 255 / 31;
#else
                  int have = e;
#endif
                  float U = (lt->base_u + i) * 0.25f, V = (lt->base_v + j) * 0.25f, w[3], L[3], d, cs, f;
                  if (have < 12)
                     continue;
                  w[0] = anchor->x + ri->tex_u.x * (U - tbu) + ri->tex_v.x * (V - tbv);
                  w[1] = anchor->y + ri->tex_u.y * (U - tbu) + ri->tex_v.y * (V - tbv);
                  w[2] = anchor->z + ri->tex_u.z * (U - tbu) + ri->tex_v.z * (V - tbv);
                  L[0] = pos[0] - w[0]; L[1] = pos[1] - w[1]; L[2] = pos[2] - w[2];
                  d = (float)sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
                  if (d < 0.01f)
                     continue;
                  cs = (L[0] * nrm->x + L[1] * nrm->y + L[2] * nrm->z) / d;
                  if (cs <= 0.0f)
                     continue;
                  f = (0.5f + 0.5f * cs) * B / (d > 0.5f ? d : 0.5f);
                  if (R > 0.0f && inner > 0.0f && d > inner)
                     f *= (R - d) / (R - inner);
                  if (f > 255.0f)
                     f = 255.0f;
                  if (f < 12.0f)
                     continue;
                  {
                     const double ratio = have / f;
                     sum += ratio;
                     sum2 += ratio * ratio;
                     if (ratio > 0.85 && ratio < 1.15)
                        ++close;
                     ++n;
                  }
               }
         }
      }
   }
   if (n > 0) {
      const double mean = sum / n;
      mprintf("Real-time light %d: its lightmaps against the reckoning: %d lit texels, "
              "ratio %.2f (spread %.2f), %d%% within 15%%\n", li, n, mean,
              sqrt(sum2 / n - mean * mean > 0 ? sum2 / n - mean * mean : 0), close * 100 / n);
   } else
      mprintf("Real-time light %d: no lit texels of its own to compare (%d cells, %d polygons, %d WREXT)\n",
              li, cells, polys, wrext);
}

static void portal_gl_begin(void)
{
   if (RenderRTActive())
      return;
   // (with the setting off too: the lights it had in real time put back
   // into the lightmaps)
   if (RenderBackendDynamicLightsWanted())
      portal_gl_world_mesh();
   objlight_gl_submit(&portal_camera_loc);
}

static void portal_rt_begin(float zoom, int cell)
{
   mxs_matrix *slew;
   float p[3], f[3], l[3], u[3];
   if (!RenderRTActive())
      return;
   slew = r3_get_slew_matrix();
   p[0] = portal_camera_loc.x; p[1] = portal_camera_loc.y; p[2] = portal_camera_loc.z;
   f[0] = slew->vec[0].x; f[1] = slew->vec[0].y; f[2] = slew->vec[0].z;
   l[0] = slew->vec[1].x; l[1] = slew->vec[1].y; l[2] = slew->vec[1].z;
   u[0] = slew->vec[2].x; u[1] = slew->vec[2].y; u[2] = slew->vec[2].z;
   RenderRTSceneBegin(p, f, l, u, zoom, grd_bm.w, grd_bm.h);
   // MEDIA_WATER (engfeat/media.h) is 2
   RenderRTCameraMedium(cell >= 0 && cell < wr_num_cells && WR_CELL(cell) &&
                        WR_CELL(cell)->medium == 2);
   RenderRTFog(portal_frame_fog_rgb, portal_fog_on ? portal_frame_fog_dist : 0.0f);
   RenderRTPrepBegin();
   portal_rt_submit_world();
   RenderRTPrepMark(0);
   portal_rt_find_lights(cell);
   RenderRTPrepMark(1);
   portal_rt_compute_reach(cell, 90.0f / 0.3048f);
   RenderRTPrepMark(2);
   objlight_rt_submit();
   RenderRTPrepMark(3);
   RenderRTPrepEnd();
}
#endif

extern void DetailAttachmentsUpdate(void);

#ifdef DARK_RT
#include <time.h>
// Where the game's view spends its frame, for the performance log
// (rtgl1.cpp): seconds summed, until the log takes them -- 0 finding the
// cells in view, 1 sorting them and finding their objects, 2 their light
// maps, 3 drawing them (their objects: the walls and floors are not drawn),
// 4 after them (see-through objects, particles, overlays), 5 the objects
// out of view drawn for their shadows
double g_PortalPartSeconds[6];
static double portal_part_clock(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#define PORTAL_PART(i) do { const double now_ = portal_part_clock(); \
   g_PortalPartSeconds[i] += now_ - portal_part_last; portal_part_last = now_; } while (0)
#else
#define PORTAL_PART(i) ((void)0)
#endif

void portal_render_scene(Position *pos, float zoom)
{  PROF
   Location *loc;
   int cell, i;
   BOOL fog_was_on;
   // objects attached to others (NewDark detail attachments) follow them
   DetailAttachmentsUpdate();
   loc = &pos->loc;
   cell = CellFromLoc(loc);
   FogZoneFrame(cell, portal_frame_fog_rgb, &portal_frame_fog_dist);
   fog_was_on = portal_fog_on;
#ifdef STATS_ON
   zero_stats();
#endif

   cache_mem_this_frame = 0;
   background_setup = FALSE;

#ifndef SHIP
   if (portal_write_traversal) {
      trav_file = fopen("traverse.log", "a");
      portal_write_traversal = FALSE;
   }
   AssertMsg2(HACK_MAX_OBJ>=gMaxObjID,"Portal Using fewer ObjIDs (%d) than object system (%d)\n",HACK_MAX_OBJ,gMaxObjID);
#endif

   portal_start_3d(pos);
   r3_set_zoom (zoom);
#ifdef DARK_RT
   portal_rt_begin(zoom, cell);
   portal_gl_begin();
   double portal_part_last = portal_part_clock();
#endif

     // tell the scene clipper to clear its clipinfo

   g2pt_reset_clip(0, grd_bm.w, grd_bm.h);

   g2pt_duv_set_size(grd_bm.w, grd_bm.h);

     // if we have the special "just draw it all flag",
     // just go through it all and draw it in any order
     // but we don't draw any objects!

   if (cell == CELL_INVALID) {
      bool save_state = punt_hardware_lighting;

      Warning(("Viewer not in any cell.\n"));

      // we need to do this since we don't preload lightmaps here...
      punt_hardware_lighting = TRUE;

      if (portal_pre_draw_callback != NULL)
         portal_pre_draw_callback();

      for (i=0; i < wr_num_cells; ++i) {
         r_rdata=0; // clear the active cell after every cell.  duh
         outgoing=0; // similarly
         r3_start_block();
         r3_set_clipmode(R3_CLIP);
         setup_cell(WR_CELL(i));
         r3_end_block();
         CLIP_DATA(wr_cell[i]) = PortalClipRectangle(0,0, grd_bm.w, grd_bm.h);
         draw_region(i);
         free_cell(wr_cell[i]);
      }
      punt_hardware_lighting = save_state;
      goto cleanup;
   }

   // do the traversal of all visible cells
   portal_traverse_scene(cell);
   PORTAL_PART(0);

   if (portal_pre_draw_callback != NULL)
      portal_pre_draw_callback();

   if (bRenderSky && pSkyRenderCallback)
   {
      bRenderNewSky = pSkyRenderCallback(); // New-style sky may have rendered

      // wsf: not sure if we really need to reset these or not, but they're cheap anyway.
      g2pt_reset_clip(0, grd_bm.w, grd_bm.h);
      g2pt_duv_set_size(grd_bm.w, grd_bm.h);
   }
   else
      bRenderNewSky = FALSE;


   update_terrain_animation();

     // now traverse them in front-to-back order so we can determine
     // object rendering information for now we just draw each object
     // in the first cell we encounter

   setup_bsp(0);
   sort_via_bsp(0, &(loc->vec), 0);

   for (i=0; i < r_sorted_count; ++i)
      wr_cell[active_regions[i]]->render_data->sorted_index = i;

#ifdef STATS_ON
   stat_num_sort_ms = portal_get_time();
#endif

   init_process_objects();

   for (i=0; i < r_sorted_count; ++i)
      process_objects(wr_cell[active_regions[i]]);

   if (portal_allow_object_splitting) {
      check_for_object_splitting();

      if (portal_obj_fixup)
         check_for_extra_object_splitting();
   } else {
#ifdef EDITOR
      // Hardware rendering already clips objects against the viewport and
      // resolves them against terrain with the depth buffer.  The legacy
      // screen-octagon test is based on portal-local projected bounds and is
      // unreliable when DromEd renders into an offset sub-viewport: objects
      // are rejected as the camera moves across the pane, often surviving
      // only near its left or right edge.  Objects reach this point only when
      // they are referenced by a visible portal cell, so queue them and let
      // the GPU perform the final clip/depth test.
#else
      check_for_object_hiding();
#endif
   }

#ifdef STATS_ON
   stat_num_sort_ms = portal_get_time() - stat_num_sort_ms;
#endif

   // Now that we've explored all of the areas, we're going to draw
   // them all.  If the variable "span_clip" is set, then we're using
   // a scene clipper that requires we render front to back.
   // Otherwise, we render back to front.  But to keep things
   // interesting, if the flag "render_backward" is on, we do the
   // opposite--this is a useful debugging tool to show you how much
   // stuff is being processed.

   PORTAL_PART(1);
   portal_mip_setup(zoom);

   // (not under the path tracer: these are the light map textures the
   // engine's own drawing of the walls and floors uses, and it does not draw
   // them -- see draw_region_lgd3d; the path tracer's light maps are its
   // own, made with the world. 4 ms a frame at Reflections in Red's vista.)
   if ((g_lgd3d)&&(!punt_hardware_lighting)
#ifdef DARK_RT
       && !RenderRTActive()
#endif
      ) {
      for (i=0; i < r_sorted_count; i++)
         portal_preload_lightmaps(active_regions[i]);
      porthw_end_frame();
   }
   PORTAL_PART(2);

   // the frame's fog (a fog zone's, eased): a frame without any leaves the
   // setting alone -- it was switched off for good here, so one fogless
   // moment (a clear zone) ended the mission's fog
   if (portal_frame_fog_dist <= 0)
      portal_fog_on = FALSE;   // for this frame only: restored at its end
   if (g_lgd3d && portal_fog_on && portal_frame_fog_dist > 0) {
      const int fr = (int)(portal_frame_fog_rgb[0] + 0.5f);
      const int fg = (int)(portal_frame_fog_rgb[1] + 0.5f);
      const int fb = (int)(portal_frame_fog_rgb[2] + 0.5f);
      lgd3d_set_fog_color(fr, fg, fb);
      lgd3d_set_linear_fog_distance(portal_frame_fog_dist);
      fog_r3_color = guiRGB(fr, fg, fb);
   }

#ifdef STATS_ON
   stat_num_render_ms = portal_get_time();
#endif
   if (portal_render_back_to_front()) {
      if (g_lgd3d && portal_true_two_pass) {
         for(i=r_sorted_count-1; i >= 0; --i) {
            // render textures only if we're zbuffering to avoid state changes
            draw_region_lgd3d(active_regions[i]);
         }
         // now render lightmaps if we're z buffering
         // (not under the path tracer: see draw_region_lgd3d)
         if (g_zbuffer && !portal_multitexture && (!punt_hardware_lighting)
#ifdef DARK_RT
             && !RenderRTActive()
#endif
            ) {
            lgd3d_set_zcompare(TRUE);
            lgd3d_set_zwrite(FALSE);
            lgd3d_blend_multiply(portal_hack_blend);
            lgd3d_set_blend(TRUE);
#ifdef RGB_LIGHTING
            lgd3d_set_alpha(0.5);
#endif
            for(i=r_sorted_count-1; i >= 0; --i) {
               draw_region_lightmap_only(active_regions[i]);
               free_cell(WR_CELL(active_regions[i]));
            }
            lgd3d_blend_normal();
            lgd3d_set_blend(FALSE);
#ifdef RGB_LIGHTING
            lgd3d_set_alpha(1.0);
#endif
            lgd3d_set_zcompare(FALSE);
            lgd3d_set_zwrite(TRUE);
         } else {
            // still need to free cells...
            for(i=r_sorted_count-1; i >= 0; --i) {
               free_cell(WR_CELL(active_regions[i]));
            }
         }
      } else {
         for(i=r_sorted_count-1; i >= 0; --i) {
            draw_region(active_regions[i]);
            free_cell(WR_CELL(active_regions[i]));
         }
      }
   } else {
      for(i=0; i < r_sorted_count; ++i) {
         draw_region(active_regions[i]);
         free_cell(wr_cell[active_regions[i]]);
#ifdef LIGHTMAP_DEBUG
         if (show_lightmap)
            draw_lightmap_points(active_regions[i]);
#endif
      }
      if (!render_backward) {
#ifdef STATS_ON
         // don't count background hack pixels as clipped away
         int old_drawn = stat_num_drawn_pixels;
         int old_clipped = stat_num_clipped_pixels;
         render_background_hack();
         stat_num_clipped_pixels =
            old_clipped + (stat_num_drawn_pixels - old_drawn);
#else
         render_background_hack();
#endif
      }
   }
#ifdef STATS_ON
   stat_num_render_ms = portal_get_time() - stat_num_render_ms;
#endif
   PORTAL_PART(3);

cleanup:
   if (ptsky_type == PTSKY_SPAN)
      ptsky_render_stars();

   // now that we've finished traversing the scene, we tell the
   // renderer--it may have deferred rendering some polygons, e.g.
   // ones with transparency/translucency if we were drawing
   // front-to-back

   background_hack_cleanup();

   g2pt_post_render_polys();

#ifdef DARK_RT
   {
      // deferred (translucent) objects, particles and overlays are objects
      const int rt_suppressed = RenderRTSuppressCapture(0);
#endif
   if (portal_post_render_cback)
      portal_post_render_cback();

   if (g_lgd3d && portal_fog_on && portal_frame_fog_dist > 0)
      lgd3d_set_fog_enable(FALSE);

   if (portal_render_overlays_cback)
      portal_render_overlays_cback();
#ifdef DARK_RT
      RenderRTSuppressCapture(rt_suppressed);
   }
#endif

#ifndef SHIP
   if (portal_test_blend)
      portal_do_blendtest();
#endif

#ifdef DARK_RT
   if (cell != CELL_INVALID)
      PORTAL_PART(4);
   {
      // nearby objects out of view: their shadows and the light they block
      const int rt_suppressed = RenderRTSuppressCapture(0);
      RenderRTPrepBegin();
      RenderRTGameDrawExtraObjects();
      RenderRTPrepMark(4);
      RenderRTPrepEnd();
      RenderRTSuppressCapture(rt_suppressed);
   }
   if (cell != CELL_INVALID)
      PORTAL_PART(5);
   RenderRTSceneEnd();
   if (!RenderRTActive()) {
      extern void RenderGLGameDrawShadowObjects(void);
      RenderGLGameDrawShadowObjects();
      RenderBackendDynamicViewEnd();
   }
#endif
   portal_fog_on = fog_was_on;
   portal_end_3d();

#ifndef SHIP
   if (trav_file) {
      fprintf(trav_file, "End of scene.\n");
      fclose(trav_file);
      trav_file = NULL;
   }
#endif

   _portal_mark_visible_cells();

   last_r_count = r_sorted_count;
   END_PROF;
}

int PortalRenderPick(Position *pos, int x, int y, float zoom)
{
   Location *loc = &pos->loc;
   int cell = CellFromLoc(loc), i, val;

   if (cell == CELL_INVALID)
      return -1;

   portal_start_3d(pos);
   r3_set_zoom (zoom);

     // do the traversal of all visible cells
   portal_traverse_scene(cell);

     // pick_region until val != -1, and free them all
   val = -1;
   for(i=0; i < r_sorted_count; ++i) {
      if (val == -1) {
         val = pick_region(wr_cell[active_regions[i]], x, y);
         if (val != -1)
            val = active_regions[i]*256 + val;  // encode cell+polygon
      }
      free_cell(wr_cell[active_regions[i]]);
   }

   portal_end_3d();

   return val;
}

/////////////////////////////////////////////////////////////////////
//              lighting
//

#ifdef RGB_LIGHTING

#ifndef RGB_888
extern ulong pl_red_color[256], pl_green_color[256], pl_blue_color[256];
extern ulong pl_blue_mask, pl_green_mask, pl_red_mask;
ulong pl_blue_mask=0x7c00, pl_green_mask=0x03e0, pl_red_mask=0x001f;
#endif // ~RGB_888

int hack_red, hack_green, hack_blue;

// set color tables given color 0..255 in each thing, one of them 255
void portal_set_normalized_color(int rm, int gm, int bm)
{
#ifdef RGB_888
   hack_red = (rm * 256) / 255;
   hack_green = (gm * 256) / 255;
   hack_blue = (bm * 256) / 255;
#else // RGB_888
   int r,g,b,dr,dg,db,i;
   hack_red = rm;
   hack_green = gm;
   hack_blue = bm;

   // after 256 steps, r should reach rm% of red_mask, << 8:
   dr = pl_red_mask * rm;
   dg = pl_green_mask * gm;
   db = pl_blue_mask * bm;
   r = g = b = 0;
   for (i=0; i < 256; ++i) {
      pl_red_color[i]   = (r >> 16) & pl_red_mask;
      pl_green_color[i] = (g >> 16) & pl_green_mask;
      pl_blue_color[i]  = (b >> 16) & pl_blue_mask;
      r += dr;
      g += dg;
      b += db;
   }
#endif // RGB_888
}

void portal_convert_hsb_to_rgb(int *rp, int *gp, int *bp, float hue, float saturation)
{
#ifndef DEEPC
   float r,g,b, isat;
   hue *= 3;
   // compute fully saturated color
   if (hue < 1.0) {
      // interpolate from red to green
      r = 1-hue;
      g = hue;
      b = 0;
   } else if (hue < 2.0) {
      g = 2-hue;
      b = hue-1;
      r = 0;
   } else {
      b = 3-hue;
      r = hue-2;
      g = 0;
   }

   // blend with white
   isat = 1.0f - saturation;
   r = r*saturation + isat;
   g = g*saturation + isat;
   b = b*saturation + isat;
   *rp = (int)(r * 255);
   *gp = (int)(g * 255);
   *bp = (int)(b * 255);
#else // ~DEEPC
   float r, g, b, isat, max_recip;

   // quantize to 24 hues--the strange number is 1/48, for rounding
   int i_hue = ((double) hue + 0.020833333333333333333333333333333) * 24.0;
   if (i_hue > 24)
      i_hue = 24;
   hue = i_hue;

   // compute fully saturated color
   if (hue < 8.0) {
      // interpolate from red to green
      r = 8-hue;
      g = hue;
      b = 0;
   } else if (hue < 16.0) {
      g = 16-hue;
      b = hue-8;
      r = 0;
   } else {
      b = 24-hue;
      r = hue-16;
      g = 0;
   }

   // make largest component unit so light won't be dim
   max_recip = (r > g) ? r : g;
   max_recip = (max_recip > b) ? max_recip : b;
   max_recip = 1.0 / max_recip;

   r *= max_recip;
   g *= max_recip;
   b *= max_recip;

   // blend with white
   isat = 1.0 - saturation;
   r = r*saturation + isat;
   g = g*saturation + isat;
   b = b*saturation + isat;

   *rp = r * 255;
   *gp = g * 255;
   *bp = b * 255;
#endif // ~DEEPC
}

extern ulong pl_red_convert[32], pl_blue_convert[32], pl_green_convert[32];

void portal_setup_light_masks(void)
{
   grs_rgb_bitmask bm;
   if (g_lgd3d)
      lgd3d_get_opaque_texture_bitmask(&bm);
   else
      gr_get_screen_rgb_bitmask(&bm);

#ifndef RGB_888
   {
      int i;

      for (i=0; i < 32; ++i) {
         pl_red_convert[i]   = (i * bm.red   / 31) & bm.red;
         pl_green_convert[i] = (i * bm.green / 31) & bm.green;
         pl_blue_convert[i]  = (i * bm.blue  / 31) & bm.blue;
      }
   }
#endif // ~RGB_888
   portal_set_normalized_color(255,255,255);
}
#endif

void portal_render_light(Position *pos, float zoom, float br,
                         void (*light_func)(int))
{
   Location *loc = &pos->loc;
   grs_bitmap dum_bm;
   grs_canvas dum_cnv;

   int cell = CellFromLoc(loc), i;

   if (cell == CELL_INVALID) {
      //mprintf("Light not in region\n");
      return;
   }

   gr_init_bitmap(&dum_bm, 0, BMT_FLAT8, 0, 240, 240);
   gr_make_canvas(&dum_bm, &dum_cnv);
   gr_push_canvas(&dum_cnv);

   portal_start_3d(pos);

   r3_set_zoom(zoom);

   portal_traverse_scene(cell);

     // setup some parameters which are true forever
   PortalSetLightInfo(loc, br, wr_cell[cell]->medium);

   for (i=0; i < r_sorted_count; ++i) {
      light_func(active_regions[i]);
      free_cell(wr_cell[active_regions[i]]);
   }
//mprintf("lit cells: %d  viewed cells %d\n", r_sorted_count, last_r_count);

   portal_end_3d();
   gr_pop_canvas();
}

#define MAX_LIT_CELLS DARK_LIMIT_RUNTIME_LIGHT_CELLS
int lit_cell[MAX_LIT_CELLS], num_lit, num_culled;

// store indexes of all cells reached in a given direction in lit_cell[]
bool portal_visit_light(Position *pos, float zoom)
{
   Location *loc = &pos->loc;
   grs_bitmap dum_bm;
   grs_canvas dum_cnv;
   bool enough_active_regions = TRUE;

   int cell = CellFromLoc(loc), i, j;

   if (cell == CELL_INVALID) {
      //mprintf("Light not in region\n");
      return TRUE;
   }

   gr_init_bitmap(&dum_bm, 0, BMT_FLAT8, 0, 240, 240);
   gr_make_canvas(&dum_bm, &dum_cnv);
   gr_push_canvas(&dum_cnv);

   portal_start_3d(pos);

   r3_set_zoom(zoom);

   portal_traverse_scene(cell);
   if (r_sorted_count == MAX_ACTIVE_REGIONS)
      enough_active_regions = FALSE;

   for (i=0; i < r_sorted_count; ++i) {
      int r = active_regions[i];
      for (j=0; j < num_lit; ++j)
        if (r == lit_cell[j])
           break;
      if (j == num_lit && num_lit < MAX_LIT_CELLS)
        lit_cell[num_lit++] = r;
      free_cell(wr_cell[r]);
   }

   portal_end_3d();
   gr_pop_canvas();
   return enough_active_regions;
}

#define BRIGHT_SCALE   4


int portal_add_spotlight(float br, Position *pos, float zoom,
                         uchar dynamic)
{
#if 0
   dynamic_light = dynamic & LIGHT_DYNAMIC;

//   if (dynamic == LIGHT_STATIC)
//      LightDefineStart();

   br *= BRIGHT_SCALE;

   portal_render_light(pos, zoom, br, spotlight_region);

//   if (dynamic == LIGHT_STATIC) {
//      int lt = LightDefineEnd();
//      LightTurnOnSet(lt, 255);
//      return lt;
//   } else
#endif
      return -1;
}

int cur_raycast_cell;

#ifdef LIGHT_MAP

#ifdef LIGHTMAP_DEBUG

#define MAX_RAYCAST_CELL 256
#define MAX_RAYCAST_PT   256

mxs_vector lt_pt[MAX_RAYCAST_CELL][MAX_RAYCAST_PT];
int lt_count[MAX_RAYCAST_CELL];

void save_lightmap_point(mxs_vector *pt, bool dummy)
{
   if (cur_raycast_cell >= MAX_RAYCAST_CELL) return;
   if (lt_count[cur_raycast_cell] < MAX_RAYCAST_PT)
      lt_pt[cur_raycast_cell][lt_count[cur_raycast_cell]++] = *pt;
}

void draw_lightmap_points(int cell)
{
   int i;
   r3s_point p;
   if (cell >= MAX_RAYCAST_CELL) return;
   for (i=0; i < lt_count[cell]; ++i) {
      r3_transform_point(&p, &lt_pt[cell][i]);
      if (!p.ccodes) {
         gr_set_pixel(1, fix_int(p.grp.sx), fix_int(p.grp.sy));
      }
   }
}

#else

void save_lightmap_point(mxs_vector *pt, bool dummy)
{
}

#endif

extern void (*lightmap_point_callback)(mxs_vector *loc, bool lit);

Location *light_loc;
void draw_light_region_number(int n)
{
   portal_dynamic_light(wr_cell[n], light_loc);
}


extern float ambient_weight;
extern float max_dist, max_dist_2, inner_dist, dist_diff_recip;

int portal_add_omni_light(float br, float ambient, Location *loc,
                          uchar style, float radius, float inner_radius)
{
   Position pos;
   int cell, i, t;
   bool region_check = TRUE;

   num_lit = 0;
   num_culled = 0;

   cell = CellFromLoc(loc);
   if (cell == CELL_INVALID)
      return -1;

   br *= BRIGHT_SCALE / 2;
   max_dist = radius;
   max_dist_2 = radius * radius;
   inner_dist = inner_radius;
   if (inner_dist)
      dist_diff_recip = 1.0f / (radius - inner_radius);

   lightmap_point_callback = save_lightmap_point;

   ambient_weight = br;

   PortalSetLightInfo(loc, br * BRIGHT_SCALE, (WR_CELL(cell))->medium);
   light_loc = loc;

   // record all the cells reached by our light in lit_cell[] and num_lit
   pos.loc = *loc;
   pos.fac.tx = 0;
   pos.fac.ty = 0;
   pos.fac.tz = 0x4000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.tz = 0x8000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.tz = 0xc000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.tz = 0;      region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.ty = 0x4000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.ty = 0xc000; region_check &= portal_visit_light(&pos, 1.0);

   if (!region_check)
      mprintf("\nToo many cells to light at (%g, %g, %g)",
              loc->vec.x, loc->vec.y, loc->vec.z);

   for (i = 0; i < (num_lit - num_culled); ++i) {
      int n = lit_cell[i];
      bool cell_lit = TRUE;

      cur_raycast_cell = n;
#ifdef LIGHTMAP_DEBUG
      if (n < MAX_RAYCAST_CELL)
         lt_count[n] = 0;
#endif

      // Dynamic lights are never animated, since they change every
      // frame anyhow.  And quickness overrides raycast lighting.
      // (Currently, they're actually opposites.)
      if (style & LIGHT_DYNAMIC)
         portal_dynamic_light(WR_CELL(n), loc);
      else if (style & LIGHT_QUICK)
         cell_lit = portal_nonraycast_light(WR_CELL(n), loc, style);
      else
         cell_lit = portal_raycast_light(WR_CELL(n), loc, style);

      // If we didn't really reach a cell, we move it to a group of them
      // at the end of the list.  This lets us discard those cells if we
      // like (this is used to cull the cell lists of animated lights).
      if (!cell_lit) {
         ++num_culled;
         t = lit_cell[i];
         lit_cell[i] = lit_cell[num_lit - num_culled];
         lit_cell[num_lit - num_culled] = t;
         --i;
      }
   }

   return num_lit;
}

void portal_add_light_bright(Location *loc, float radius, BOOL light)
{
   int amount = light ? 1 : -1;
   Position pos;
   int cell, i;
   bool region_check = TRUE;

   num_lit = 0;

   cell = CellFromLoc(loc);
   if (cell == CELL_INVALID)
      return;

   z_test = TRUE;  // don't allow light to go further than radius
   test_radius = radius;

   lightmap_point_callback = save_lightmap_point;

   //ambient_weight = br;
   //PortalSetLightInfo(loc, br * BRIGHT_SCALE, (WR_CELL(cell))->medium);
   light_loc = loc;

   flashbomb_through_door_hack = TRUE;

   // record all the cells reached by our light in lit_cell[] and num_lit
   pos.loc = *loc;
   pos.fac.tx = 0;
   pos.fac.ty = 0;
   pos.fac.tz = 0x4000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.tz = 0x8000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.tz = 0xc000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.tz = 0;      region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.ty = 0x4000; region_check &= portal_visit_light(&pos, 1.0);
   pos.fac.ty = 0xc000; region_check &= portal_visit_light(&pos, 1.0);

   flashbomb_through_door_hack = FALSE;

#ifdef DBG_ON
   if (!region_check)
      mprintf("\nToo many cells to light at (%g, %g, %g)",
              loc->vec.x, loc->vec.y, loc->vec.z);
#endif

   for (i = 0; i < num_lit; ++i) {
      int n = lit_cell[i];
      PortalCell *r = WR_CELL(n);
      r->num_full_bright += amount;
   }

   z_test = FALSE;
}
#endif // LIGHT_MAP


// This is meant to be called right after portal_add_omni_light.
// It sets up the arrays which tell each cell which lights affect it.
void portal_shine_omni_light(int light_index, Location *loc,
                             uchar dynamic)
{
   int i;

   for (i=0; i < num_lit; ++i) {
      int n = lit_cell[i];
      PortalCell *r = WR_CELL(n);
      ++r->light_indices[0];  // count of lights
      r->light_indices = Realloc(r->light_indices,
           (r->light_indices[0]+1) * sizeof(r->light_indices[0]));
      r->light_indices[r->light_indices[0]] = light_index;
   }
   return;
}


#define kPortalSunlightMinMag .1

int portal_shine_sunlight(const mxs_vector *pDirection, float fBrightness,
                          int light_index, uchar perm)
{
   int i;
   BOOL num_cells_lit = 0;
   BOOL cell_lit;

   // set up globals
   if (fabs(pDirection->x) < kPortalSunlightMinMag
    && fabs(pDirection->y) < kPortalSunlightMinMag
    && fabs(pDirection->z) < kPortalSunlightMinMag) {
      Warning(("portal.c: sunlight vector too small.  Not using sunlight.\n"));
      return 0;
   }

   ambient_weight = fBrightness / kPortalSunlightCastLength;

   // iterate over all cells
   for (i=0; i < wr_num_cells; ++i) {
      PortalCell *cell = WR_CELL(i);
      cur_raycast_cell = i;
      cell_lit = portal_raycast_sunlight(cell, perm);
      if (cell_lit) {
         ++num_cells_lit;

         ++cell->light_indices[0];         // count of lights on cell
         cell->light_indices
            = Realloc(cell->light_indices, (cell->light_indices[0]+1)
                                         * sizeof(cell->light_indices[0]));
         cell->light_indices[cell->light_indices[0]] = light_index;
      }
   }
   return num_cells_lit;
}


// helper for portal_add_simple_dynamic_light--pares down cell list to
// cells which can be reached from a given cell
int portal_contiguous_cells(int *cell_list, int num_cells, int root_cell)
{
   int i, j, t, portal_index;
   PortalPolygonCore *portal;
   PortalCell *cell, *adjacent_cell;
   int adjacent_cell_index;
   int num_contiguous;

   if (num_cells == 1)
      return 1;

   if (root_cell == CELL_INVALID)
      return 0;

   // find the seed cell in the list
   i = 0;
   while ((i < num_cells) && (cell_list[i] != root_cell))
      ++i;

#ifdef DBG_ON
   // Is the seed cell in the list?
   AssertMsg1(i != num_cells,
              "Dynamic light cell list does not contain root cell %d\n",
              root_cell);
#endif // ~DBG_ON

   // force the seed to element 0
   cell_list[i] = cell_list[0];
   cell_list[0] = root_cell;

   // We'll mark all cells but the first, then clear each we visit as
   // we recurse through the database from the seed cell.
   for (i = 1; i < num_cells; ++i)
      WR_CELL(cell_list[i])->flags |= CELL_TRAVERSED;

   num_contiguous = 1;

   // Now we iterate in the brute_force, ugly way, collecting all the
   // cells we can reach in the beginning of the array.  Notice that
   // num_contiguous may rise.
   for (i = 0; i < num_contiguous; ++i) {
      cell = WR_CELL(cell_list[i]);

      portal_index = cell->num_portal_polys;
      portal = cell->portal_poly_list;

      while (portal_index--) {
         adjacent_cell_index = portal->destination;
         adjacent_cell = WR_CELL(adjacent_cell_index);

         // If a cell is flagged, we know that it's within the sphere
         // and has not yet been visited.  We swap it into the lower
         // part of cell_list, where we're collecting the contiguous
         // cells, and clear its flag.
         if (adjacent_cell->flags & CELL_TRAVERSED) {
            adjacent_cell->flags &= ~CELL_TRAVERSED;

            // find our cell in the array
            for (j = num_contiguous; j < num_cells; ++j)
               if (cell_list[j] == adjacent_cell_index)
                  break;

#ifdef DBG_ON
            AssertMsg1(j != num_cells,
                       "light cell list does not contain marked cell %d\n",
                       adjacent_cell_index);
#endif // ~DBG_ON

            t = cell_list[num_contiguous];
            cell_list[num_contiguous] = cell_list[j];
            cell_list[j] = t;
            ++num_contiguous;
         }

         portal++;
      }
   }

   // Now in theory, we have num_contiguous cells for which we've
   // cleared the CELL_TRAVERSED flags, and some others at the end of
   // the array which are still marked.
#ifdef DBG_ON
   for (i = num_contiguous; i < num_cells; ++i)
      AssertMsg1((WR_CELL(cell_list[i])->flags & CELL_TRAVERSED) != 0,
                 "cell we did not visit is not marked %d\n",
                 adjacent_cell_index);
#endif // ~DBG_ON

   for (i = num_contiguous; i < num_cells; ++i)
      WR_CELL(cell_list[i])->flags &= ~CELL_TRAVERSED;

   return num_contiguous;
}


// This is for the following function.  It's on a scale of 0 to 255.
#define SIMPLE_LIGHT_MIN_BRIGHTNESS 16.0

// This is a sloppier way to generate a dynamic light.  Rather than
// using the rendering system's visibility code, it just finds all the
// cells which 1) are within the radius affected by the light and 2)
// can be reached in the cell database from the cell containing the
// light.
void portal_add_simple_dynamic_light(float br, float ambient, Location *loc,
                                     float radius)
{
   int i;
   int cell;

   br *= BRIGHT_SCALE / 2;

   ambient_weight = br;

   cell = CellFromLoc(loc);
   if (cell == CELL_INVALID)
      return;

   PortalSetLightInfo(loc, br * BRIGHT_SCALE, (WR_CELL(cell))->medium);
   light_loc = loc;

   max_dist = radius;
   max_dist_2 = radius * radius;

   num_lit = portal_cells_intersecting_sphere(loc, radius, &lit_cell[0]);
   num_lit = portal_contiguous_cells(lit_cell, num_lit, loc->cell);

   if (!_portal_cell_list_visible(&lit_cell[0], num_lit))
      return;

   for (i = 0; i < num_lit; ++i) {
      int n = lit_cell[i];
      cur_raycast_cell = n;

      portal_dynamic_light(WR_CELL(n), loc);
   }
}


void portal_add_simple_dynamic_dark(float br, float ambient, Location *loc,
                                    float radius)
{
   int i;
   int cell;

   br *= BRIGHT_SCALE / 2;

   ambient_weight = br;

   cell = CellFromLoc(loc);
   if (cell == CELL_INVALID)
      return;

   PortalSetLightInfo(loc, br * BRIGHT_SCALE, (WR_CELL(cell))->medium);
   light_loc = loc;

   max_dist = radius;
   max_dist_2 = radius * radius;

   num_lit = portal_cells_intersecting_sphere(loc, radius, &lit_cell[0]);
   num_lit = portal_contiguous_cells(lit_cell, num_lit, loc->cell);

   for (i = 0; i < num_lit; ++i) {
      int n = lit_cell[i];
      cur_raycast_cell = n;

      portal_dynamic_dark(WR_CELL(n), loc);
   }
}


// Perspective/lighting spans can be as wide as a modern render target.
#define RECIP_TABLE_SIZE 8192
fix reciprocal_table_24[RECIP_TABLE_SIZE+1];
float int_table[32];

void init_portal_renderer(int dark, int light)
{
   int i;

   init_background_hack();

   reciprocal_table_24[0] = 0x7fffffff;

   for (i=1; i <= RECIP_TABLE_SIZE; ++i)
      reciprocal_table_24[i] = fix_make(256,0) / i;

   for (i=0; i < 32; ++i)
      int_table[i] = (float)i;

   init_portal_shading(dark, light);
   init_portal_light();

   // this turns out useless, as gMax ain't initialized yet
   AssertMsg2(HACK_MAX_OBJ>=gMaxObjID,"Portal Using fewer ObjIDs (%d) than object system (%d)\n",HACK_MAX_OBJ,gMaxObjID);
}


// callback needed by lighting system (once upon a time)
#if 0
unsigned char *portal_get_lighting_data(int cell_id)
{
   return WR_CELL(cell_id)->vertex_list_lighting;
}
#endif

bool show_span_lengths, show_render_times;

 int pixel_count[32];
#ifdef STATS_ON

 static void zero_stats(void)
 {
   int undrawn = stat_num_clipped_pixels - stat_num_drawn_pixels;

   if (undrawn > stat_max_undrawn_pixels)
      stat_max_undrawn_pixels = undrawn;

   if (stat_num_polys_clipped_away > stat_max_undrawn_polys) {
      stat_max_undrawn_polys = stat_num_polys_clipped_away;
      stat_max_total_polys = stat_num_poly_drawn;
   }

   stat_num_port = 0;
   stat_num_port_visit = 0;
   stat_num_port_traverse = 0;
   stat_num_cell = 0;
   stat_num_cell_visit = 0;
   stat_num_cell_explored = 0;
   stat_num_facing = 0;
   stat_num_poly_raw = 0;
   stat_num_poly_considered = 0;
   stat_num_poly_drawn = 0;
   stat_num_polys_clipped_away = 0;
   stat_num_lit_pixels = 0;
   stat_num_source_pixels = 0;
   stat_num_dest_pixels = 0;
   stat_num_spans_drawn = 0;
   stat_num_spans_clamped = 0;
   stat_num_backface_tests = 0;
   stat_num_render_ms = 0;
   stat_num_traverse_ms = 0;
   stat_num_object_ms = 0;
   stat_num_sort_ms = 0;
   stat_num_drawn_pixels = 0;
   stat_num_clipped_pixels = 0;
   stat_num_transp_pixels = 0;
   stat_num_objects = 0;
   stat_num_visible_objects = 0;
   stat_num_terrsplit_objects = 0;
   stat_num_resplit_objects = 0;
   stat_num_hidden_objects = 0;
   stat_num_cell_tests = 0;
   stat_num_partial_objects = 0;
   memset(pixel_count, 0, sizeof(pixel_count));
 }

   // The following macro is used internally in the output function.
   // "index" is the variable # of the last message we printed; the
   // current one is found in 'i', so if i <= index, we've already
   // been printed.  Ideally we would just switch() on the value of index,
   // but then we'd have to explicitly assign numbers to each case,
   // instead of making them implicit using i as seen here.

 #define INFO(var, str, my_vol)              \
    if (++i > index && vol >= my_vol) {      \
       index = i;                            \
       sprintf(buffer, str, stat_num_##var); \
       return buffer;                        \
    }

 #define MAXINFO(var, str, my_vol)           \
    if (++i > index && vol >= my_vol) {      \
       index = i;                            \
       sprintf(buffer, str, stat_max_##var); \
       return buffer;                        \
    }

 #define BLANK(my_vol)  if (++i > index && vol >= my_vol) { index = i; return "----"; }

 static char buffer[256];
 char *portal_scene_info(int vol)
 {
    static int index=0;
    int i=0;

    if (++i > index && show_render_times) {
       index = i;
       // note that stat_num_object_ms is sampling based!
       sprintf(buffer,
              "traverse %d ms; objsort %d ms; render %d ms (%d ms on objects)",
                stat_num_traverse_ms, stat_num_sort_ms,
                stat_num_render_ms, stat_num_object_ms);
       return buffer;
    }

    if (g2pt_span_clip) {
       int stat_num_poly_notclipped =
             stat_num_poly_drawn - stat_num_polys_clipped_away;
       INFO(poly_notclipped, "Polygons past span-clip: %d", 5);
    }

    INFO(poly_drawn, "Polygons rendered: %d", 1);
    INFO(poly_considered, "Polygons before portal clip: %d", 20);
    INFO(poly_raw, "Polygons before backfacing: %d", 20);

    BLANK(25);

    if (g2pt_span_clip) {
       int stat_num_undrawn_pixels =
              stat_num_clipped_pixels - stat_num_drawn_pixels;
       INFO(drawn_pixels, "Opaque pixels output: %d", 10);
       INFO(undrawn_pixels, "Opaque Pixels not drawn: %d", 10);
       INFO(transp_pixels, "Transparent pixels output: %d", 10);
    } else {
       INFO(dest_pixels, "Pixels output: %d", 10);
    }
    INFO(lit_pixels, "Surface pixels computed: %d", 3);
    INFO(source_pixels, "Surface pixels used: %d", 5);

    BLANK(40);

    INFO(spans_drawn, "Spans: %d", 15);
    INFO(spans_clamped, "Spans clamped: %d", 30);

    BLANK(20);

    INFO(cell, "Unique cells: %d", 10);
    INFO(cell_explored, "Cells explored: %d", 20);
    INFO(cell_visit, "Cells visited: %d", 30);

    BLANK(30);

    INFO(port, "Portals: %d", 5);
    INFO(port_visit, "Portals visited: %d", 30);
    INFO(port_traverse, "Portals traversed: %d", 30);
    INFO(backface_tests, "Backface checks: %d", 10);

    BLANK(30);

    if (g2pt_span_clip) {
       MAXINFO(undrawn_pixels, "Max span-clipped-away pixels: %d", 10);
       if (++i > index && vol >= 10) {
          index = i;
          sprintf(buffer, "Max span-clipped-away polys: %d of %d (%d percent)",
             stat_max_undrawn_polys, stat_max_total_polys,
             (int)(((float)stat_max_undrawn_polys / (float)stat_max_total_polys) * 100));
          return buffer;
       }
    }

    INFO(objects, "Visible objects: %d", 8);
    INFO(hidden_objects, "Re-hidden objects: %d", 10);
    INFO(partial_objects, "Partially-visible objects: %d", 10);
    INFO(terrsplit_objects, "Terrain-split objects: %d", 10);
    INFO(resplit_objects, "Object-split objects: %d", 10);
    INFO(visible_objects, "Visible object fragments: %d", 10);
    INFO(cell_tests, "object cell tests: %d", 15);

    if (++i > index && show_span_lengths) {
       index = i;
       sprintf(buffer, "%d %d %d %d %d %d %d %d %d %d %d %d",
          pixel_count[0], pixel_count[1], pixel_count[2], pixel_count[3],
          pixel_count[4], pixel_count[5], pixel_count[6], pixel_count[7],
          pixel_count[8], pixel_count[9], pixel_count[10], pixel_count[11]);
       return buffer;
    }

    if (++i > index && show_span_lengths) {
       index = i;
       sprintf(buffer, "%d %d %d %d %d %d %d %d %d %d %d %d",
          pixel_count[12], pixel_count[13], pixel_count[14], pixel_count[15],
          pixel_count[16], pixel_count[17], pixel_count[18], pixel_count[19],
          pixel_count[20], pixel_count[21], pixel_count[22], pixel_count[23]);
       return buffer;
    }

    if (++i > index && show_span_lengths) {
       index = i;
       sprintf(buffer, "%d %d %d %d %d %d %d %d",
          pixel_count[24], pixel_count[25], pixel_count[26], pixel_count[27],
          pixel_count[28], pixel_count[29], pixel_count[30], pixel_count[31]);
       return buffer;
    }

    index=0; // we got to end without printing, so reset to beginning of list
    return 0;
 }

 #undef INFO
 #undef BLANK

#else

 char *portal_scene_info(int vol)
 {
    return 0;
 }

#endif


/*
Local Variables:
typedefs:("ClipData" "LightPermanence" "Location" "ObjID" "ObjRef" "ObjRefID" "ObjVisibleID" "PortalCell" "PortalPolygonCore" "Position" "grs_bitmap" "grs_canvas" "mxs_real" "uchar")
End:
*/
