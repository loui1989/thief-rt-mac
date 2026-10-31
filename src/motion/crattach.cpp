/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

// $Header: r:/t2repos/thief2/src/motion/crattach.cpp,v 1.30 1999/12/09 20:15:23 BODISAFA Exp $

#include <lg.h>
#include <comtools.h>
#include <appagg.h>
#include <config.h>
#include <cfgdbg.h>

#include <traitman.h>
#include <iobjsys.h>
#include <objsys.h>
#include <osysbase.h>
#include <objpos.h>
#include <linkman.h>
#include <linkbase.h>
#include <lnkquery.h>
#include <relation.h>

#include <sdesc.h>
#include <sdesbase.h>

#include <crjoint.h>
#include <schema.h>
#include <rendprop.h>
#include <weaphit.h>
#include <weaphalo.h>
#include <weapon.h>
#include <plycbllm.h>

#include <crattach.h>
#include <creatapi.h>
#include <creatext.h>
#include <creature.h>
#include <creatur_.h>
#include <crwpnapi.h>

#include <mprintf.h>

#include <phflist.h>
#include <physapi.h>
#include <dmgmodel.h>
#include <dmgbase.h>
#include <playrobj.h>

// for physics listeners
#include <phcore.h>
#include <phlistn_.h>

// Must be last header
#include <dbmem.h>

static sRelationDesc sAttachRelDesc = 
{ 
   CREATURE_ATTACH_RELATION_NAME, 
   0,
};

static sRelationDataDesc sAttachRelDataDesc = LINK_DATA_DESC_FLAGS(sCreatureAttachInfo, kRelationDataAutoCreate);

static sFieldDesc sAttachRelFields[] =
{
   { "Joint",      kFieldTypeEnum, FieldLocation(sCreatureAttachInfo, joint),     kFieldFlagNone, 0, 22, 22, g_pJointNames },
   { "Is Weapon?", kFieldTypeBool, FieldLocation(sCreatureAttachInfo, is_weapon), kFieldFlagNotEdit },
};

static sStructDesc sAttachRelStructDesc = StructDescBuild(sCreatureAttachInfo, kStructFlagNone, sAttachRelFields);

IRelation *g_pCreatureAttachRelation = NULL;

////////////////////////////////////////
// NewDark's detail attachments: an object (the source) shown fixed to
// another (the destination) -- to one of a creature's joints, or to the
// object itself -- and following it as it moves: a pack horse's load, the
// lamp a guard carries. The link data, as NewDark's missions store it
// (32 bytes): the kind, the joint, the position and rotation relative to it.

struct sDetailAttachData
{
   uchar type;         // 0 object, 1 vhot, 2 joint, 3 subobject
   uchar joint;        // a creature joint (crjoint.h), for type 2
   ushort pad0;
   mxs_vector relPos;
   mxs_angvec relRot;
   ushort pad1;
   int vhotSub;
   int flags;
};
static_assert(sizeof(sDetailAttachData) == 32, "NewDark's DetailAttachement link data is 32 bytes");

static sRelationDesc sDetailAttachRelDesc =
{
   "DetailAttachement",
   0,
};

static sRelationDataDesc sDetailAttachRelDataDesc = LINK_DATA_DESC(sDetailAttachData);

static const char *g_pDetailAttachTypes[] = { "Object", "VHot", "Joint", "Subobject" };

static sFieldDesc sDetailAttachFields[] =
{
   { "Type",       kFieldTypeEnum,   FieldLocation(sDetailAttachData, type),    kFieldFlagNone, 0, 4, 4, g_pDetailAttachTypes },
   { "vhot/sub #", kFieldTypeInt,    FieldLocation(sDetailAttachData, vhotSub), kFieldFlagNone },
   { "joint",      kFieldTypeEnum,   FieldLocation(sDetailAttachData, joint),   kFieldFlagNone, 0, 22, 22, g_pJointNames },
   { "Flags",      kFieldTypeInt,    FieldLocation(sDetailAttachData, flags),   kFieldFlagNone },
   { "rel pos",    kFieldTypeVector, FieldLocation(sDetailAttachData, relPos),  kFieldFlagNone },
   { "rel rot",    kFieldTypeAngVec, FieldLocation(sDetailAttachData, relRot),  kFieldFlagNone },
};

