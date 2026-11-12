/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

// $Header: r:/t2repos/thief2/src/dark/drkloot.cpp,v 1.15 2000/01/04 18:25:47 BFarquha Exp $

#include <appagg.h>
#include <drkloot.h>
#include <lootprop.h>
#include <contain.h>
#include <string.h>
#include <rendprop.h>
#include <mnamprop.h>
#include <invrndpr.h>
#include <propman.h>
#include <propbase.h>
#include <str.h>
#include <iobjsys.h>
#include <drkinvui.h>
#include <drkinvpr.h>
#include <drkuires.h>
#include <report.h>
#include <drkrphlp.h>
#include <questapi.h>
#include <drkldout.h>
#include <iobjsys.h>
#include <objquery.h>

#include <simman.h>
#include <dspchdef.h>
#include <simdef.h>
#include <simtime.h>
#include <playrobj.h>

#include <objedit.h>
#include <sdestool.h>
#include <objpos.h>
#include <objdef.h>

#include <traitman.h>
#include <stdio.h>
#include <stdlib.h>

// include these last
#include <initguid.h>
#include <drkloot_.h>

#include <dbmem.h>

ILootProperty* gpLootProp = NULL;
static IInvRenderProperty* gpInvProp = NULL;
static IObjectSystem* gpObjSys = NULL;

static void set_loot_name(ObjID loot, const sLootProp* lootval)
{
   AutoAppIPtr_(PropertyManager,pPropMan);
   cAutoIPtr<IProperty> prop ( pPropMan->GetPropertyNamed(PROP_GAME_NAME_NAME));
   cAutoIPtr<IStringProperty> nameprop(IID_IStringProperty,prop);

   cStr name = "@LOOT: \"";
   int total = 0;
   char buf[32];

   // copy in loot values
   for (int i = 0; i < kLootSpecial; i++)
   {
      char strname[8];
      sprintf(strname,"Loot_%d",i);
      cStr lootname = FetchUIString("misc",strname);

      sprintf(buf,"%s: %d\n",(const char*)lootname,lootval->count[i]);
      name += buf;
      total += lootval->count[i];
   }

   cStr totname = FetchUIString("misc","loot_total");
   sprintf(buf,"%s: %d\"",(const char*)totname,total);
   name += buf;
   nameprop->Set(loot,name);
}

static BOOL combine_loot(ObjID targ, ObjID src)
{
   sLootProp* srcprop;
   if (gpLootProp->Get(src,&srcprop))
   {
      sLootProp newprop;
      sLootProp* targprop;

      if (gpLootProp->Get(targ,&targprop))
         newprop = *targprop;
      else
         memset(&newprop,0,sizeof(newprop));

      // copy in loot values
      for (int i = 0; i < kLootSpecial; i++)
         newprop.count[i] += srcprop->count[i];
      newprop.count[kLootSpecial] |= srcprop->count[kLootSpecial];

      gpLootProp->Set(targ,&newprop);

      set_loot_name(targ,&newprop);

      // Copy render info too
      ObjSetRenderType(targ,ObjRenderType(src));

      char buf[256]; // this is dumb, but the api is ancient
      if (ObjGetModelName(src,buf))
         ObjSetModelName(targ,buf);

      sInvRenderType* rtype;

      if (gpInvProp->Get(src,&rtype))
         gpInvProp->Set(targ,rtype);
      {
         sInvRenderType blank = { kInvRenderDefault };
         gpInvProp->Set(targ,&blank);
      }

      return TRUE;

   }

   return FALSE;
}

// The mission's loot objectives (goal_type_N 3), met by what the player
// carries: as the game's VictoryChecker (convict.osm) has them -- the loot
// item's total, gold, gems and goods at least the goal's, its special bits
// all there. That script did not complete one (Black Parade: 807 of 800
// stolen, the objective still open), so the engine checks them too, on
// every piece of loot the player takes and as the game resumes. (Only
// ever completing one -- never undoing what the script has set.)
// What the player carried as the mission began (a purse the mission gives
// to spend: Reflections in Red's 2000 to 3500 gold), kept in the mission's
// quest data so a saved game keeps it: the loot objectives count what was
// taken since, not that -- its loot objective, 1500 to 3250, came up done
// as the mission began. Begun as the simulation starts (with what the
// player holds then), and added to by what reaches the player before its
// clock first moves (the purse, given at sim time 0).
static const char *const kLootBaseVars[5] = {"drkloot_base_total", "drkloot_base_gold", "drkloot_base_gems",
                                             "drkloot_base_goods", "drkloot_base_special"};

