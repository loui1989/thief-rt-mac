/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

// $Header: r:/t2repos/thief2/src/sim/plycbllm.cpp,v 1.55 1999/05/15 10:32:31 JON Exp $

#include <plyrbase.h>
#include <plyablty.h>
#include <plycbllm.h>
#include <mclntapi.h>
#include <motschem.h>
#include <mschbase.h>
#include <playrobj.h>
#include <objtype.h>
#include <objsys.h>
#include <objpos.h>
#include <osysbase.h>
#include <propbase.h>
#include <mnamprop.h>
#include <iobjsys.h>
#include <cretprop.h>
#include <creatapi.h>
#include <ctagset.h>
#include <camera.h>
#include <command.h>
#include <config.h>
#include <stdio.h>
#include <matrixs.h>
#include <fix.h>
#include <rendprop.h>
#include <mnumprop.h>
#include <dmgmodel.h>
#include <objsys.h>
#include <creatext.h>
#include <physapi.h>
#include <port.h>
#include <wrtype.h>
#include <plyrmode.h>
#include <objshape.h>
#include <render_rt.h>
#include <objmodel.h>
#include <drkwbow.h>   // IsBowEquipped
#include <drkwswd.h>   // WeaponIsSword
#include <weapon.h>    // GetWeaponObjID
#include <string.h>
#include <mprintf.h>
#include <math.h>

#include <dbmem.h> // must be last header

typedef class cPlayerCerebellum
{
public:
   cPlayerCerebellum();
   ~cPlayerCerebellum();

   void StartAction(tPlayerActionType actType = kPlayerActionTypeInvalid);
   void FinishAction();
   void AbortAction();

   void MakeBody();
   void DestroyBody();
   ObjID GetObjID() { return m_RightArmObj; }

   void Update(ulong dt);

   void SetupManeuver(int skillID);
   void SetupMode(int mode);
   void FinishMode();

   cPlayerAbility *GetCurrentAbility() { return m_pCurAbility; }

private:
   void SetInput(ePlayerInput input, tPlayerActionType actType=kPlayerActionTypeInvalid) { m_Input=input; m_ActionType=actType; }

   int GetConnectSkill(int skillNum, ePlayerInput input, tPlayerActionType actType=kPlayerActionTypeInvalid);
   int GetNextSkill();
   void UpdateNextSkill();

   ePlayerTransition GetTransitionType() { return m_pCurAbility?(m_pCurAbility->GetTransitionType(m_CurSkill)):(kTrans_Invalid); }

   IMotionCoordinator *m_pMCoord;
   ObjID m_RightArmObj;
   int m_CurSkill;
   int m_NextSkill;
   ePlayerInput m_Input;
   tPlayerActionType m_ActionType;

   cPlayerAbility *m_pCurAbility;
} cPlayerCerebellum;

// callbacks
static fPlayerFrameCallback g_fFrameCallback=NULL;
static fPlayerAbilityCallback g_fAbilityCallback=NULL;
static fPlayerModeChangeCallback g_fEnterModeCallback=NULL;
static fPlayerModeChangeCallback g_fLeaveModeCallback=NULL;
static fPlayerIdleCallback g_fIdleCallback=NULL;


static cPlayerCerebellum *g_pPlayerCerebellum = NULL;

static int g_NextPlayerMode=kPlayerModeInvalid;
static int g_CurrentPlayerMode=kPlayerModeInvalid;

////////////////////////////////////////////////////////////////
///
///   Property Listeners
///
////////////////////////////////////////////////////////////////