static sStructDesc sDetailAttachStructDesc = StructDescBuild(sDetailAttachData, kStructFlagNone, sDetailAttachFields);

static IRelation *g_pDetailAttachRelation = NULL;

// A link without placement data (or with all of it zero) keeps the place its
// object had relative to the other when first seen: what the mission showed.
struct sDetailAttachKept
{
   LinkID link;
   mxs_vector relPos;   // in the parent's space
   mxs_matrix relMat;
};
static cDynArray<sDetailAttachKept> g_DetailAttachKept;

// Every attached object placed where its link puts it. Once a frame, before
// the scene is drawn.
EXTERN void DetailAttachmentsUpdate(void)
{
   if (g_pDetailAttachRelation == NULL)
      return;
   ILinkQuery *query = g_pDetailAttachRelation->Query(LINKOBJ_WILDCARD, LINKOBJ_WILDCARD);
   for (; !query->Done(); query->Next())
   {
      sLink link;
      query->Link(&link);
      const sDetailAttachData *data = (const sDetailAttachData *)g_pDetailAttachRelation->GetData(query->ID());
      ObjPos *parent = ObjPosGet(link.dest);
      if (parent == NULL || link.source == link.dest)
         continue;
      if (data != NULL)
      {
         static const sDetailAttachData zero = {};
         if (memcmp(data, &zero, sizeof(zero)) == 0)
            data = NULL;
      }
      {
         // each link once in the log: what it says
         static cDynArray<LinkID> logged;
         BOOL seen = FALSE;
         for (int i = 0; i < logged.Size() && !seen; ++i)
            seen = logged[i] == query->ID();
         if (!seen && logged.Size() < 64)
         {
            logged.Append(query->ID());
            if (data)
               mprintf("Detail attachment: %d on %d, type %d joint %d, at (%.2f %.2f %.2f), turned (%d %d %d)%s\n",
                       link.source, link.dest, data->type, data->joint, data->relPos.x, data->relPos.y,
                       data->relPos.z, data->relRot.tx, data->relRot.ty, data->relRot.tz,
                       CreatureGetMeshInterface(link.dest) ? ", a creature" : "");
            else
               mprintf("Detail attachment: %d on %d, no placement: kept where it was%s\n",
                       link.source, link.dest, CreatureGetMeshInterface(link.dest) ? ", a creature" : "");
         }
      }

      mxs_vector loc;
      mxs_matrix orient;
      if (data != NULL && data->type == 2 && data->joint > kCJ_Invalid &&
          data->joint < kCJ_NumCreatureJoints && CreatureGetMeshInterface(link.dest) != NULL)
      {
         loc = GetCreatureJointPos(link.dest, data->joint);
         orient = GetCreatureJointOrient(link.dest, data->joint);
      }
      else
      {
         loc = parent->loc.vec;
         mx_ang2mat(&orient, &parent->fac);
      }

      mxs_matrix rel, world;
      mxs_vector offset, where;
      mxs_angvec fac;
      if (data != NULL)
      {
         mx_ang2mat(&rel, &data->relRot);
         mx_mat_mul_vec(&offset, &orient, &data->relPos);
      }
      else
      {
         // its place relative to the parent when first seen
         sDetailAttachKept *kept = NULL;
         for (int i = 0; i < g_DetailAttachKept.Size() && !kept; ++i)
            if (g_DetailAttachKept[i].link == query->ID())
               kept = &g_DetailAttachKept[i];
         if (kept == NULL)
         {
            ObjPos *own = ObjPosGet(link.source);
            if (own == NULL)
               continue;
            sDetailAttachKept k;
            k.link = query->ID();
            mxs_vector d;
            mx_sub_vec(&d, &own->loc.vec, &loc);
            mx_mat_tmul_vec(&k.relPos, &orient, &d);     // into the parent's space
            mxs_matrix ownMat;
            mx_ang2mat(&ownMat, &own->fac);
            mx_tmul_mat(&k.relMat, &orient, &ownMat);    // orient^T * own
            g_DetailAttachKept.Append(k);
            kept = &g_DetailAttachKept[g_DetailAttachKept.Size() - 1];
         }
         rel = kept->relMat;
         mx_mat_mul_vec(&offset, &orient, &kept->relPos);
      }
      mx_add_vec(&where, &loc, &offset);
      mx_mul_mat(&world, &orient, &rel);
      mx_mat2ang(&fac, &world);

      // moved only when it has to be: each move relinks it to the cells
      ObjPos *own = ObjPosGet(link.source);
      if (own != NULL && mx_dist2_vec(&own->loc.vec, &where) < 0.0001f &&
          own->fac.tx == fac.tx && own->fac.ty == fac.ty && own->fac.tz == fac.tz)
         continue;
      PosPropLock++;
      ObjPosUpdate(link.source, &where, &fac);
      PosPropLock--;
   }
   SafeRelease(query);
}