static void add_loot_base(const sLootProp *loot)
{
   AutoAppIPtr(QuestData);
   int total = 0;
   for (int i = 0; i < kLootSpecial; i++)
      total += loot->count[i];
   const int add[5] = {total, loot->count[kLootGold], loot->count[kLootGems], loot->count[kLootArt], 0};
   for (int k = 0; k < 5; k++)
   {
      if (!pQuestData->Exists(kLootBaseVars[k]))
         pQuestData->Create(kLootBaseVars[k], 0, kQuestDataMission);
      if (k < 4)
         pQuestData->Set(kLootBaseVars[k], pQuestData->Get(kLootBaseVars[k]) + add[k]);
      else
         pQuestData->Set(kLootBaseVars[k], pQuestData->Get(kLootBaseVars[k]) | loot->count[kLootSpecial]);
   }
   fprintf(stderr, "Loot: %d carried as the mission begins (not counted for its loot objectives)\n", total);
}

static void check_loot_goals(const sLootProp *loot)
{
   if (!loot)
      return;
   AutoAppIPtr(QuestData);
   if (!pQuestData->Exists(kLootBaseVars[0]))
      return;   // (before the mission's start is known: nothing taken yet)
   int total = 0;
   for (int i = 0; i < kLootSpecial; i++)
      total += loot->count[i];
   sLootProp taken = *loot;
   total -= pQuestData->Get(kLootBaseVars[0]);
   if (total < 0)
      total = 0;
   {
      const int idx[3] = {kLootGold, kLootGems, kLootArt};
      for (int k = 0; k < 3; k++)
      {
         taken.count[idx[k]] -= pQuestData->Get(kLootBaseVars[k + 1]);
         if (taken.count[idx[k]] < 0)
            taken.count[idx[k]] = 0;
      }
      taken.count[kLootSpecial] &= ~pQuestData->Get(kLootBaseVars[4]);
   }
   loot = &taken;
   for (int n = 0; n < 32; n++)
   {
      char name[64];
      sprintf(name, "goal_type_%d", n);
      if (!pQuestData->Exists(name) || pQuestData->Get(name) != 3)
         continue;
      sprintf(name, "goal_state_%d", n);
      if (!pQuestData->Exists(name) || pQuestData->Get(name) != 0)
         continue;
      sprintf(name, "goal_visible_%d", n);
      if (pQuestData->Exists(name) && !pQuestData->Get(name))
         continue;
      // (one for another difficulty)
      if (pQuestData->Exists("difficulty"))
      {
         const int diff = pQuestData->Get("difficulty");
         sprintf(name, "goal_min_diff_%d", n);
         if (pQuestData->Exists(name) && diff < pQuestData->Get(name))
            continue;
         sprintf(name, "goal_max_diff_%d", n);
         if (pQuestData->Exists(name) && diff > pQuestData->Get(name))
            continue;
      }
      static const char *const kinds[4] = {"loot", "gold", "gems", "goods"};
      const int have[4] = {total, loot->count[kLootGold], loot->count[kLootGems], loot->count[kLootArt]};
      BOOL met = TRUE;
      // (an objective that asks for nothing yet -- its amounts not set, or
      // set by the mission's own scripts later -- is not met by nothing:
      // Reflections in Red's loot objective came up done as the mission began)
      BOOL asks = FALSE;
      for (int k = 0; k < 4 && met; k++)
      {
         sprintf(name, "goal_%s_%d", kinds[k], n);
         if (pQuestData->Exists(name) && pQuestData->Get(name) > 0)
         {
            asks = TRUE;
            if (pQuestData->Get(name) > have[k])
               met = FALSE;
         }
      }
      int specials = 0;
      sprintf(name, "goal_specials_%d", n);
      if (pQuestData->Exists(name))
         specials = pQuestData->Get(name);
      sprintf(name, "goal_special_%d", n);
      if (pQuestData->Exists(name) && pQuestData->Get(name) > 0)
         specials |= 1 << (pQuestData->Get(name) - 1);
      if (specials)
         asks = TRUE;
      if ((loot->count[kLootSpecial] & specials) != specials)
         met = FALSE;
      if (!met || !asks)
         continue;
      fprintf(stderr, "Loot: objective %d met (loot taken %d: gold %d, gems %d, goods %d) -- completed\n", n, total,
              loot->count[kLootGold], loot->count[kLootGems], loot->count[kLootArt]);
      sprintf(name, "goal_state_%d", n);
      pQuestData->Set(name, 1);
   }
}

