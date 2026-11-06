/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

//////////////////////////////////////////////////////////////////////////////
// $Header: r:/t2repos/thief2/src/ai/aisgtcst.cpp,v 1.10 2000/03/25 22:14:55 adurant Exp $
//
// line-of-sight check from pStartLoc to pEndLoc, accounting for those
// doors which are set to block visibility
//


#include <dynarray.h>

#include <wrtype.h>
#include <objcast.h>

#include <propface.h>
#include <propbase.h>
#include <property.h>
#include <propfac_.h>
#include <rendprop.h>
#include <prophash.h>

#include <aitype.h>
#include <aiutils.h>
#include <aiprops.h>
#include <memall.h>
#include <stdio.h>
#include <physcast.h>
#include <objpos.h>
#include <osysbase.h>
#include <objedit.h>
#include <dbmem.h>   // must be last header! 

static cDynArray<ObjID> _g_InterestingObjects;


//////////////////////////////////////////////////////////////////////////////

static IBoolProperty * g_pAIBlocksVisionProperty;

///////////////////////////////////////

static sPropertyDesc g_AIBlocksVisionPropertyDesc = 
{
   "AI_BlkVis",
   0,
   NULL, 0, 0,  // constraints, version
   { AI_UTILITY_CAT, "Blocks AI Vision" },
   kPropertyChangeLocally,  // net_flags
};

///////////////////////////////////////

typedef cSpecificProperty<IBoolProperty,&IID_IBoolProperty,BOOL,cHashPropertyStore<cSimpleDataOps> > cBaseAIBVProp; 


class cAIBlocksVisionProperty : public cBaseAIBVProp
{
public:
   cAIBlocksVisionProperty()
      : cBaseAIBVProp(&g_AIBlocksVisionPropertyDesc)
   {
      SetRebuildConcretes(TRUE);
   }

   virtual void RebuildConcrete(ObjID obj, BOOL fIsRelevant, uPropListenerValue, ObjID donor);

   STANDARD_DESCRIBE_TYPE(BOOL); 
};

///////////////////////////////////////

void cAIBlocksVisionProperty::RebuildConcrete(ObjID obj, BOOL fIsRelevant, uPropListenerValue val, ObjID donor)
{
   BOOL fHad;
   int iPrev;
   
   if (!ObjHasRefs(obj))
      return;
   
   for (iPrev = _g_InterestingObjects.Size() - 1; iPrev >= 0 ; iPrev--)
   {
      if (_g_InterestingObjects[iPrev] == obj)
         break;
   }
   
   fHad = (iPrev != -1);

   if (fIsRelevant && val.intval)
   {
      if (!fHad)
         _g_InterestingObjects.Append(obj);
   }
   else if (fHad)
   {
      _g_InterestingObjects.FastDeleteItem(iPrev);
   }
   
   cBaseAIBVProp::RebuildConcrete(obj, fIsRelevant, val, donor);
}

//////////////////////////////////////////////////////////////////////////////

void AIInitSightCast()
{
   g_pAIBlocksVisionProperty = new cAIBlocksVisionProperty;
}

///////////////////////////////////////

void AITermSightCast()
{
   SafeRelease(g_pAIBlocksVisionProperty);
}

///////////////////////////////////////

void AIDBResetSightCast()
{
   _g_InterestingObjects.SetSize(0);
}

//////////////////////////////////////////////////////////////////////////////

// (the physics cast below takes only the doors and things that block sight)
static BOOL AISightCastIsBlocker(ObjID obj, const cPhysModel *, void *)
{
   for (int i = 0; i < _g_InterestingObjects.Size(); i++)
      if (_g_InterestingObjects[i] == obj)
         return TRUE;
   return FALSE;
}

BOOL AISightCast(Location *pStartLoc, Location *pEndLoc)
{
   Location HitLoc;     // dummy
   if (ObjRaycast(pStartLoc, pEndLoc, &HitLoc, TRUE, _g_InterestingObjects.AsPointer(),
                  _g_InterestingObjects.Size()) != kObjCastNone)
      return FALSE;
   if (_g_InterestingObjects.Size() == 0)
      return TRUE;

   // A door is also its solid box (its physics, as it stops the player): the
   // model test above goes by the door's own triangles, and a line through a
   // gap or an edge of a model -- a high-detail replacement's, or one that
   // does not quite fill its frame -- let a guard see through a shut door.
   Location start = *pStartLoc, end = *pEndLoc, hit;
   ObjID hitObj = OBJ_NULL;
   PhysRaycastSetCallback(AISightCastIsBlocker, NULL);
   const int cast = PhysRaycast(start, end, &hit, &hitObj, 0, kCollideOBB);
   PhysRaycastClearCallback();
   if (cast == kCollideNone || hitObj == OBJ_NULL || !AISightCastIsBlocker(hitObj, NULL, NULL))
      return TRUE;

   static int s_told = 0;
   if (s_told < 20)
   {
      ++s_told;
      fprintf(stderr, "AI sight: %s blocks a line its model let through, at (%.1f %.1f %.1f)\n",
              ObjWarnName(hitObj), hit.vec.x, hit.vec.y, hit.vec.z);
   }
   return FALSE;
}


//////////////////////////////////////////////////////////////////////////////

// preperatory func, on entering sim: gather ObjIDs of all objects
// which AIs should not be able to see through

// @TODO: Is there anything AIs should care about other than doors?
void AISightCastCollectRefs()
{
   const cDynArray<sAIDoorInfo> & doors = AIGetDoors();
   _g_InterestingObjects.SetSize(0);
   for (int i = 0; i < doors.Size(); i++)
   {
      if (doors[i].flags & kAIDF_BlocksVision)
         _g_InterestingObjects.Append(doors[i].obj);
   }
   
   
   int              iLimit = _g_InterestingObjects.Size();
   ObjID            obj;
   sPropertyObjIter iter;
   
   g_pAIBlocksVisionProperty->IterStart(&iter);

   while (g_pAIBlocksVisionProperty->IterNext(&iter, &obj))
   {
      if (!OBJ_IS_CONCRETE(obj) || !ObjHasRefs(obj))
         continue;

      int i;
      for (i = 0; i < iLimit; i++)
         if (_g_InterestingObjects[i] == obj)
            break;

      if (i == iLimit)
         _g_InterestingObjects.Append(obj);
   }
   
   g_pAIBlocksVisionProperty->IterStop(&iter);
   
}

//////////////////////////////////////////////////////////////////////////////
