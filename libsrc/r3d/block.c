// $Header: x:/prj/tech/libsrc/r3d/RCS/block.c 1.5 1996/06/13 11:53:26 JAEMZ Exp $
// This has start and end block

#include <_xfrm.h>
#include <r3ds.h>
#include <matrix.h>

// Requires:  In a context, not in a block 
// Modifies:  r3d_glob and r3d_state
// Effects: sets the block flag, and rebuilds xform
//    matrix o2c and w2c if necessary
//    This also recalculates vino

// Camera space back to world space for the ray tracer: the inverse of the
// current w2c, row-major 3x4 (world = M * camera + t).  Updated whenever w2c
// is rebuilt; r3d_rt_c2w_valid is cleared for linear space.
float r3d_rt_c2w[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
int r3d_rt_c2w_valid = 0;
// Nonzero in R3_PROJECT_SPACE, where a screen offset of one pixel is
// 65536 * z camera units (screen sx = x * w + x_off, in 16.16 fixed point):
// lets lgd3d's screen-space sprites be placed in the world.
int r3d_rt_project_space = 0;
// Nonzero while the ray tracer captures a whole object: every point is coded
// as inside the view, so nothing of it is clipped or culled by the frustum
// (lgd3d never rasterizes captured triangles; their world positions come
// from camera space, which stays valid behind the camera too).
int r3d_rt_noclip = 0;
// Set by md/mm, during such a capture, while drawing a face turned away from
// the camera (which the game would have culled): the ray tracer keeps it for
// shadows only, so coincident two-sided faces do not fight in view.
int r3d_rt_backface = 0;

void r3_rt_zero_codes(int n, r3s_point *src)
{
   uchar *cur = (uchar *)src, *last = cur + n * r3d_glob.cur_stride;
   for (; cur < last; cur += r3d_glob.cur_stride)
      ((r3s_point *)cur)->ccodes = 0;
}

void _r3_rt_update_c2w(void)
{
   const mxs_real *m = X2MAT(&cx.w2c)->el;   // column-major
   const mxs_vector *t = X2VEC(&cx.w2c);
   // rows of M: r0 = (m0 m3 m6), r1 = (m1 m4 m7), r2 = (m2 m5 m8)
   double a = m[0], b = m[3], c = m[6];
   double d = m[1], e = m[4], f = m[7];
   double g = m[2], h = m[5], i = m[8];
   double A = e*i - f*h, B = -(d*i - f*g), C = d*h - e*g;
   double det = a*A + b*B + c*C;
   double inv[9];
   if (det > -1e-12 && det < 1e-12) {
      r3d_rt_c2w_valid = 0;
      return;
   }
   det = 1.0 / det;
   inv[0] = A*det;           inv[1] = -(b*i - c*h)*det; inv[2] = (b*f - c*e)*det;
   inv[3] = B*det;           inv[4] = (a*i - c*g)*det;  inv[5] = -(a*f - c*d)*det;
   inv[6] = C*det;           inv[7] = -(a*h - b*g)*det; inv[8] = (a*e - b*d)*det;
   r3d_rt_c2w[0] = (float)inv[0]; r3d_rt_c2w[1] = (float)inv[1]; r3d_rt_c2w[2] = (float)inv[2];
   r3d_rt_c2w[4] = (float)inv[3]; r3d_rt_c2w[5] = (float)inv[4]; r3d_rt_c2w[6] = (float)inv[5];
   r3d_rt_c2w[8] = (float)inv[6]; r3d_rt_c2w[9] = (float)inv[7]; r3d_rt_c2w[10] = (float)inv[8];
   r3d_rt_c2w[3]  = (float)-(inv[0]*t->x + inv[1]*t->y + inv[2]*t->z);
   r3d_rt_c2w[7]  = (float)-(inv[3]*t->x + inv[4]*t->y + inv[5]*t->z);
   r3d_rt_c2w[11] = (float)-(inv[6]*t->x + inv[7]*t->y + inv[8]*t->z);
   r3d_rt_c2w_valid = 1;
   r3d_rt_project_space = r3d_glob.cur_space == R3_PROJECT_SPACE;
}

void r3_start_block()
{

   TEST_STATE("r3_start_block");

   // if either is true, recalculate vino
   // vino = inv(o2w) * viewer_pos
   if (cx.w2c_invalid || cx.o2c_invalid) {
      mx_trans_tmul_vec(&cx.vino,X2TRANS(&cx.o2w),X2VEC(&cx.view_trans));

      // world to camera built from world,
      // inv(v2w), and space_vec
      // w2c = csp^t * world * inv(v2w)
      // world is truly world space, not view space
      // manipulation

      if (cx.w2c_invalid) {
         if (r3d_glob.cur_space == R3_LINEAR_SPACE) {
            // now we get sneaky, linear space has a full transform it needs
            // to pre-push, not just a wimpy csp

            _r3_recalc_linear();
            mx_mul_trans(X2TRANS(&cx.w2c),X2TRANS(&cx.lin_trans),X2TRANS(&cx.w2v));
         } else {
            // Apply space_vec across the whole transform to scale dest space
            // copy transform over
            cx.w2c = cx.w2v;
            mx_mat_eltmuleq_vec(X2MAT(&cx.w2c),&cx.cspace_vec);
            mx_elmuleq_vec(X2VEC(&cx.w2c),&cx.cspace_vec);
         }   

         cx.w2c_invalid = FALSE;
      }

      // this is just o2c = w2c * o2w
      if (cx.o2c_invalid) {
         mx_mul_trans(X2TRANS(&cx.o2c),X2TRANS(&cx.w2c),X2TRANS(&cx.o2w));

         cx.o2c_invalid = FALSE;
      }
  }


   // The ray tracer's camera-to-world is a global, but each r3d context has
   // its own camera: a second context (an inventory item, a HUD model, the
   // sky) rebuilt it for itself and the main view's next objects were then
   // taken back to the world with that camera -- captured metres from where
   // they are, so they vanished from their place.  Taken from the context
   // in use at every block.
   if (r3d_glob.cur_space == R3_LINEAR_SPACE)
      r3d_rt_c2w_valid = 0;
   else
      _r3_rt_update_c2w();

   r3d_state.in_block = TRUE;
}


// Requires:  In a context, in a block
// Modifies:  r3d_glob and r3d_state
// Effects:  unsets the block flag

void r3_end_block()
{
   #ifdef DBG_ON 
   if (!r3d_state.in_block) { 
      Warning(("r3d: r3_end_block: called outside of a block!\n")); 
      return; 
   } 
   if (r3d_state.cur_con==NULL) { 
      Warning(("r3d: r3_end_block: called outside of a context!\n")); 
      return; 
   }
   #endif

   r3d_state.in_block = FALSE;

}