// the player's own loot (the one item it all combines into: the most of it)
static void check_player_loot_goals(void)
{
   AutoAppIPtr(QuestData);
   if (!PlayerObjectExists())
      return;
   sLootProp best;
   memset(&best, 0, sizeof(best));
   int bestTotal = -1;
   AutoAppIPtr(ContainSys);
   sContainIter *iter = pContainSys->IterStart(PlayerObject());
   for (; !iter->finished; pContainSys->IterNext(iter))
   {
      sLootProp *prop;
      if (gpLootProp->Get(iter->containee, &prop))
      {
         int t = 0;
         for (int i = 0; i < kLootSpecial; i++)
            t += prop->count[i];
         if (t > bestTotal)
         {
            bestTotal = t;
            best = *prop;
         }
      }
   }
   pContainSys->IterEnd(iter);
   if (!pQuestData->Exists(kLootBaseVars[0]))
   {
      // the mission's start: what the player carries now is not taken
      const int base[5] = {bestTotal > 0 ? bestTotal : 0, best.count[kLootGold], best.count[kLootGems],
                           best.count[kLootArt], best.count[kLootSpecial]};
      for (int k = 0; k < 5; k++)
         pQuestData->Create(kLootBaseVars[k], base[k], kQuestDataMission);
      fprintf(stderr, "Loot: the mission begins with %d loot carried (not counted for its objectives)\n", base[0]);
      return;
   }
   if (bestTotal >= 0)
      check_loot_goals(&best);
}

static BOOL loot_combine_CB(eContainsEvent event, ObjID targ, ObjID src, eContainType ctype, ContainCBData /*data*/)
{
   if (event == kContainCombine)
   {
      combine_loot(targ,src);
      sLootProp* prop;
      AutoAppIPtr(ContainSys);
      if (PlayerObjectExists() && pContainSys->Contains(PlayerObject(), targ) && gpLootProp->Get(targ, &prop) &&
          GetSimTime() > 0)
         check_loot_goals(prop);
   }
   else if (event == kContainAdd)
   {
      sLootProp* prop;
      if (gpLootProp->Get(src,&prop))
      {
         set_loot_name(src,prop);
         if (IsPlayerObj(targ))
         {
            InvUIRefreshObj(src);
            if (GetSimTime() == 0)
               add_loot_base(prop);   // (given as the mission begins)
            else
               check_loot_goals(prop);
         }
      }
   }

   return TRUE;
}

static void tabulate_player_loot(BOOL destroy)
{
   AutoAppIPtr(QuestData);

   gpObjSys->Lock();
   int total = pQuestData->Get(MONEY_QVAR);
   sLootProp* prop;
   AutoAppIPtr(ContainSys);
   sContainIter* iter = pContainSys->IterStart(PlayerObject());
   for (; !iter->finished; pContainSys->IterNext(iter))
      if (gpLootProp->Get(iter->containee,&prop))
      {
         // copy in loot values
         for (int i = 0; i < kLootSpecial; i++)
            total += prop->count[i];

         if (destroy)
            gpObjSys->Destroy(iter->containee);
      }
   pContainSys->IterEnd(iter);

   gpObjSys->Unlock();
   pQuestData->Set(MONEY_QVAR,total);

}

void DarkLootPrepMission(void)
{
   tabulate_player_loot(TRUE);
}

static void sim_cb(const sDispatchMsg* msg, const sDispatchListenerDesc* desc)
{
   switch(msg->kind)
   {
      case kSimInit:
         {
            AutoAppIPtr(QuestData);
            pQuestData->Set(MONEY_QVAR,0);
            // the loot objectives' count begins (what the player holds now
            // was not taken in this mission)
            for (int k = 0; k < 5; k++)
               if (!pQuestData->Exists(kLootBaseVars[k]))
                  pQuestData->Create(kLootBaseVars[k], 0, kQuestDataMission);
            if (PlayerObjectExists())
            {
               AutoAppIPtr(ContainSys);
               sContainIter *iter = pContainSys->IterStart(PlayerObject());
               for (; !iter->finished; pContainSys->IterNext(iter))
               {
                  sLootProp *prop;
                  if (gpLootProp->Get(iter->containee, &prop))
                     add_loot_base(prop);
               }
               pContainSys->IterEnd(iter);
            }
         }
         break;

      case kSimTerm:
         tabulate_player_loot(FALSE);
         break;

      case kSimResume:
         check_player_loot_goals();
         break;
   }
}

