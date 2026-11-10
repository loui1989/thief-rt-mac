/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

//////////////////////////////////////////////////////////////////////////////
// $Header: r:/t2repos/thief2/src/physics/phclimb.cpp,v 1.30 1999/12/02 17:15:45 BFarquha Exp $
//
// Physics climbing api functions
//

#include <lg.h>
#include <config.h>
#include <matrix.h>

#include <port.h>

#include <objpos.h>
#include <objedit.h>
#include <textarch.h>
#include <label.h>
#include <schema.h>
#include <plyrmov.h>
#include <plyrmode.h>
#include <playrobj.h>
#include <simtime.h>

#include <physapi.h>
#include <phcore.h>
#include <phmods.h>
#include <phmod.h>
#include <phmodobb.h>
#include <phmodsph.h>
#include <phprop.h>
#include <phcontct.h>
#include <phutils.h>
#include <phmoapi.h>
#include <physcast.h>

#include <phclimb.h>


// Must be last header
#include <dbmem.h>


///////////////////////////////////////

BOOL PhysObjIsClimbing(ObjID objID)
{
   cPhysModel *pModel;

   if ((pModel = g_PhysModels.GetActive(objID)) == NULL)
   {
      Warning(("PhysObjIsClimbing: %s has no active models\n", ObjWarnName(objID)));
      return FALSE;
   }

   return pModel->IsClimbing();
}

///////////////////////////////////////

BOOL PhysObjIsRopeClimbing(ObjID objID)
{
   cPhysModel *pModel;

   if ((pModel = g_PhysModels.GetActive(objID)) == NULL)
   {
      Warning(("PhysObjIsRopeClimbing: %s has no active models\n", ObjWarnName(objID)));
      return FALSE;
   }

   return pModel->IsRopeClimbing();
}

///////////////////////////////////////

BOOL PhysObjIsMantling(ObjID objID)
{
   cPhysModel *pModel;

   if ((pModel = g_PhysModels.GetActive(objID)) == NULL)
   {
      Warning(("PhysObjIsMantling: %s has no active models\n", ObjWarnName(objID)));
      return FALSE;
   }

   return pModel->IsMantling();
}

///////////////////////////////////////

void BreakClimb(ObjID objID, BOOL jumping, BOOL jump_thru)
{
   cPhysModel *pModel;

   if ((pModel = g_PhysModels.GetActive(objID)) == NULL)
   {
      Warning(("BreakClimb: %s has no active physics models\n", ObjWarnName(objID)));
      return;
   }

   if (!pModel->IsPlayer() && !pModel->IsAvatar())
      return;

   ObjID climbing_obj = pModel->GetClimbingObj();

   if (climbing_obj != OBJ_NULL)
   {
      jump_thru = pModel->IsRopeClimbing();

      mxs_vector normal;

      mx_zero_vec(&normal);
      if (GetObjTextureIdx(climbing_obj) >= 0)
      {
         // Texture
         cFaceContactList *pFaceContactList;

         for (int i=0; i<pModel->NumSubModels(); i++)
         {
            if (pModel->GetFaceContacts(i, &pFaceContactList))
            {
               cFaceContact *pFaceContact;

               pFaceContact = pFaceContactList->GetFirst();
               while (pFaceContact)
               {
                  if (pFaceContact->GetObjID() == climbing_obj)
                  {
                     mx_copy_vec(&normal, &pFaceContact->GetNormal());
                     break;
                  }

                  pFaceContact = pFaceContact->GetNext();
               }
            }
         }
      }
      else
      {
         // Object
         cPhysModel *pModel2 = g_PhysModels.Get(climbing_obj);

         if (pModel2)
         {
            if (pModel2->GetType(0) == kPMT_OBB)
            {
               cPhysSubModelInst *pObjContact;

               // Are we close enough to the top to jump through?
               mxs_matrix orien;
               mxs_vector top_pt;
               mxs_vector offset;
               mxs_vector proj_pt;

               mx_ang2mat(&orien, &pModel2->GetRotation());
               mx_scale_add_vec(&top_pt, &pModel2->GetLocationVec(), &orien.vec[2], ((cPhysOBBModel *)pModel2)->GetEdgeLengths().z / 2);

               mx_sub_vec(&offset, &pModel->GetLocationVec(), &pModel2->GetLocationVec());
               mx_scale_add_vec(&proj_pt, &pModel2->GetLocationVec(), &orien.vec[2], mx_dot_vec(&offset, &orien.vec[2]));

               if (mx_dist2_vec(&top_pt, &proj_pt) < 1)
                  jump_thru = TRUE;
               else
               {
                  // Find which submodel we're attached to
                  for (int i=0; i<pModel->NumSubModels() && IsZeroVector(normal); i++)
                  {
                     if (GetObjectContacts(pModel->GetObjID(), i, pModel, &pObjContact))
                     {
                        while (pObjContact)
                        {
                           if (pModel2 == pObjContact->GetPhysicsModel())
                           {
                              cFaceContact faceContact((cPhysOBBModel *)pModel2, pObjContact->GetSubModId());
                              mx_copy_vec(&normal, &faceContact.GetNormal());
                              break;
                           }

                           pObjContact = pObjContact->GetNext();
                        }
                     }
                  }
               }
            }
            else
            if (pModel2->IsRope())
               jump_thru = TRUE;
            else
               Warning(("BreakClimb: unknown physics model type on obj %d\n", pModel2->GetObjID()));
         }
         else
            Warning(("BreakClimb: no physics model on climbing obj %d\n", climbing_obj));
      }

      if (jump_thru)
      {
         mxs_matrix orien;

         mx_ang2mat(&orien, &pModel->GetRotation());
         mx_copy_vec(&normal, &orien.vec[0]);
         mx_scaleeq_vec(&normal, 2.0);
         mx_scale_addeq_vec(&normal, &orien.vec[2], 0.1f);

         cPhysModel *pClimbModel;

         if ((pClimbModel = g_PhysModels.Get(climbing_obj)) == NULL)
         {
            Warning(("BreakClimb: no physics on climbing obj %d?\n", climbing_obj));
            return;
         }

         if (pClimbModel->IsRope())
            pClimbModel->SetObjectPassThru(1.0);
      }

      if (IsZeroVector(normal))
         Warning(("BreakClimb: unable to find climbing object to break from\n"));
      else
      if (jumping)
      {
         mxs_matrix orien;
         mxs_vector jump_dir;

         mx_ang2mat(&orien, &pModel->GetRotation());

         if (mx_dot_vec(&orien.vec[0], &normal) > 0)
         {
            if (jump_thru)
               mx_copy_vec(&jump_dir, &normal);
            else
            {
               // facing away from climbing surface, just jump
               mx_copy_vec(&jump_dir, &orien.vec[0]);
            }
         }
         else
         {
            // reflect from climbing surface
            mx_copy_vec(&jump_dir, &orien.vec[0]);

            mxs_vector into_norm;
            mx_scale_vec(&into_norm, &normal, mx_dot_vec(&jump_dir, &normal)  * 2);

            mx_subeq_vec(&jump_dir, &into_norm);
            mx_scaleeq_vec(&jump_dir, 0.5);
         }

         mxs_vector velocity;

         mx_copy_vec(&velocity, &pModel->GetVelocity());
         mx_scale_addeq_vec(&velocity, &jump_dir, 5.0);
         pModel->GetDynamics()->SetVelocity(velocity);
      }

      for (int i=0; i<pModel->NumSubModels(); i++)
      {
         pModel->DestroyAllTerrainContacts(i);
         DestroyAllObjectContacts(objID, i, pModel);
      }

      if (pModel->IsPlayer())
         pModel->SetSpringTension(PLAYER_HEAD, DEFAULT_SPRING_TENSION);

      pModel->SetClimbingObj(OBJ_NULL);
      pModel->SetRopeSegment(-1);

      g_pPlayerMovement->SetGroundObj(OBJ_NULL);

      if (jumping)
         g_pPlayerMode->SetMode(kPM_Jump);
      else
         g_pPlayerMode->SetMode(kPM_Stand);
   }
}