void LGAPI CreatureAttachRelationListener(sRelationListenMsg *msg, RelationListenerData data)
{
   if (msg->type & kListenLinkBirth)
   {
      sCreatureAttachInfo *pAttachInfo = (sCreatureAttachInfo *)g_pCreatureAttachRelation->GetData(msg->id);

      if (!pAttachInfo->is_weapon)
      {
         pAttachInfo->weapon = kCrWeap_Invalid;

         BOOL all_zero = TRUE;
         for (int i=0; i<9 && all_zero; i++)
         {
            if (pAttachInfo->relTrans.mat.el[i] != 0.0)
               all_zero = FALSE;
         }

         if (all_zero)
            mx_identity_mat(&pAttachInfo->relTrans.mat);

         g_pCreatureAttachRelation->SetData(msg->id, (void *)pAttachInfo);
      }
   }
}

void InitCreatureAttachments()
{
   Assert_(g_pCreatureAttachRelation == NULL);

   // install item attachment relation
   g_pCreatureAttachRelation = CreateStandardRelation(&sAttachRelDesc, &sAttachRelDataDesc, kQCaseSetSourceKnown);
   g_pCreatureAttachRelation->Listen(kListenLinkBirth, CreatureAttachRelationListener, NULL);

   StructDescRegister(&sAttachRelStructDesc);

   // (before any mission's relations are read, so its link data is kept)
   g_pDetailAttachRelation = CreateStandardRelation(&sDetailAttachRelDesc, &sDetailAttachRelDataDesc, kQCaseSetAll);
   StructDescRegister(&sDetailAttachStructDesc);
}

void TermCreatureAttachments()
{
   Assert_(g_pCreatureAttachRelation != NULL);

   SafeRelease(g_pCreatureAttachRelation);
   SafeRelease(g_pDetailAttachRelation);
}

// weapon gets attached.  Is left non-physical by default
BOOL CreatureAttachWeapon(ObjID creature, ObjID weaponObj, int weaponType)
{
   LinkID linkID;
   sCreatureAttachInfo attachInfo;

   if(!OBJ_IS_CONCRETE(weaponObj))
   {
      Warning(("Unable to attach weapon %d to creature %d\n",creature,weaponObj));
      return FALSE;
   }
   cCreature *pCreature=CreatureFromObj(creature);

   if(pCreature)
   {
      pCreature->GetWeaponAttachDefaults(weaponType,&attachInfo);
   }

   // Set link
   linkID = g_pCreatureAttachRelation->AddFull(creature, weaponObj, (void *)&attachInfo);

   // Reffed, but invisible
   if (!config_is_defined("show_weapon"))
      ObjSetRenderType(weaponObj, kRenderNotAtAll);
   ObjSetHasRefs(weaponObj, TRUE);

   return linkID != LINKID_NULL;
}

void CreatureDetachWeapon(ObjID creature, ObjID weaponObj)
{
   // Visible, but unreffed
   ObjSetHasRefs(weaponObj, FALSE);
   if (!config_is_defined("show_weapon"))
   {
      ObjSetRenderType(weaponObj, kRenderNormally);
   }
   
   CreatureMakeWeaponNonPhysical(creature,weaponObj);
   CreatureDetachItem(creature,weaponObj);
}