void LGAPI PlayerCreatureListener(sPropertyListenMsg* msg, PropListenerData data)
{
   if(!g_pPlayerCerebellum||g_pPlayerCerebellum->GetObjID()!=msg->obj)
      return;
   if(msg->type&kListenPropUnset)
   {
      g_pPlayerCerebellum->DestroyBody();
   }
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
/////
///// Player Cerebellum Implementation
/////
////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////

void cPlayerCerebellum::DestroyBody()
{
   if(m_pMCoord)
      m_pMCoord->SetMotor(NULL);
   m_CurSkill=kSkillInvalid;

   if(m_RightArmObj!=OBJ_NULL)
   {
      ObjID oldObj=m_RightArmObj;

      m_RightArmObj=OBJ_NULL;
      AutoAppIPtr(DamageModel);
      pDamageModel->SlayObject(oldObj, OBJ_NULL);
   }
}

////////////////////////////////////////////////////////////////

void cPlayerCerebellum::MakeBody()
{
   IObjectSystem* objsys;
   ObjID obj;
   mxs_vector pos;
   mxs_angvec facing;
   IMotor *pMotor;
   const char* name = "PlyrArm";
   int ctype;

   DestroyBody(); // in case it already exists

   if(!m_pCurAbility)
      return;

   objsys = AppGetObj(IObjectSystem);

   obj=m_RightArmObj=objsys->BeginCreate(ROOT_ARCHETYPE,kObjectConcrete);

   // first position at origin, so virtual motion will work.. XXX this is lame KJ 12/97
   mx_zero_vec(&pos);
   facing.tx=facing.ty=facing.tz=0;
   ObjPosUpdate(obj,&pos,&facing);

   objsys->NameObject(obj,name);
   objsys->SetObjTransience(obj,TRUE); 
   objsys->EndCreate(obj);

   SafeRelease(objsys);

   Label *modname;

   if(m_pCurAbility->GetModelName(&modname))
      ObjSetModelName(GetObjID(),modname->text);

   m_pCurAbility->GetCreatureType(&ctype);
   ObjSetCreatureType(obj,ctype);

   ObjSetHasRefs(obj,FALSE); // don't want it to get sorted in scene

   if(NULL!=(pMotor=CreatureGetMotorInterface(obj)))
   {
      m_pMCoord->SetMotor(pMotor);
      pMotor->SetMotFrameOfReference(MFRT_ARM);  // was kMFRT_Global - MTR 8/31
   }
   if (getenv("DARK_PLAYER_TRACE"))
      fprintf(stderr, "Player arm created obj=%d type=%d model=%s motor=%s\n",
              obj, ctype,
              m_pCurAbility->GetModelName(&modname) ? modname->text : "<none>",
              pMotor ? "yes" : "no");

}

////////////////////////////////////////////////////////////////


cPlayerCerebellum::cPlayerCerebellum()
{  
   m_pMCoord=MotSysCreateMotionCoordinator(); 
   m_RightArmObj=OBJ_NULL; 
   m_CurSkill=kSkillInvalid;
   m_NextSkill=kSkillInvalid;
   m_Input=kPlyrInput_None;
   m_pCurAbility=NULL;
}

////////////////////////////////////////////////////////////////

cPlayerCerebellum::~cPlayerCerebellum()
{ 
   if(m_pMCoord)
   {
      delete m_pMCoord;
      m_pMCoord=NULL;
   }   
   if(m_RightArmObj!=OBJ_NULL) 
      DestroyBody(); 

   // for halting sound schemas etc..
   if(m_pCurAbility)
   {
      m_pCurAbility->End();
      m_pCurAbility=NULL;
   }
}

////////////////////////////////////////////////////////////////

void cPlayerCerebellum::SetupManeuver(int skillID)
{
   sMcMoveParams param;
   IMotionPlan *pPlan;

   // clear input
   m_Input=kPlyrInput_None;

   // clean up old skill
   if(m_CurSkill!=kSkillInvalid && m_pCurAbility)
   {
      m_pCurAbility->CleanupSkill(m_CurSkill);
   }

   if(skillID==kSkillInvalid)
   {
      m_pMCoord->SetCurrentManeuver(NULL);
      m_CurSkill=kSkillInvalid;
      return;
   }

   param.tags.Clear();
   param.mask=0;
   if(m_pCurAbility)
   {
      m_pCurAbility->SetupSkill(skillID);
      m_pCurAbility->GetMotionTags(skillID,&param.tags);
   }
   if(NULL!=(pPlan=m_pMCoord->BuildPlan(NULL,&param)))
   {
      m_pMCoord->SetCurrentManeuver(pPlan->PopFirstManeuver());
      delete pPlan;
   }
   m_CurSkill=skillID;
   m_NextSkill=kSkillInvalid; // will need to be re-computed from current skill
   return;
}

////////////////////////////////////////////////////////////////

int cPlayerCerebellum::GetConnectSkill(int skillNum, ePlayerInput input, tPlayerActionType actType)
{
   if(skillNum==kSkillInvalid)
      return kSkillInvalid;

   if(m_pCurAbility)
   {
      return m_pCurAbility->GetNextSkillID(skillNum,input,actType);
   }

   return kSkillInvalid;
}

////////////////////////////////////////////////////////////////

int cPlayerCerebellum::GetNextSkill()
{
   if(m_CurSkill==kSkillInvalid)
      return kSkillInvalid;

   if(m_NextSkill!=kSkillInvalid)
      return m_NextSkill;

   return GetConnectSkill(m_CurSkill, m_Input, m_ActionType);
}

////////////////////////////////////////////////////////////////

void cPlayerCerebellum::UpdateNextSkill()
{
   if(m_NextSkill)
      m_NextSkill=GetConnectSkill(m_NextSkill,m_Input,m_ActionType);
   else
      m_NextSkill=GetConnectSkill(m_CurSkill,m_Input,m_ActionType);
}


////////////////////////////////////////////////////////////////

void cPlayerCerebellum::FinishMode()
{
   SetInput(kPlyrInput_Abort);

   if(GetTransitionType()==kTrans_Immediate)
   {
      if(m_pMCoord)
         m_pMCoord->SetCurrentManeuver(NULL);
      UpdateNextSkill();
   }
}

////////////////////////////////////////////////////////////////

void cPlayerCerebellum::StartAction(tPlayerActionType actionType)
{
   SetInput(kPlyrInput_Start, actionType);
   
   if(GetTransitionType()==kTrans_Immediate)
   {
      if(m_pMCoord)
         m_pMCoord->SetCurrentManeuver(NULL);
      UpdateNextSkill();
   }
}

////////////////////////////////////////////////////////////////

void cPlayerCerebellum::FinishAction()
{
   SetInput(kPlyrInput_Finish);

   if(GetTransitionType()==kTrans_Immediate)
   {
      if(m_pMCoord)
         m_pMCoord->SetCurrentManeuver(NULL);
      UpdateNextSkill();
   }
}

////////////////////////////////////////////////////////////////

void cPlayerCerebellum::AbortAction()
{
   SetInput(kPlyrInput_Abort);
   
   if(GetTransitionType()==kTrans_Immediate)
   {
      if(m_pMCoord)
         m_pMCoord->SetCurrentManeuver(NULL);
      UpdateNextSkill();
   }
}

////////////////////////////////////////////////////////////////

void cPlayerCerebellum::Update(ulong dt)
{
   int skillID;

   if(!m_pMCoord)
      return;

   // do the frame callback
   if(g_fFrameCallback)
   {
      // XXX this should look at return value of frame
      // callback and do something with it.
      (*g_fFrameCallback)(dt,(void *)(intptr_t)g_CurrentPlayerMode);
   }
   if(m_pMCoord->GetStatus()==kMCoord_Idle)
   {
      if(g_NextPlayerMode!=kPlayerModeInvalid)
      {
         // keep going through abort maneuvers until none left, then you're ready
         // to switch.
         SetInput(kPlyrInput_Abort);
         if(kSkillInvalid==(skillID=GetNextSkill()))
         {
            // change modes
            SetupMode(g_NextPlayerMode);
            g_NextPlayerMode=kPlayerModeInvalid;
            return;
         }
      } else
      {
         skillID=GetNextSkill();
      }
      SetupManeuver(skillID);

      // Inform any callbacks if we're idling
      if(m_pCurAbility)
      {
         if(g_fIdleCallback&&m_pCurAbility->IsIdle(skillID))
         {
            g_fIdleCallback(g_CurrentPlayerMode);
         }
      }
   }
}


void cPlayerCerebellum::SetupMode(int mode)
{
   if (getenv("DARK_PLAYER_TRACE"))
      fprintf(stderr, "Player mode setup old=%d new=%d next=%d\n",
              g_CurrentPlayerMode, mode, g_NextPlayerMode);
   // leave current mode
   if(m_pCurAbility)
      m_pCurAbility->End();
   if(g_fLeaveModeCallback)
   {
      g_fLeaveModeCallback(g_CurrentPlayerMode);
   }
   SetupManeuver(kSkillInvalid);
   DestroyBody();
   g_CurrentPlayerMode=mode;


   // start new mode

   if(mode==kPlayerModeInvalid)
      return;

   // must get ability for mode before able to make body
   if(g_fAbilityCallback)
      g_fAbilityCallback(mode,&m_pCurAbility,NULL);

   MakeBody();

   // must make body before calling this callback, since some
   // things want to refer to it here (like sword handler).
   if(g_fEnterModeCallback)
   {
      // this sets the ability pointer for the player mode.
      // this pointer must remain valid while mode is current mode.
      g_fEnterModeCallback(mode);
   }

   if(m_pCurAbility)
   {
      m_pCurAbility->Begin();
      SetupManeuver(m_pCurAbility->GetStartSkillID());
   }
   // (thief.log: each change of what the player holds -- a weapon drawn
   // that never shows starts here or doesn't)
   fprintf(stderr, "Player mode ready mode=%d arm=%d ability=%s skill=%d\n",
              g_CurrentPlayerMode, m_RightArmObj,
              m_pCurAbility ? "yes" : "no", m_CurSkill);
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
/////
///// Global Functions  -- Control Inputs
/////
////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////

void PlayerStartAction()
{
   g_pPlayerCerebellum->StartAction();
}

void PlayerStartActionOfType(tPlayerActionType type)
{
   g_pPlayerCerebellum->StartAction(type);
}

void PlayerFinishAction()
{
   g_pPlayerCerebellum->FinishAction();
}

void PlayerAbortAction()
{
   g_pPlayerCerebellum->AbortAction();
}

ObjID PlayerArm()
{
   if (g_pPlayerCerebellum)
      return g_pPlayerCerebellum->GetObjID();
   return OBJ_NULL; 
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
/////
/////    Schema Utility Funcs
/////
////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////

BOOL PlayerGetMotControllerID(int *pID)
{
   Assert_(g_pPlayerCerebellum);

   cPlayerAbility * pA=g_pPlayerCerebellum->GetCurrentAbility();

   if(!pA)
      return FALSE;

   return pA->GetCurControllerID(pID);
}

BOOL PlayerGetSkillData(sMPlayerSkillData *pSkillData)
{
   Assert_(g_pPlayerCerebellum);

   cPlayerAbility * pA=g_pPlayerCerebellum->GetCurrentAbility();
   sMPlayerSkillData *pD;

   if(!pA)
      return FALSE;

   if(NULL!=(pD=pA->GetCurSkillData()))
   {
      *pSkillData=*pD;
      return TRUE;
   }
   return FALSE;
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
/////
///// Global Functions  -- Cerebellum Setup
/////
////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////

#ifndef SHIP
static void RegisterCerebellumCommands();
#endif


void PlayerCbllmInit()
{
#ifndef SHIP
   RegisterCerebellumCommands();
#endif
}

void PlayerCbllmSetFrameCallback(fPlayerFrameCallback callback)
{
   g_fFrameCallback=callback;
}

void PlayerCbllmSetEnterModeCallback(fPlayerModeChangeCallback callback)
{
   g_fEnterModeCallback=callback;
}

void PlayerCbllmSetAbilityCallback(fPlayerAbilityCallback callback)
{
   g_fAbilityCallback=callback;
}

void PlayerCbllmSetLeaveModeCallback(fPlayerModeChangeCallback callback)
{
   g_fLeaveModeCallback=callback;
}

void PlayerCbllmSetIdleCallback(fPlayerIdleCallback callback)
{
   g_fIdleCallback=callback;
}

void PlayerCbllmCreate()
{
   g_pPlayerCerebellum = new cPlayerCerebellum;

   AssertMsg(g_pPlayerCerebellum,"Could not create player cerebellum\n");
}

static void PlayerBodyReset();   // (below)

void PlayerCbllmDestroy()
{
   PlayerBodyReset();
   PlayerSwitchMode(kPlayerModeInvalid); // so handler knows we're finished
   if(g_pPlayerCerebellum)
   {
      delete g_pPlayerCerebellum;
      g_pPlayerCerebellum=NULL;
   }
}


////////////////////////////////////////////////////////////////
//
// Garrett's body double, for the ray tracer's shadow and reflection of him
// (its setting: RenderRTPlayerBodyActive). A thief of the game's own -- a
// model and the motion-captured walk the guards' thieves have -- made as
// the player's arm is (a transient creature, in no cell, unseen by the
// view), kept where the player stands, facing as he faces or goes, walking,
// running, standing (crouching, where a schema has it) as he does, by the
// motion system as the AIs are (cAIMoveEnactor's tags). The ray tracer
// draws it as its "first-person viewer": the camera never sees it.
//

static ObjID g_BodyObj = OBJ_NULL;
static IMotionCoordinator *g_pBodyMCoord = NULL;
static int g_BodyState = -1;     // what it was last set doing (BodyStateTags)
static float g_BodyButt = 3.0f;  // its object's height over its feet (ft)
static BOOL g_BodyGaveUp = FALSE;

static const char *const g_BodyModels[] =
{
   // Garrett where a mission has him (The Black Parade), else Thief Gold's
   // and Thief 2's hooded thieves (the first of them the mission has)
   "garrett", "thiefsw", "thief", "thiefbow", "thiefsw2",
};

static void PlayerBodyDestroy()
{
   if (g_pBodyMCoord)
   {
      g_pBodyMCoord->SetMotor(NULL);
      delete g_pBodyMCoord;
      g_pBodyMCoord = NULL;
   }
   if (g_BodyObj != OBJ_NULL && ObjectExists(g_BodyObj))
      DestroyObject(g_BodyObj);
   g_BodyObj = OBJ_NULL;
   g_BodyState = -1;
}

// a model the mission has, and a jointed one: a name with no model behind
// it is drawn as the engine's placeholder wedge
static BOOL BodyModelLoads(const char *name)
{
   const int idx = objmodelLoadModel((char *)name);
   return idx != MIDX_INVALID && objmodelGetModelType(idx) == OM_TYPE_MESH;
}

// What he holds: 0 nothing (or the blackjack), 1 his sword, 2 his bow
static int PlayerBodyWeapon(ObjID player)
{
   if (IsBowEquipped())
      return 2;
   const ObjID w = GetWeaponObjID(player);
   return (w != OBJ_NULL && WeaponIsSword(player, w)) ? 1 : 0;
}

// The model for him holding that: the first of g_BodyModels the mission
// has; the bow thief, with his bow out, where it is the hooded thief (a
// Garrett has no bow of his own). And whether its weapon is his: the
// sword thief's sword with his sword out, the bow thief's bow with his bow
// -- else the model's weapon is not drawn (rendobj.cpp)
static char g_BodyModel[32] = "";
static int g_BodyWeapon = -1;
static BOOL g_BodyShowsWeapon = FALSE;

static const char *PlayerBodyModelFor(int weapon)
{
   const char *first = NULL;
   for (int m = 0; m < (int)(sizeof(g_BodyModels) / sizeof(g_BodyModels[0])) && !first; ++m)
      if (BodyModelLoads(g_BodyModels[m]))
         first = g_BodyModels[m];
   if (first && weapon == 2 && strcmp(first, "garrett") != 0 && BodyModelLoads("thiefbow"))
      return "thiefbow";
   return first;
}

static BOOL PlayerBodyMake(const char *model)
{
   PlayerBodyDestroy();
   if (!model)
   {
      mprintf("Garrett's shadow: no thief model found for his body double\n");
      return FALSE;
   }
   IObjectSystem *objsys = AppGetObj(IObjectSystem);
   ObjID obj = objsys->BeginCreate(ROOT_ARCHETYPE, kObjectConcrete);
   mxs_vector pos;
   mxs_angvec facing;
   mx_zero_vec(&pos);
   facing.tx = facing.ty = facing.tz = 0;
   ObjPosUpdate(obj, &pos, &facing);
   objsys->NameObject(obj, "PlyrBody");
   objsys->SetObjTransience(obj, TRUE);
   objsys->EndCreate(obj);
   SafeRelease(objsys);

   ObjSetModelName(obj, model);
   ObjSetCreatureType(obj, 0);   // a humanoid (kDCRTYPE_Humanoid)
   IMotor *pMotor = CreatureExists(obj) ? CreatureGetMotorInterface(obj) : NULL;
   if (!pMotor)
   {
      DestroyObject(obj);
      mprintf("Garrett's shadow: \"%s\" made no creature\n", model);
      return FALSE;
   }
   ObjSetHasRefs(obj, FALSE);    // in no cell: the view never draws it
   CreatureMakeNonPhysical(obj);
   g_BodyObj = obj;
   g_pBodyMCoord = MotSysCreateMotionCoordinator();
   g_pBodyMCoord->SetMotor(pMotor);
   g_BodyState = -1;
   strncpy(g_BodyModel, model, sizeof(g_BodyModel) - 1);
   g_BodyModel[sizeof(g_BodyModel) - 1] = 0;
   mprintf("Garrett's shadow: his body double is a \"%s\" (object %d)\n", model, obj);
   return TRUE;
}

BOOL PlayerBodyShowsWeapon()
{
   return g_BodyShowsWeapon;
}

// 0 standing, 1 walking, 2 running, 3 walking backwards, 4 crouching,
// 5 creeping (crouched, moving)
static const char *BodyStateTags(int state)
{
   switch (state)
   {
      case 1: return "Locomote 0";
      case 2: return "Locomote 0, LocoUrgent 0";
      case 3: return "Locomote 0, Direction 4";
      case 4: return "Crouch 0";
      case 5: return "Locomote 0, Crouch 0";
      default: return "Stand 0";
   }
}

static void PlayerBodyUpdate(ulong dt)
{
   ObjID player = PlayerObjectExists() ? PlayerObject() : OBJ_NULL;
   if (!RenderRTPlayerBodyActive() || player == OBJ_NULL)
   {
      if (g_BodyObj != OBJ_NULL)
         PlayerBodyDestroy();
      return;
   }
   if (g_BodyObj == OBJ_NULL || !ObjectExists(g_BodyObj) || !CreatureExists(g_BodyObj))
   {
      // (a load or a new mission takes transient objects away)
      if (g_BodyObj != OBJ_NULL)
      {
         if (g_pBodyMCoord)
         {
            delete g_pBodyMCoord;
            g_pBodyMCoord = NULL;
         }
         g_BodyObj = OBJ_NULL;
      }
      g_BodyWeapon = -1;   // (made below)
   }
   {
      // the model for what he holds (made again when that changes it)
      const int weapon = PlayerBodyWeapon(player);
      if (weapon != g_BodyWeapon || g_BodyObj == OBJ_NULL)
      {
         if (g_BodyGaveUp)
            return;
         const char *model = PlayerBodyModelFor(weapon);
         if (g_BodyObj == OBJ_NULL || !model || strcmp(model, g_BodyModel) != 0)
         {
            if (!PlayerBodyMake(model))
            {
               g_BodyGaveUp = TRUE;
               return;
            }
         }
         g_BodyWeapon = weapon;
         const BOOL bow = strcmp(g_BodyModel, "thiefbow") == 0;
         g_BodyShowsWeapon = (weapon == 2 && bow) || (weapon == 1 && !bow);
      }
   }

   // where he stands and where he looks, and how he goes
   mxs_vector camPos;
   mxs_angvec camAng;
   CameraGetLocation(PlayerCamera(), &camPos, &camAng);
   mxs_vector foot;
   PhysGetSubModLocation(player, PLAYER_FOOT, &foot);
   {
      // the floor under his feet
      Location a, b, hit;
      mxs_vector below = foot;
      below.z -= 3.0f;
      MakeLocationFromVector(&a, &foot);
      MakeLocationFromVector(&b, &below);
      if (!PortalRaycast(&a, &b, &hit, 0))
         foot.z = hit.vec.z;
      else
         foot.z -= 0.5f;
   }
   mxs_vector vel;
   PhysGetVelocity(player, &vel);
   const float speed = sqrtf(vel.x * vel.x + vel.y * vel.y);
   const mxs_ang look = camAng.tz;
   const mxs_ang going = (mxs_ang)(atan2f(vel.y, vel.x) / (2.0f * 3.14159265f) * 65536.0f);
   const short ahead = (short)(going - look);   // how far from where he looks he goes
   const BOOL crouched = GetPlayerMode() == kPM_Crouch;
   // Facing: where he looks; going sideways (a humanoid has no side step
   // in its schemas) turned part of the way, a third of a right angle at
   // most, walking forward -- turned all the way it looked like walking off
   // to the side. Backing away: walking backwards, facing ahead.
   int state;
   short turn = 0;
   if (speed < 1.0f)
      state = crouched ? 4 : 0;
   else if (ahead > 0x6000 || ahead < -0x6000)
      state = crouched ? 5 : 3;
   else
   {
      state = crouched ? 5 : speed > 11.0f ? 2 : 1;
      turn = ahead > 0x1555 ? 0x1555 : ahead < -0x1555 ? -0x1555 : ahead;
   }
   // turning as a body turns, not at once: up to a turn and a half a second
   static mxs_ang s_facing = 0;
   static BOOL s_facingSet = FALSE;
   const mxs_ang want = (mxs_ang)(look + turn);
   if (!s_facingSet)
      s_facing = want, s_facingSet = TRUE;
   {
      short d = (short)(want - s_facing);
      const int most = (int)(dt * 98.3f) + 1;   // 65536 * 1.5 / 1000 per ms
      if (d > most) d = (short)most;
      else if (d < -most) d = (short)-most;
      s_facing = (mxs_ang)(s_facing + d);
   }
   mxs_ang facingTo = s_facing;

   // kept where he stands: the creature's own walk moves it a little each
   // frame, put back (its feet on his floor)
   mxs_vector mn, mx;
   Position *bp = ObjPosGet(g_BodyObj);
   if (bp && ObjGetWorldBBox(g_BodyObj, &mn, &mx) && mx.z > mn.z)
   {
      const float butt = bp->loc.vec.z - mn.z;
      if (butt > 1.0f && butt < 6.0f)
         g_BodyButt += (butt - g_BodyButt) * 0.1f;
   }
   mxs_vector at = foot;
   at.z += g_BodyButt;
   mxs_angvec fac;
   fac.tx = fac.ty = 0;
   fac.tz = facingTo;
   if (RenderRTPlayerBodyActive() == 2)
   {
      // shown (the setting's test): seven feet ahead of him, facing as
      // he faces, doing as he does -- to see what makes the shadow
      const float a = look * (2.0f * 3.14159265f / 65536.0f);
      at.x += cosf(a) * 7.0f;
      at.y += sinf(a) * 7.0f;
   }
   ObjPosUpdate(g_BodyObj, &at, &fac);

   // what it is doing: as the AIs' bodies are told (cAIMoveEnactor)
   const eMCoordStatus status = g_pBodyMCoord->GetStatus();
   if (state != g_BodyState || status == kMCoord_Idle || status == kMCoord_ActiveInterrupt)
   {
      sMcMoveParams params;
      params.mask = kMotParmFlag_Direction | kMotParmFlag_Duration | kMotParmFlag_Facing;
      params.direction.tx = params.direction.ty = 0;
      params.direction.tz = (state == 0 || state == 4) ? facingTo : going;   // (the way he goes)
      params.facing = fac;
      params.duration = dt;
      params.tags = cTagSet(BodyStateTags(state));
      if (state == 1 || state == 2 || state == 3 || state == 5)
      {
         params.distance = speed * 0.5f;
         params.mask |= kMotParmFlag_Distance;
      }
      IMotionPlan *pPlan = g_pBodyMCoord->BuildPlan(g_pBodyMCoord->GetNextEndState(), &params);
      if (!pPlan && (state == 4 || state == 5))
      {
         // no crouching schema for it: standing, walking
         params.tags = cTagSet(BodyStateTags(state == 4 ? 0 : 1));
         pPlan = g_pBodyMCoord->BuildPlan(g_pBodyMCoord->GetNextEndState(), &params);
      }
      if (pPlan)
      {
         g_pBodyMCoord->SetCurrentManeuver(pPlan->PopFirstManeuver());
         delete pPlan;
      }
      g_BodyState = state;
   }
}

static void PlayerBodyReset()
{
   PlayerBodyDestroy();
   g_BodyGaveUp = FALSE;
}

ObjID PlayerBodyDouble()
{
   return (g_BodyObj != OBJ_NULL && ObjectExists(g_BodyObj)) ? g_BodyObj : OBJ_NULL;
}

void PlayerCbllmUpdate(ulong dt)
{
   if(g_pPlayerCerebellum)
   {
      g_pPlayerCerebellum->Update(dt);
   }
   PlayerBodyUpdate(dt);
}

// @TODO: take power out of plycbllm
BOOL PlayerCbllmGetPower(float *pPower)
{
   *pPower=1.0;
   return TRUE;
}

void PlayerSwitchMode(int mode)
{
   // do nothing if same as current mode, except perhaps switch bow model..
   if(mode==g_CurrentPlayerMode&&g_NextPlayerMode==kPlayerModeInvalid)
   {
      return;
   }
   if(mode==kPlayerModeInvalid) // must switch NOW
   {
      // change modes
      g_NextPlayerMode=mode;
      if(g_pPlayerCerebellum)
         g_pPlayerCerebellum->SetupMode(mode);
   } else
   {
      if(g_NextPlayerMode==kPlayerModeInvalid) // so not already finishing
      {
         g_pPlayerCerebellum->FinishMode();
      }
      g_NextPlayerMode=mode;
   }
}

int PlayerGetMode()
{
   return g_CurrentPlayerMode;
}

int PlayerGetNextMode()
{
   return g_NextPlayerMode;
}

////////////////////////////////////////////////////
//
// PLAYER ARM ALIGNMENT UTILITIES
//
// for debugging/tweaking purposes
// @TODO: reimplement these
#ifndef SHIP

EXTERN BOOL g_PlayerArmAttached;

Command PlayerCerebellumCommands[] =
{
   { "player_detach_arm", TOGGLE_BOOL,&g_PlayerArmAttached, "toggle whether arm attached to player camera"},
   { "player_spew_arm_offset", FUNC_VOID, PlayerCbllmSpewArmOffset, "spew arm offset from camera to monochrome" },
};

void RegisterCerebellumCommands()
{
   COMMANDS(PlayerCerebellumCommands,HK_GAME_MODE);
}

static BOOL CalcArmOffset(mxs_vector *pArmRelPos, mxs_angvec *pArmRelAng)
{
   Position *pArmPos;
   mxs_vector camPos;
   mxs_matrix mat,armRelOrient,armWorldOrient;
   mxs_angvec camAng;
   ObjID armObj;

   if(!g_pPlayerCerebellum)
      return FALSE;
   if(OBJ_NULL==(armObj=g_pPlayerCerebellum->GetObjID() ) )
      return FALSE;
   if(NULL==(pArmPos=ObjPosGet(armObj)))
      return FALSE;
   CameraGetLocation(PlayerCamera(),&camPos,&camAng);
   mx_sub_vec(pArmRelPos,&pArmPos->loc.vec,&camPos);
   mx_ang2mat(&mat,&camAng);
   mx_transeq_mat(&mat); // inverse in conjugate transpose for unitary matrices
   mx_mat_muleq_vec(&mat,pArmRelPos); // offset of arm in camera space

   mx_ang2mat(&armWorldOrient,&pArmPos->fac);
   mx_mul_mat(&armRelOrient,&mat,&armWorldOrient); // arm orient in camera space
   mx_mat2ang(pArmRelAng,&armRelOrient);

   return TRUE;
}


void PlayerCbllmSpewArmOffset()
{
   mxs_vector armRelPos;
   mxs_angvec armRelAng;

   if(!CalcArmOffset(&armRelPos,&armRelAng))
      return;
   mprintf("\n\n");
   mprintf("arm position: %g, %g, %g\n",armRelPos.x,armRelPos.y,armRelPos.z);   
   mprintf("arm orientation: %d, %d, %d\n",fixang_to_degrees(armRelAng.tx),\
   fixang_to_degrees(armRelAng.ty), fixang_to_degrees(armRelAng.tz) );
}

#endif // ifndef SHIP