////////////////////////////////////////

BOOL CheckClimb(ObjID objID)
{
   cPhysModel *pModel;

   if ((pModel = g_PhysModels.GetActive(objID)) == NULL)
   {
      Warning(("CheckClimb: %s has no active physics models\n", ObjWarnName(objID)));
      return FALSE;
   }

   if ((!pModel->IsPlayer() && !pModel->IsAvatar()) || pModel->IsGhost())
      return FALSE;

   mxs_matrix orien;

   mx_ang2mat(&orien, &pModel->GetRotation());

   cFaceContactList *pFaceContactList;

   for (int i=0; i<pModel->NumSubModels(); i++)
   {
      if (pModel->GetFaceContacts(i, &pFaceContactList))
      {
         cFaceContact *pFaceContact;

         pFaceContact = pFaceContactList->GetFirst();
         while (pFaceContact)
         {
            if (FALSE) // don't climb on walls, spidey
            {
               if (mx_dot_vec(&orien.vec[0], &pFaceContact->GetNormal()) <= 0.0)
               {
                  // Set the internal info
                  pModel->SetClimbingObj(pFaceContact->GetObjID());

                  // Update constraints
                  ConstrainFromTerrain(pModel, i);

                  if (PlayerObjectExists() && (objID == PlayerObject()))
                  {
                     mxs_vector foot_loc;
                     PhysGetSubModLocation(PlayerObject(), PLAYER_FOOT, &foot_loc);

                     g_pPlayerMode->SetMode(kPM_Climb);
                     g_pPlayerMovement->SetGroundObj(pFaceContact->GetObjID());
                     g_pPlayerMovement->SetLastFoot(foot_loc, GetSimTime());
                  }

                  return TRUE;
               }
            }

            pFaceContact = pFaceContact->GetNext();
         }
      }

      cPhysSubModelInst *pObjContact;

      if (GetObjectContacts(pModel->GetObjID(), i, pModel, &pObjContact))
      {
         cPhysModel *pModel2;
         int j;

         while (pObjContact)
         {
            pModel2 = pObjContact->GetPhysicsModel();
            j = pObjContact->GetSubModId();

            if (pModel2->GetType(0) == kPMT_OBB)
            {
               if (((cPhysOBBModel *)pModel2)->GetClimbableSides() & (1 << j))
               {
                  mxs_vector face_normal;

                  ((cPhysOBBModel *)pModel2)->GetNormal(j, &face_normal);

                  if (mx_dot_vec(&orien.vec[0], &face_normal) <= 0.0)
                  {
                     // Set the internal info
                     pModel->SetClimbingObj(pModel2->GetObjID());

                     // Update constraints
                     ConstrainFromObjects(pModel, i);

                     if (PlayerObjectExists() && (objID == PlayerObject()))
                     {
                        mxs_vector foot_loc;
                        PhysGetSubModLocation(PlayerObject(), PLAYER_FOOT, &foot_loc);

                        g_pPlayerMode->SetMode(kPM_Climb);
                        g_pPlayerMovement->SetGroundObj(pModel2->GetObjID());
                        g_pPlayerMovement->SetLastFoot(foot_loc, GetSimTime());
                     }

                     return TRUE;
                  }
               }
            }
            else
            if (pModel2->IsRope())
            {
               mxs_vector to_rope;

               mx_sub_vec(&to_rope, &pModel2->GetLocationVec(j), &pModel->GetLocationVec(i));
               if (mx_dot_vec(&orien.vec[0], &to_rope) > 0)
               {
                  pModel->SetClimbingObj(pModel2->GetObjID());

                  if (j < 1)
                     pModel->SetRopeSegment(1);
                  else
                  if (j > pModel2->NumSubModels())
                     pModel->SetRopeSegment(pModel2->NumSubModels());
                  else
                     pModel->SetRopeSegment(j);

                  ConstrainFromObjects(pModel, i);

                  mxs_vector vel, add_vel;
                  mx_copy_vec(&vel, &pModel2->GetVelocity(j));

                  if (pModel->IsPlayer())
                     mx_scale_vec(&add_vel, &pModel->GetVelocity(PLAYER_HEAD), 5.0);
                  else
                     mx_scale_vec(&add_vel, &pModel->GetVelocity(), pModel->GetDynamics()->GetMass() * 0.1f);
                  add_vel.z = 0;
                  mx_addeq_vec(&vel, &add_vel);
                  pModel2->GetDynamics(j)->SetVelocity(vel);

                  if (PlayerObjectExists() && (objID == PlayerObject()))
                  {
                     mxs_vector foot_loc;
                     PhysGetSubModLocation(PlayerObject(), PLAYER_FOOT, &foot_loc);

                     g_pPlayerMode->SetMode(kPM_Climb);
                     g_pPlayerMovement->SetGroundObj(pModel2->GetObjID());
                     g_pPlayerMovement->SetLastFoot(foot_loc, GetSimTime());
                  }

                  return TRUE;
               }
            }

            pObjContact = pObjContact->GetNext();
         }
      }
   }

   return FALSE;
}