static void setup_sim_cb()
{
   sDispatchListenerDesc desc =
   {
      &SIM_Loot,
      kSimInit|kSimTerm|kSimResume,
      sim_cb,
      NULL
   };

   AutoAppIPtr(SimManager);
   pSimManager->Listen(&desc);
}

// count all loot (not held by except)
int count_all_loot(ObjID except)
{
   sLootProp *val;
   int total=0;

   AutoAppIPtr(ObjectSystem);
   AutoAppIPtr(ContainSys);
   IObjectQuery *allObjQuery=pObjectSystem->Iter(kObjectConcrete);
   for ( ; !allObjQuery->Done(); allObjQuery->Next())
      if (gpLootProp->Get(allObjQuery->Object(),&val))
         if (except==OBJ_NULL || !pContainSys->Contains(except,allObjQuery->Object()))
            for (int i = 0; i < kLootSpecial; i++)
               total += val->count[i];
   SafeRelease(allObjQuery);
   return total;
}

//////////////////
// Report Functions
#ifdef REPORT

static void _loot_report(ObjID obj, int WarnLevel, void *data, char *buffer, eReportPhase phase)
{
   BOOL loud=(WarnLevel==kReportLevel_DumpAll);
   static sLootProp total;
   static int t_obj_count;
   sLootProp *pProp;
   char *p=buffer;
   int i;
   AutoAppIPtr_(StructDescTools,pTools);
   const sStructDesc* sdesc = pTools->Lookup("sLootProp");

   if (WarnLevel<kReportLevel_Info) return;  // nothing to say, no warnings or errors for loot yet

   switch (phase)
   {
      case kReportPhase_Init:
         for (i = 0; i < kNumLoot; i++)
            total.count[i] = 0;
         t_obj_count=0;
         if (loud)
         {
            rsprintf(&p,"\nLoot Report\n"); // no need to deal with p, since we are done
            DarkReportHelpersStartUse();
         }
         break;
      case kReportPhase_Loop:
         if (!gpLootProp->Get(obj,&pProp))
            break;
         for (i = 0; i < kLootSpecial; i++)
            total.count[i] += pProp->count[i];
         total.count[kLootSpecial] |= pProp->count[kLootSpecial];
         t_obj_count++;

         if (loud)
         {
            rsprintf(&p,"%s%s%s vals ",ObjWarnName(obj),RHLocS(obj),DRHDiffS(obj));
            if (sdesc)
            {
               pTools->UnparseFull(sdesc,pProp,p,128);
               p+=strlen(p);
            }
            rsprintf(&p,"\n");
         }
         break;
      case kReportPhase_Term:
         if (sdesc)
         {
            rsprintf(&p,"%sTotal loot: ",loud?"":"\n");
            pTools->UnparseFull(sdesc,&total,p,128);
            p+=strlen(p);
            rsprintf(&p," from %d objects\n",t_obj_count);
         }
         if (loud)
            DarkReportHelpersDoneWith();
         break;
   }
}
#endif

//////////////////
// Init/Term

void DarkLootInit(void)
{
   gpLootProp = InitLootProp();

   AutoAppIPtr_(PropertyManager,pPropMan);
   IProperty* invprop = pPropMan->GetPropertyNamed(PROP_INVREND_NAME);
   Verify(SUCCEEDED(invprop->QueryInterface(IID_IInvRenderProperty,(void**)&gpInvProp)));

   AutoAppIPtr(ContainSys);
   pContainSys->Listen(OBJ_NULL, loot_combine_CB, NULL);

   gpObjSys = AppGetObj(IObjectSystem);

   ReportRegisterObjCallback(_loot_report,"Loot System",NULL);
   setup_sim_cb();
}

void DarkLootTerm(void)
{
   ReportUnRegisterObjCallback(_loot_report,NULL);
   SafeRelease(gpLootProp);
   SafeRelease(gpInvProp);
   SafeRelease(gpObjSys);
}