BOOL CreatureMakeWeaponPhysical(ObjID creature, ObjID weapon, int weaponType)
{
   if(weapon==OBJ_NULL)
      return FALSE;
   cCreature *pCreature=CreatureFromObj(creature);

   if(!pCreature)
   {
      Warning(("No creature with objID %d\n",creature));
      return FALSE;
   }
   return pCreature->MakeWeaponPhysical(weapon,weaponType);
}

void CreatureMakeWeaponNonPhysical(ObjID creature, ObjID weapon)
{
   if(weapon==OBJ_NULL)
      return;

   cCreature *pCreature=CreatureFromObj(creature);

   if(!pCreature)
   {
      Warning(("No creature with objID %d\n",creature));
      return;
   }
   pCreature->MakeWeaponNonPhysical(weapon);
}

BOOL CreatureAttachItem(ObjID creature, ObjID item, const sCreatureAttachInfo *pAttachInfo)
{
   LinkID linkID;

   if(!CreatureExists(creature) || !OBJ_IS_CONCRETE(creature) || !OBJ_IS_CONCRETE(item))
   {
      Warning(("Unable to attach item %d to creature %d\n",creature,item));
      return FALSE;
   }

   // Set link
   if(pAttachInfo)
      linkID = g_pCreatureAttachRelation->AddFull(creature, item, (void *)pAttachInfo);
   else   
      linkID = g_pCreatureAttachRelation->Add(creature, item);

   return linkID != LINKID_NULL;
}

// iterate over creature attachment links between the two items, 
// deleting links as you go.
void CreatureDetachItem(ObjID creature, ObjID item)
{
   LinkID linkID;

   ILinkQuery *Query = g_pCreatureAttachRelation->Query(creature, item);

   for(; !Query->Done(); Query->Next())
   {
      linkID=Query->ID();
      g_pCreatureAttachRelation->Remove(linkID);
   }

   SafeRelease(Query);
}

void CreatureDetachAllItems(ObjID creature)
{
   LinkID linkID;

   ILinkQuery *Query= g_pCreatureAttachRelation->Query(creature, LINKOBJ_WILDCARD);

   for(; !Query->Done(); Query->Next())
   {
      linkID=Query->ID();
      g_pCreatureAttachRelation->Remove(linkID);
   }
   SafeRelease(Query);
}

void CreatureAttachmentsPhysUpdate(ObjID creature)
{
   cCreature *pCreature;

   if(creature==OBJ_NULL || (NULL==(pCreature=CreatureFromObj(creature))) )
      return;

   ILinkQuery *Query = g_pCreatureAttachRelation->Query(creature, LINKOBJ_WILDCARD);

   for(; !Query->Done(); Query->Next())
   {
      sLink link;
      sCreatureAttachInfo *pInfo;
      sCrPhysModOffsetTable *pTable;
      eForceListResult forceListResult;
      int i;

      Query->Link(&link);
      if(!PhysObjHasPhysics(link.dest))
         continue;

      if(NULL!=(pInfo=(sCreatureAttachInfo *)g_pCreatureAttachRelation->GetData(Query->ID())))
      {
         if(pInfo->weapon==kCrWeap_Invalid)
         {
            Warning(("CreatureAttachmentsUpdate: do not support item with physics\n"));
         } else
         {
            // get positions of all the phys submodels
            if(NULL!=(pTable=pCreature->GetWeaponPhysOffsets(pInfo->weapon)))
            {
               sCrPhysModOffset *pMod=pTable->pPhysModels;
               mxs_vector  subPos;
               mxs_vector  endPos;
               mxs_vector  force;
               mxs_real    time;

               PhysicsListenerLock = TRUE;
               for(i=0;i<pTable->nPhysModels;i++,pMod++)
               {
                  pCreature->GetPhysSubModPos(pMod,&subPos);
                  forceListResult = PhysApplyForceList(link.dest, i, NULL, 0, 1, &subPos, &endPos, 
                                                       &force, &time, FALSE, TRUE);
                  // This is probably the wrong place to do this, i.e., we should really 
                  // do it in physics, I think.
                  if (!(forceListResult&kFLR_Success))
                  {
                     if (forceListResult & kFLR_TerrainCollision)
                     {
                        // for the moment, don't report terrain collisions
                     }
                     else
                     {
                        if (g_pPhysListeners->HasListener(link.dest, kCollisionMsg))
                        {
                           mxs_vector null = {0, 0, 0};
                           // this sucks, because we don't have most of the information here.
                           g_pPhysListeners->CallCollision(link.dest, 0, kPC_Object, OBJ_NULL, 0, null, 0, null);
                        }
                     }
                  }
                  if (forceListResult & kFLR_TerrainCollision)
                  {
                     HandleWeaponHit(OBJ_NULL, link.dest);
                     WeaponPostHit(OBJ_NULL, link.dest, OBJ_NULL); 
                     break;
                  }
                  if (forceListResult & kFLR_MadeNonPhysical)
                     break;
               }
               WeaponHaloUpdate(creature, link.dest, pTable->nPhysModels);
               PhysicsListenerLock = FALSE;
            }
         }
      } else
      {
         if(PhysObjHasPhysics(link.dest))
         {
            Warning(("CreatureAttachmentsUpdate: do not support item with physics\n"));
         }
      }
   }
   SafeRelease(Query);
}