////////////////////////////////////////

// Set when the mantle under way ends in a crouch (see CheckMantle).
static BOOL g_MantleCrouch = FALSE;
// A mantle broken on the way up (the body knocking the ledge's edge -- run
// into a table with jump held): not tried again by the retry below until
// this sim time, or it began again every frame, the grunt with it, the
// player hung against the ledge until jump was let go.
static tSimTime g_MantleNoRetryUntil = 0;
// where the crouched shape was found to fit, for the end of the mantle
static mxs_vector g_MantleCrouchDest;

// One line in the log per mantle attempt that ends differently from the last
// (the retry below checks every frame, so repeats are dropped).
static void MantleLog(const char *what, const char *detail = NULL)
{
   static const char *s_last = NULL;
   static int s_lines = 0;
   if (what == s_last || s_lines >= 400)
      return;
   s_last = what;
   ++s_lines;
   fprintf(stderr, "Mantle: %s%s%s\n", what, detail ? " -- " : "", detail ? detail : "");
}

// The ledge's height over the player's feet and the room above it, for the log.
static const char *LedgeDetail(cPhysModel *pModel, const mxs_vector &ledge, const mxs_vector &dir)
{
   static char text[160];
   Location start, end, hit;
   ObjID hit_obj = 0;
   mxs_vector at = ledge;
   mx_scale_addeq_vec(&at, &dir, 0.5f);
   at.z += 0.1f;
   MakeLocationFromVector(&start, &at);
   ComputeCellForLocation(&start);
   MakeLocationFromVector(&end, &at);
   end.vec.z += 8.0f;
   float room = 8.0f;
   if (PhysRaycast(start, end, &hit, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone)
      room = hit.vec.z - ledge.z;
   snprintf(text, sizeof(text), "ledge %.2f ft above the feet, %.2f ft of room over it (standing needs 6.4, crouched 4.0)",
            ledge.z - pModel->GetLocationVec(PLAYER_FOOT).z, room);
   return text;
}

void BreakMantle(ObjID objID)
{
   cPhysModel *pModel;
   mxs_vector offset;

   if ((pModel = g_PhysModels.GetActive(objID)) == NULL)
   {
      Warning(("BreakMantle: %s has no active physics models\n", ObjWarnName(objID)));
      return;
   }

   if (!pModel->IsPlayer())
      return;

   mx_mk_vec(&offset, 0, 0, PLAYER_HEAD_POS);
   pModel->SetSubModOffset(PLAYER_HEAD, offset);
   mx_mk_vec(&offset, 0, 0, PLAYER_BODY_POS);
   pModel->SetSubModOffset(PLAYER_BODY, offset);
   mx_mk_vec(&offset, 0, 0, PLAYER_KNEE_POS);
   pModel->SetSubModOffset(PLAYER_KNEE, offset);
   mx_mk_vec(&offset, 0, 0, PLAYER_SHIN_POS);
   pModel->SetSubModOffset(PLAYER_SHIN, offset);
   mx_mk_vec(&offset, 0, 0, PLAYER_FOOT_POS);
   pModel->SetSubModOffset(PLAYER_FOOT, offset);

   pModel->SetSpringTension(PLAYER_HEAD, DEFAULT_SPRING_TENSION);

   PhysSetModLocation(objID, (mxs_vector *)&pModel->GetLocationVec());

   // only a mantle that got over the ledge (state 3, moving onto it, or
   // later) ends crouched; one let go of earlier drops back standing. (Only
   // state 4 on: the low ceiling a crouched mantle squeezes under was
   // touched as it went forward, the mantle broken there, and it ended
   // standing -- under the ceiling, and pushed back out of the space.)
   const BOOL end_crouched = g_MantleCrouch && pModel->GetMantlingState() >= 3;
   g_MantleCrouch = FALSE;
   if (pModel->GetMantlingState() > 0 && pModel->GetMantlingState() < 3)
   {
      g_MantleNoRetryUntil = GetSimTime() + SIM_TIME_SECOND * 6 / 10;
      MantleLog("mantle broken on the way up (the body met the ledge)");
   }

   PlayerMotionActivate(kMoEnable);
   pModel->SetMantlingState(0);

   // A mantle into a space too low to stand in ends crouched: the crouch
   // motion's offsets are put in place at once, so the shape that is placed
   // and tested below is the crouched one.
   if (end_crouched)
   {
      g_pPlayerMode->SetMode(kPM_Crouch);
      mxs_vector head_lower = {0, 0, -2.02f};
      mxs_vector body_lower = {0, 0, -1.0f};
      PlayerMotionSetOffset(PLAYER_HEAD, &head_lower);
      PlayerMotionSetOffset(PLAYER_BODY, &body_lower);
      PhysSetModLocation(objID, (mxs_vector *)&pModel->GetLocationVec());
      // Not fitting where the climb left it (its head an inch into the low
      // ceiling, or short of the ledge): a little lower first, then the
      // place the crouched shape was found to fit -- not the search below,
      // which steps back out of the space it was climbing into.
      if (!PhysObjValidPos(objID, NULL))
      {
         BOOL placed = FALSE;
         for (float down = 0.05f; down <= 0.31f && !placed; down += 0.05f)
         {
            mxs_vector d = {0, 0, -down};
            if (PhysObjValidPos(objID, &d))
            {
               mxs_vector at = pModel->GetLocationVec();
               at.z -= down;
               PhysSetModLocation(objID, &at);
               placed = TRUE;
            }
         }
         if (!placed)
         {
            mxs_vector d;
            mx_sub_vec(&d, &g_MantleCrouchDest, &pModel->GetLocationVec());
            if (mx_mag2_vec(&d) < 5.0f * 5.0f && PhysObjValidPos(objID, &d))
               PhysSetModLocation(objID, &g_MantleCrouchDest);
         }
      }
      MantleLog("mantle finished crouched");
   }

   mxs_matrix orien;
   mx_ang2mat(&orien, &pModel->GetRotation());

   mxs_vector target_loc = pModel->GetLocationVec();
   mxs_vector delta;

   BOOL found_valid = PhysObjValidPos(objID, NULL);

   for (float dist = 0.1f; (dist < 4.0f) && !found_valid; dist += 0.1f)
   {
      mx_scale_add_vec(&target_loc, &pModel->GetLocationVec(), &orien.vec[0], -dist);
      mx_sub_vec(&delta, &target_loc, &pModel->GetLocationVec());

      if (PhysObjValidPos(objID, &delta))
         found_valid = TRUE;

      if (!found_valid)
      {
         mx_scale_add_vec(&target_loc, &pModel->GetLocationVec(), &orien.vec[2], dist);
         mx_sub_vec(&delta, &target_loc, &pModel->GetLocationVec());

         if (PhysObjValidPos(objID, &delta))
            found_valid = TRUE;
      }
   }

   if (found_valid)
      PhysSetModLocation(objID, &target_loc);
}

////////////////////////////////////////

static inline BOOL CanMantleOnto(ObjID objid)
{
   BOOL bCanMantle;

   if (!g_pPhysCanMantleProp || !g_pPhysCanMantleProp->Get(objid, &bCanMantle))
      return TRUE;

   return bCanMantle;
}


////////////////////////////////////////

// NewDark's new_mantle: a mantle is taken when the player can move at least
// new_mantle_fwd (0.5-1.0, default 0.5) of the usual distance onto the ledge,
// rather than only the whole of it -- the usual reason a mantle was refused,
// or had to be retried from another angle. The forward move is scaled to match.
static float g_MantleForward = 1.0f;

// On unless "new_mantle 0": NewDark's own cam_ext.cfg turns it on.
static BOOL NewMantle()
{
   int value;
   if (!config_is_defined("new_mantle"))
      return TRUE;
   return !config_get_int("new_mantle", &value) || value != 0;
}

// The object a mantle under way is onto (a statue, a crate, a table: none
// for the world's own ledges). Climbing an irregular one -- Malazar's
// Tower's statue, its edge not under its top -- the body met its side on
// the way up and the mantle broke, every time. Until the mantle is over the
// body passes it (as it passes a ladder); where it ends was found free of
// it before the mantle began.
static ObjID g_MantleOnto = OBJ_NULL;

BOOL PhysMantleIgnores(ObjID mantler, ObjID other)
{
   cPhysModel *pModel = g_PhysModels.GetActive(mantler);
   if (!pModel || !pModel->IsMantling() || other == OBJ_NULL)
      return FALSE;
   return other == g_MantleOnto || PhysObjIsLadder(other);
}

BOOL PhysObjIsLadder(ObjID objID)
{
   if (objID == OBJ_NULL)
      return FALSE;
   cPhysModel *pModel = g_PhysModels.Get(objID);
   return pModel && pModel->GetType(0) == kPMT_OBB &&
          ((cPhysOBBModel *)pModel)->GetClimbableSides() != 0;
}

// A mantle looks past ladders: from a ladder's top, or jumping beside one,
// the ladder itself was taken for the ledge (and the wall in front of the
// head): a climb onto the ladder began, the ladder broke it, and the player
// was put back -- off the ladder, falling. The ledge is the floor beyond it.
static BOOL MantleCastSkipsLadders(ObjID obj, const cPhysModel *, void *)
{
   return !PhysObjIsLadder(obj);
}

static BOOL CheckMantleCasts(ObjID objID);
BOOL CheckMantle(ObjID objID)
{
   PhysRaycastSetCallback(MantleCastSkipsLadders, NULL);
   const BOOL result = CheckMantleCasts(objID);
   PhysRaycastClearCallback();
   return result;
}

static BOOL CheckMantleCasts(ObjID objID)
{
   cPhysModel *pModel;

   if ((pModel = g_PhysModels.GetActive(objID)) == NULL)
   {
      Warning(("CheckMantle: %s has no active physics models\n", ObjWarnName(objID)));
      return FALSE;
   }

   if (!pModel->IsPlayer() || !g_pPlayerMode->IsJumping())
      return FALSE;

   // Already on the way up: a new contact during the climb restarted it
   // from the first stage with a fresh target.
   if (pModel->IsMantling())
      return TRUE;

   mxs_matrix orien;
   mxs_vector ledge_offset;
   mxs_vector movement_dir;
   mxs_real   ledge_up = 0.02f;
   mxs_real   radius = ((cPhysSphereModel *)pModel)->GetRadius(PLAYER_HEAD);
   ObjID      hit_obj = 0;

   Location start_loc, end_loc, hit_loc, hit_loc2;

   mx_ang2mat(&orien, &pModel->GetRotation());

   mx_copy_vec(&movement_dir, &orien.vec[0]);
   movement_dir.z = 0;
   if (!IsZeroVector(movement_dir))
      mx_normeq_vec(&movement_dir);
   mx_scaleeq_vec(&movement_dir, radius * 2);

   // Look for the ledge 3.5 ft above the head, as the original did, then
   // lower: a window's opening is often only a few feet tall, so by the time
   // the head is near its sill, 3.5 ft above it is wall again and the
   // original probe never saw the opening.  The downward cast still reaches
   // 3.5 ft below the head whichever height it starts at.
   static const mxs_real probe_heights[] = {3.5f, 2.75f, 2.0f, 1.4f};
   const char *why = "no ledge found within reach";
   BOOL found_ledge = FALSE;
   mxs_real probe_up = 3.5f;

   for (int p = 0; p < 4 && !found_ledge; ++p)
   {
      probe_up = probe_heights[p];
      mx_mk_vec(&ledge_offset, 0, 0, probe_up);

      MakeHintedLocationFromVector(&start_loc, &pModel->GetLocationVec(PLAYER_HEAD), &pModel->GetLocation(PLAYER_HEAD));
      MakeHintedLocationFromVector(&end_loc, &start_loc.vec, &start_loc);
      mx_addeq_vec(&end_loc.vec, &ledge_offset);

      // Cast up
      hit_obj = 0;
      if (PhysRaycast(start_loc, end_loc, &hit_loc, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone)
      {
         why = "no room above the head";
         continue;
      }

      // Cast forward
      MakeHintedLocationFromVector(&start_loc, &end_loc.vec, &start_loc);
      mx_addeq_vec(&end_loc.vec, &movement_dir);

      hit_obj = 0;
      if (PhysRaycast(start_loc, end_loc, &hit_loc, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone)
      {
         why = "wall in front above the head, no opening found";
         continue;
      }

      // Cast downward, to 3.5 ft below the head
      MakeHintedLocationFromVector(&start_loc, &end_loc.vec, &start_loc);
      end_loc.vec.z -= probe_up + 3.5f;

      hit_obj = 0;
      if ((PhysRaycast(start_loc, end_loc, &hit_loc, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone) && CanMantleOnto(hit_obj))
      {
         found_ledge = TRUE;
         g_MantleOnto = hit_obj;
      }
      else
         why = "no ledge found within reach";
   }

   if (!found_ledge)
      MantleLog(why);

   if (found_ledge)
   {
      // Find forward surface
      MakeHintedLocationFromVector(&start_loc, &pModel->GetLocationVec(PLAYER_HEAD), &pModel->GetLocation());
      MakeLocationFromVector(&end_loc, &start_loc.vec);

      ComputeCellForLocation(&start_loc);

      mx_addeq_vec(&end_loc.vec, &movement_dir);

      hit_obj = 0;
      if ((PhysRaycast(start_loc, end_loc, &hit_loc2, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone) && CanMantleOnto(hit_obj))
      {
         mxs_vector target_pos;

         mx_copy_vec(&target_pos, &hit_loc2.vec);
         mx_scale_addeq_vec(&target_pos, &movement_dir, -0.55f); // pull back a radius
         target_pos.z = hit_loc.vec.z + (radius * 1.02f) - PLAYER_HEAD_POS + 1.0f; // and just over the lip

         pModel->SetMantlingTargVec(target_pos);
         pModel->SetMantlingState(1);

      }
      else
      {
         mx_copy_vec(&start_loc.vec, &pModel->GetLocationVec(PLAYER_BODY));
         mx_add_vec(&end_loc.vec, &start_loc.vec, &movement_dir);

         hit_obj = 0;
         if ((PhysRaycast(start_loc, end_loc, &hit_loc2, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone) && CanMantleOnto(hit_obj))
         {
            mxs_vector target_pos;

            mx_copy_vec(&target_pos, &hit_loc2.vec);
            mx_scale_addeq_vec(&target_pos, &movement_dir, -0.55f); // pull back a radius
            target_pos.z = hit_loc.vec.z + (radius * 1.02f) - PLAYER_HEAD_POS + 1.0f; // and just over the lip

            pModel->SetMantlingTargVec(target_pos);
            pModel->SetMantlingState(1);

         }
         else
         {
            mx_copy_vec(&start_loc.vec, &pModel->GetLocationVec(PLAYER_FOOT));
            mx_add_vec(&end_loc.vec, &start_loc.vec, &movement_dir);

            hit_obj = 0;
            if ((PhysRaycast(start_loc, end_loc, &hit_loc2, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone) && CanMantleOnto(hit_obj))
            {
               mxs_vector target_pos;

               mx_copy_vec(&target_pos, &hit_loc2.vec);
               mx_scale_addeq_vec(&target_pos, &movement_dir, -0.55f); // pull back a radius
               target_pos.z = hit_loc.vec.z + (radius * 1.02f) - PLAYER_HEAD_POS + 1.0f; // and just over the lip

               pModel->SetMantlingTargVec(target_pos);
               pModel->SetMantlingState(1);

            }
            else
            {
               #ifndef SHIP
               if (config_is_defined("MantleSpew"))
                  mprintf("Mantle forward surface find failed, trying best guess\n");
               #endif

               mxs_vector target_pos;

               mx_copy_vec(&target_pos, &pModel->GetLocationVec(PLAYER_BODY));
               target_pos.z = hit_loc.vec.z + (radius * 1.02f) - PLAYER_HEAD_POS + 1.0f;

               pModel->SetMantlingTargVec(target_pos);
               pModel->SetMantlingState(1);

            }
         }
      }
   }

   if (pModel->IsMantling() && !PhysObjValidPos(objID, NULL))
   {
      // wedged in something already (the body pushed into a table's edge):
      // a climb from here only hangs, grunting -- the physics frees it first
      MantleLog("wedged in something, not mantling from here");
      pModel->SetMantlingState(0);
      return FALSE;
   }

   if (pModel->IsMantling())
   {
      mxs_vector target_pos;
      mxs_vector delta;

      pModel->GetMantlingTargVec(&target_pos);

      mx_sub_vec(&delta, &target_pos, &pModel->GetLocationVec());
      if (!PhysObjValidPos(objID, &delta))
      {
         // The spot just back from the wall can be blocked (a sill or frame
         // standing out from it): try further back, then straight up from
         // where the player is now.
         static const mxs_real back[] = {0.25f, 0.5f};
         for (int k = 0; k < 3; ++k)
         {
            mxs_vector alt = target_pos;
            if (k < 2)
               mx_scale_addeq_vec(&alt, &movement_dir, -back[k]);
            else
            {
               alt.x = pModel->GetLocationVec().x;
               alt.y = pModel->GetLocationVec().y;
            }
            mx_sub_vec(&delta, &alt, &pModel->GetLocationVec());
            if (PhysObjValidPos(objID, &delta))
            {
               target_pos = alt;
               pModel->SetMantlingTargVec(target_pos);
               break;
            }
         }
      }
      if (!PhysObjValidPos(objID, &delta))
      {
         #ifndef SHIP
         if (config_is_defined("MantleSpew"))
            mprintf("Aborting mantle, can't fit in target position\n");
         #endif

         // nothing has been started yet: just drop the target
         MantleLog("can't reach the ledge's lip", LedgeDetail(pModel, hit_loc.vec, movement_dir));
         pModel->SetMantlingState(0);
         return FALSE;
      }

     mxs_vector delta_full = delta;
     // Where it fits, NewDark's way: standing all the way onto the ledge;
     // else crouched all the way (a vent, a crawlspace over a wardrobe);
     // else standing or crouched part of the way (new_mantle_fwd, a narrow
     // ledge). Standing part of the way came before crouching all the way:
     // over a wardrobe with a crawlspace behind its lip it fitted standing
     // at the lip, and the mantle left the player standing there, unable to
     // go in. And a ledge whose room to stand ends just past the lip, with
     // room to crouch on: crouched, as the way on is.
     float min_fwd = 0.5f;
     if (NewMantle())
     {
        config_get_float("new_mantle_fwd", &min_fwd);
        min_fwd = min(1.0f, max(0.5f, min_fwd));
     }
     else
        min_fwd = 1.0f;
     auto stand_at = [&](float f) {
        mxs_vector d = delta_full;
        mx_scale_addeq_vec(&d, &movement_dir, f);
        d.z += -PLAYER_FOOT_POS;
        return PhysObjValidPos(objID, &d);
     };
     auto crouch_at = [&](float f, mxs_vector *out) {
        mxs_vector d = delta_full;
        mx_scale_addeq_vec(&d, &movement_dir, f);
        d.z += -PLAYER_FOOT_POS - 0.40f;
        if (!PhysPlayerValidPosCrouched(objID, &d))
           return FALSE;
        if (out)
           *out = d;
        return TRUE;
     };
     g_MantleForward = 1.0f;
     g_MantleCrouch = FALSE;
     BOOL fits = FALSE;
     mxs_vector crouch_delta;
     for (int pass = 0; pass < 2 && !fits; ++pass)
     {
        // pass 0: all the way; pass 1: part of the way (new mantle only)
        const float hi = pass == 0 ? 1.0f : 0.875f, lo = pass == 0 ? 1.0f : min_fwd;
        if (pass == 1 && !NewMantle())
           break;
        for (float f = hi; f >= lo - 0.001f && !fits; f -= 0.125f)
           if (stand_at(f))
           {
              fits = TRUE;
              g_MantleForward = f;
           }
        for (float f = hi; f >= lo - 0.001f && !fits && NewMantle(); f -= 0.125f)
           if (crouch_at(f, &crouch_delta))
           {
              fits = TRUE;
              g_MantleForward = f;
              g_MantleCrouch = TRUE;
           }
     }
     if (fits && !g_MantleCrouch && NewMantle())
     {
        // standing room only at the lip, crouching room on past it
        const float on = g_MantleForward + 0.75f;
        mxs_vector d;
        if (!stand_at(on) && crouch_at(on, NULL) && crouch_at(g_MantleForward, &d))
        {
           g_MantleCrouch = TRUE;
           crouch_delta = d;
        }
     }
     if (g_MantleCrouch)
        mx_add_vec(&g_MantleCrouchDest, &pModel->GetLocationVec(), &crouch_delta);
     if (!fits)
     {
         #ifndef SHIP
         if (config_is_defined("MantleSpew"))
            mprintf("Aborting mantle, can't fit in target position\n");
         #endif

         MantleLog("no room on the ledge, standing or crouched", LedgeDetail(pModel, hit_loc.vec, movement_dir));
         g_MantleCrouch = FALSE;
         pModel->SetMantlingState(0);
         return FALSE;
      }

      // It fits: start the climb -- from a standstill: running into the
      // ledge, the run carried the body on into its edge as it rose, which
      // broke the climb (as standing at the ledge and jumping does not)
      {
         mxs_vector vel;
         PhysGetVelocity(objID, &vel);
         vel.x = vel.y = 0;
         if (vel.z < 0)
            vel.z = 0;
         PhysSetVelocity(objID, &vel);
      }
      PlayerMotionActivate(kMoDisable);
      Label grunt = {"garclimb"};
      SchemaPlay(&grunt, NULL, NULL);

      MantleLog(g_MantleCrouch ? "mantling, crouched" : "mantling", LedgeDetail(pModel, hit_loc.vec, movement_dir));
      return TRUE;
   }

   return FALSE;
}

// The mantle is checked when the player first touches something while
// airborne with jump held -- once.  Touching the wall a little too low, the
// check fails and nothing re-checks it while the player keeps rising against
// the same wall, so a held jump did nothing.  While jump is held in the air
// with something in front, check again every frame.
static void MantleRetry(cPhysModel *pModel)
{
   if (!pModel->IsPlayer() || pModel->IsMantling() || pModel->IsClimbing() ||
       !g_pPlayerMode->IsJumping() || g_pPlayerMode->GetMode() == kPM_Dead ||
       g_pPlayerMode->GetMode() == kPM_Swim || g_pPlayerMovement->GetGroundObj() != OBJ_NULL ||
       PhysObjInWater(pModel->GetObjID()) || GetSimTime() < g_MantleNoRetryUntil)
      return;

   // something right in front, at the body or the feet
   mxs_matrix orien;
   mxs_vector ahead;
   mx_ang2mat(&orien, &pModel->GetRotation());
   mx_copy_vec(&ahead, &orien.vec[0]);
   ahead.z = 0;
   if (IsZeroVector(ahead))
      return;
   mx_normeq_vec(&ahead);
   mx_scaleeq_vec(&ahead, PLAYER_RADIUS * 2);

   static const int parts[2] = {PLAYER_BODY, PLAYER_FOOT};
   BOOL wall = FALSE;
   for (int k = 0; k < 2 && !wall; ++k)
   {
      Location start, end, hit;
      ObjID hit_obj = 0;
      MakeHintedLocationFromVector(&start, &pModel->GetLocationVec(parts[k]), &pModel->GetLocation(parts[k]));
      MakeHintedLocationFromVector(&end, &start.vec, &start);
      mx_addeq_vec(&end.vec, &ahead);
      // (a ladder in front is no wall to climb: CheckMantle)
      PhysRaycastSetCallback(MantleCastSkipsLadders, NULL);
      if (PhysRaycast(start, end, &hit, &hit_obj, 0, kCollideOBB | kCollideTerrain) != kCollideNone)
         wall = TRUE;
      PhysRaycastClearCallback();
   }
   if (wall)
      CheckMantle(pModel->GetObjID());
}

////////////////////////////////////////

void UpdateMantling(cPhysModel *pModel, mxs_real dt)
{
   static mxs_real delta_t = 0;
   ObjID  hit_obj;

   if (pModel->GetMantlingState() == 0)
   {
      MantleRetry(pModel);
      return;
   }

   switch (pModel->GetMantlingState())
   {
      case 1:
      {
         delta_t += dt;

         if (delta_t > 0.3)
         {
            delta_t = 0;
            pModel->SetMantlingState(2);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d going to mantling state %d\n", pModel->GetObjID(), 2);
            #endif
         }
         else
         {
            mxs_vector targ_vec;

            targ_vec.x = pModel->GetLocationVec(PLAYER_HEAD).x;
            targ_vec.y = pModel->GetLocationVec(PLAYER_HEAD).y;
            targ_vec.z = pModel->GetLocationVec().z;

            pModel->SetTargetLocation(targ_vec);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d in mantling state %d\n", pModel->GetObjID(), 1);
            #endif
         }
         break;
      }

      // Rising vertically, in order to get our head in the proper position
      case 2:
      {
         mxs_vector delta_pos;
         mxs_vector targ_vec;

         // Set up our target location
         pModel->GetMantlingTargVec(&targ_vec);
         pModel->SetTargetLocation(targ_vec);

         mx_sub_vec(&delta_pos, &pModel->GetTargetLocation(PLAYER_HEAD), &pModel->GetLocationVec(PLAYER_HEAD));

         // Are we ready to move on?
         if (mx_mag2_vec(&delta_pos) < 0.0001)
         {
            mxs_vector zero;
            mx_zero_vec(&zero);

            // Shrink inside a single sphere
            pModel->SetLocationVec(pModel->GetLocationVec(PLAYER_HEAD));
            pModel->SetSubModOffset(PLAYER_HEAD, zero);
            pModel->SetSubModOffset(PLAYER_BODY, zero);
            pModel->SetSubModOffset(PLAYER_KNEE, zero);
            pModel->SetSubModOffset(PLAYER_SHIN, zero);
            pModel->SetSubModOffset(PLAYER_FOOT, zero);

            mxs_matrix orien;
            mxs_vector facing;
            mxs_vector targ_loc;

            // Find our forward-moving target
            mx_ang2mat(&orien, &pModel->GetRotation());
            mx_copy_vec(&targ_loc, &pModel->GetLocationVec());

            mx_copy_vec(&facing, &orien.vec[0]);
            facing.z = 0;
            if (!IsZeroVector(facing))
               mx_normeq_vec(&facing);

            mx_scale_addeq_vec(&targ_loc, &facing, PLAYER_RADIUS * 2 * g_MantleForward);

            pModel->SetMantlingTargVec(targ_loc);
            pModel->SetTargetLocation(pModel->GetLocationVec(PLAYER_HEAD));

            pModel->SetSpringTension(PLAYER_HEAD, 0.02f * DEFAULT_SPRING_TENSION);

            pModel->SetMantlingState(3);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d going to mantling state %d\n", pModel->GetObjID(), 3);
            #endif

            delta_t = 0;
         }
         else
         {
            // Adjust spring tension for smooth transition
            mxs_real spring_amt = mx_mag2_vec(&delta_pos);

            if (spring_amt < 1)
               spring_amt = 1.0;
            else
               spring_amt = 1.0f / spring_amt;

            pModel->SetSpringTension(PLAYER_HEAD, spring_amt * DEFAULT_SPRING_TENSION);

            pModel->SetTargetLocation(targ_vec);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d in mantling state %d\n", pModel->GetObjID(), 2);
            #endif
         }

         break;
      }

      // Mashed into a ball, moving forward
      case 3:
      {
         mxs_vector targ_vec;

         // Set up our target location
         pModel->GetMantlingTargVec(&targ_vec);
         pModel->SetTargetLocation(targ_vec);

         delta_t += dt;

         if (delta_t > 0.4)
         {
            mxs_vector targ_loc;
            mxs_vector offset;

            mx_mk_vec(&targ_loc, 0, 0, -PLAYER_FOOT_POS - PLAYER_RADIUS - 1.0f);
            mx_addeq_vec(&targ_loc, &pModel->GetLocationVec());

            mx_mk_vec(&offset, 0, 0, PLAYER_HEAD_POS - (g_MantleCrouch ? 2.02f : 0.0f));
            pModel->SetSubModOffset(PLAYER_HEAD, offset);
            if (g_MantleCrouch)
            {
               // keep the body under the low ceiling while rising
               mx_mk_vec(&offset, 0, 0, -0.5f);
               pModel->SetSubModOffset(PLAYER_BODY, offset);
            }

            pModel->SetMantlingTargVec(targ_loc);
            pModel->SetTargetLocation(targ_loc);

            Label grunt = {"garclimbend"};
            SchemaPlay(&grunt, NULL, NULL);

            pModel->SetMantlingState(4);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d going to mantling state %d\n", pModel->GetObjID(), 4);
            #endif

            delta_t = 0;
         }
         else
         {
            mxs_vector vel;

            mx_copy_vec(&vel, &pModel->GetVelocity(PLAYER_HEAD));
            mx_scaleeq_vec(&vel, 10.0);
            pModel->GetDynamics(PLAYER_HEAD)->SetVelocity(vel);
            pModel->UpdateEndLocation(PLAYER_HEAD, dt);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d in mantling state %d\n", pModel->GetObjID(), 3);
            #endif
         }

         break;
      }

      // Mashed into a ball, moving upward
      case 4:
      {
         mxs_vector delta_pos;
         mxs_vector targ_vec;

         // Set up our target location
         pModel->GetMantlingTargVec(&targ_vec);
         pModel->SetTargetLocation(targ_vec);

         mx_sub_vec(&delta_pos, &pModel->GetTargetLocation(PLAYER_HEAD), &pModel->GetLocationVec(PLAYER_HEAD));

         // Are we ready to move on?
         if (mx_mag2_vec(&delta_pos) < 0.0001)
         {
            mxs_vector offset;

            mx_mk_vec(&offset, 0, 0, PLAYER_BODY_POS - (g_MantleCrouch ? 1.0f : 0.0f));
            pModel->SetSubModOffset(PLAYER_BODY, offset);
            mx_mk_vec(&offset, 0, 0, PLAYER_KNEE_POS);
            pModel->SetSubModOffset(PLAYER_KNEE, offset);
            mx_mk_vec(&offset, 0, 0, PLAYER_SHIN_POS);
            pModel->SetSubModOffset(PLAYER_SHIN, offset);
            mx_mk_vec(&offset, 0, 0, PLAYER_FOOT_POS);
            pModel->SetSubModOffset(PLAYER_FOOT, offset);

            // Raycast to make sure there's room
            Location start, end, hit;

            MakeLocationFromVector(&start, &targ_vec);
            MakeLocationFromVector(&end, &targ_vec);

            ComputeCellForLocation(&start);
            end.vec.z += PLAYER_FOOT_POS - 1.0;

            hit_obj = 0;
            if ((PhysRaycast(start, end, &hit, &hit_obj, 0) != kCollideNone) && CanMantleOnto(hit_obj))
            {
               mx_copy_vec(&targ_vec, &hit.vec);
               // (crouched: just over the floor, the head under a ceiling
               // 4 ft up -- at 0.1 it was in it, and the mantle put back)
               targ_vec.z += -PLAYER_FOOT_POS + (g_MantleCrouch ? 0.02f : 0.1f);
            }

            pModel->SetTargetLocation(targ_vec);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d going to mantling state %d\n", pModel->GetObjID(), 5);
            #endif

            pModel->SetMantlingState(5);
         }
         else
         {
            mxs_vector vel;

            mx_copy_vec(&vel, &pModel->GetVelocity(PLAYER_HEAD));
            mx_scaleeq_vec(&vel, min(50.0f, max(30.0f, 30 / mx_mag2_vec(&delta_pos))));
            pModel->GetDynamics(PLAYER_HEAD)->SetVelocity(vel);
            pModel->UpdateEndLocation(PLAYER_HEAD, dt);

            #ifndef SHIP
            if (config_is_defined("MantleSpew"))
               mprintf("obj %d in mantling state %d\n", pModel->GetObjID(), 4);
            #endif
         }

         break;
      }

      // Floating, ready to be expanded
      case 5:
      {
#if 0
         if (pModel->InTerrainContact())
#endif
            BreakMantle(pModel->GetObjID());

         #ifndef SHIP
         if (config_is_defined("MantleSpew"))
            mprintf("obj %d in mantling state %d\n", pModel->GetObjID(), 5);
         #endif
      }
   }
}

////////////////////////////////////////

void GetClimbingDir(ObjID objID, mxs_vector *dir)
{
   if (GetObjTextureIdx(objID) >= 0)
   {
      // texture

      // @TODO: look at the poly's v axis
      mx_unit_vec(dir, 2);

   }
   else
   {
      // object

      mxs_matrix orien;
      mx_ang2mat(&orien, &ObjPosGet(objID)->fac);

      mx_copy_vec(dir, &orien.vec[2]);
   }
}