// update object positions of all attached objects
EXTERN void CreatureAttachmentsPosUpdate(ObjID creature)
{
   IMesh *pMesh;

   if(NULL==(pMesh=CreatureGetMeshInterface(creature)))
      return;

   ILinkQuery *Query = g_pCreatureAttachRelation->Query(creature, LINKOBJ_WILDCARD);

   for(; !Query->Done(); Query->Next())
   {
      sLink link;

      PosPropLock++;

      Query->Link(&link);
      sCreatureAttachInfo *pInfo = (sCreatureAttachInfo *)g_pCreatureAttachRelation->GetData(Query->ID());
      if (pInfo != NULL)
      {
         if (pInfo->joint == kCJ_Invalid)
         {
            pInfo->joint = kCJ_Butt;
         }

         // Verify happy joint.
         AssertMsg1((pInfo->joint >= 0 && pInfo->joint < kCJ_NumCreatureJoints),
                  "CreatureAttachmentsPosUpdate: Bad joint %d\n", pInfo->joint);

         // update obj position of dst obj
         const mxs_vector *loc = &GetCreatureJointPos(creature, pInfo->joint);
         const mxs_matrix *orient = &GetCreatureJointOrient(creature, pInfo->joint);

         mxs_matrix temp_mat;
         mxs_vector temp_loc;
         mxs_angvec fac;

         mx_mul_mat(&temp_mat, orient, &pInfo->relTrans.mat);

         mx_mat_mul_vec(&temp_loc, orient, &pInfo->relTrans.vec);
         mx_addeq_vec(&temp_loc, loc);

         mx_mat2ang(&fac, &temp_mat);

         ObjPosUpdate(link.dest, &temp_loc, &fac);
      } else
      {
         ObjPos *pPos=ObjPosGet(link.source);
         ObjPosUpdate(link.dest,&pPos->loc.vec,&pPos->fac);
      }

      PosPropLock--;
   }
   SafeRelease(Query);
}

EXTERN void CreatureAttachmentModify(ObjID creature, ObjID item, sCreatureAttachInfo *pAttachInfo)
{
   ILinkQuery *Query = g_pCreatureAttachRelation->Query(creature, item);

   for(; !Query->Done(); Query->Next())
   {
      g_pCreatureAttachRelation->SetData(Query->ID(),(void *)pAttachInfo);
   }

   SafeRelease(Query);
}

// CreatureAttachmentGet:  Retrieve the nth item off of a creature.
EXTERN sCreatureAttachInfo* CreatureAttachmentGet(ObjID creature, ObjID item, const int& inItemIndex)
{
   ILinkQuery *Query = g_pCreatureAttachRelation->Query(creature, item);

   for(int i = 0; !Query->Done(); Query->Next())
   {
      if (i == inItemIndex)
      {
         return (sCreatureAttachInfo*) g_pCreatureAttachRelation->GetData(Query->ID());
      }
   }
   SafeRelease(Query);
   return NULL;
}

