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
#include <crjoint.h>   // GetCreatureJointPos
#include <mvrflags.h>  // MF_FIRE_RELEASE
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
   int GetCurSkill() { return m_CurSkill; }

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
static int g_BodyPosed = 0;      // updates since its first motion (drawn after: not in its bind pose)
static char g_BodyTags[128] = "";   // what it was last set doing
static BOOL g_BodyOneShot = FALSE;  // that, once through (a swing, a shot, readying)
// His bow drawn and held: the archer's shot stopped at its loosing (its
// motion's fire-release flag), the creature frozen there until he lets go
static BOOL g_BodyAiming = FALSE, g_BodyHeld = FALSE;

static void BodyFireRelease(ObjID obj, IMotor *, cMotionCoordinator *, const int)
{
   if (obj == g_BodyObj && g_BodyAiming && !g_BodyHeld)
   {
      CreatureFreeze(obj);
      g_BodyHeld = TRUE;
   }
}

static const char *const g_BodyModels[] =
{
   // Garrett where a mission has him (The Black Parade), else Thief Gold's
   // and Thief 2's hooded thieves (the first of them the mission has)
   "garrett", "thiefsw", "thief", "thiefbow", "thiefsw2",
};

static void PlayerBodyItemDestroy();   // (below)

static void PlayerBodyDestroy()
{
   if (g_pBodyMCoord)
   {
      g_pBodyMCoord->SetMotor(NULL);
      delete g_pBodyMCoord;
      g_pBodyMCoord = NULL;
   }
   if (g_BodyObj != OBJ_NULL && ObjectExists(g_BodyObj))
   {
      RemoveMotionFlagListener(g_BodyObj, MF_FIRE_RELEASE);
      DestroyObject(g_BodyObj);
   }
   g_BodyObj = OBJ_NULL;
   g_BodyAiming = g_BodyHeld = FALSE;
   PlayerBodyItemDestroy();
   g_BodyState = -1;
   g_BodyTags[0] = 0;
   g_BodyPosed = 0;
}

// a model the mission has, and a jointed one: a name with no model behind
// it is drawn as the engine's placeholder wedge
static BOOL BodyModelLoads(const char *name)
{
   const int idx = objmodelLoadModel((char *)name);
   return idx != MIDX_INVALID && objmodelGetModelType(idx) == OM_TYPE_MESH;
}

// What he holds: 0 nothing, 1 his sword, 2 his bow, 3 his blackjack
static int PlayerBodyWeapon(ObjID player)
{
   if (IsBowEquipped())
      return 2;
   const ObjID w = GetWeaponObjID(player);
   if (w == OBJ_NULL)
      return 0;
   return WeaponIsSword(player, w) ? 1 : WeaponIsBlackjack(player, w) ? 3 : 0;
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
   AddMotionFlagListener(obj, MF_FIRE_RELEASE, BodyFireRelease);
   strncpy(g_BodyModel, model, sizeof(g_BodyModel) - 1);
   g_BodyModel[sizeof(g_BodyModel) - 1] = 0;
   mprintf("Garrett's shadow: his body double is a \"%s\" (object %d)\n", model, obj);
   return TRUE;
}

BOOL PlayerBodyShowsWeapon()
{
   return g_BodyShowsWeapon;
}

// What he holds that no thief model has -- his blackjack: the game's own
// model of it (the one his blackjack has, else Thief's "blacjack"), made
// as the double is (transient, in no cell) and put in its right hand as it
// is drawn (PlayerBodyHeldItem)
static ObjID g_BodyItem = OBJ_NULL;

static void PlayerBodyItemDestroy()
{
   if (g_BodyItem != OBJ_NULL && ObjectExists(g_BodyItem))
      DestroyObject(g_BodyItem);
   g_BodyItem = OBJ_NULL;
}

static void PlayerBodyItemMake(ObjID player)
{
   PlayerBodyItemDestroy();
   char model[64] = "blacjack";
   {
      char name[64];
      const ObjID w = GetWeaponObjID(player);
      if (w != OBJ_NULL && ObjGetModelName(w, name) && objmodelLoadModel(name) != MIDX_INVALID)
         strcpy(model, name);
   }
   if (objmodelLoadModel(model) == MIDX_INVALID)
   {
      mprintf("Garrett's shadow: no model for his blackjack (\"%s\")\n", model);
      return;
   }
   IObjectSystem *objsys = AppGetObj(IObjectSystem);
   ObjID obj = objsys->BeginCreate(ROOT_ARCHETYPE, kObjectConcrete);
   mxs_vector pos;
   mxs_angvec facing;
   mx_zero_vec(&pos);
   facing.tx = facing.ty = facing.tz = 0;
   ObjPosUpdate(obj, &pos, &facing);
   objsys->NameObject(obj, "PlyrBodyItem");
   objsys->SetObjTransience(obj, TRUE);
   objsys->EndCreate(obj);
   SafeRelease(objsys);
   ObjSetModelName(obj, model);
   ObjSetHasRefs(obj, FALSE);    // in no cell: the view never draws it
   g_BodyItem = obj;
   mprintf("Garrett's shadow: his blackjack is a \"%s\" (object %d)\n", model, obj);
}

ObjID PlayerBodyHeldItem()
{
   if (g_BodyItem == OBJ_NULL || !ObjectExists(g_BodyItem) || g_BodyObj == OBJ_NULL ||
       !CreatureExists(g_BodyObj))
      return OBJ_NULL;
   // In the right wrist's frame (a thief model's) the hand runs out along
   // -y, the forearm's line, flat across z (its palm), its fist 0.45 ft
   // out; a sword runs on along -y (tipped a little toward +x, the thumb).
   // The blackjack model lies along z, its handle at -z (its end at -0.98),
   // its head at +z. Held upright, as he holds it: across the fist, its
   // head out at the thumb (+x), its grip (0.7 ft from its middle) in the
   // fist -- along the forearm it hung from the hand.
   const mxs_vector &wrist = GetCreatureJointPos(g_BodyObj, kCJ_RWrist);
   const mxs_matrix &orient = GetCreatureJointOrient(g_BodyObj, kCJ_RWrist);
   mxs_matrix r, m;
   mx_mk_vec(&r.vec[0], 0, 1, 0);    // (the model's x, y, z in the wrist's frame)
   mx_mk_vec(&r.vec[1], 0, 0, 1);
   mx_mk_vec(&r.vec[2], 1, 0, 0);
   mx_mul_mat(&m, &orient, &r);
   mxs_vector rel, at;
   mx_mk_vec(&rel, 0.75f, -0.45f, -0.04f);
   mx_mat_mul_vec(&at, &orient, &rel);
   mx_addeq_vec(&at, &wrist);
   mxs_angvec fac;
   mx_mat2ang(&fac, &m);
   ObjPosUpdate(g_BodyItem, &at, &fac);
   return g_BodyItem;
}

// What his hands do, from the first-person arm's skill (this cerebellum's):
// the sword drawn and ready (its wind-up too), swung, blocking, being drawn;
// the bow lifted, drawn, loosed, being readied. A skill without a sword tag
// (held at the block, held wound up) goes on as the one before it.
enum { kBodyArm_None, kBodyArm_Ready, kBodyArm_Swing, kBodyArm_Block, kBodyArm_Readying, kBodyArm_Aim,
       kBodyArm_Loose };

static int BodyTagValue(const cTagSet &tags, const char *type)
{
   for (int i = 0; i < tags.Size(); ++i)
   {
      const cTag *t = tags.GetEntry(i);
      if (strcasecmp(t->GetType(), type) == 0)
         return t->GetIntValue();
   }
   return -1;
}

static int PlayerBodyArmAction(int weapon, int *swing)
{
   static int s_sword = kBodyArm_None, s_swing = 0;
   *swing = s_swing;
   if (!g_pPlayerCerebellum || weapon == 0)
      return s_sword = kBodyArm_None;
   cPlayerAbility *pA = g_pPlayerCerebellum->GetCurrentAbility();
   const int skill = g_pPlayerCerebellum->GetCurSkill();
   if (!pA || skill == kSkillInvalid)
      return kBodyArm_None;
   if (weapon == 2)
   {
      // drkwbow_.c: 0 idle, 1 lift, 2 draw, 3 loose, 4 relax, 5 begin, 6 end
      switch (skill)
      {
         case 1: case 2: return kBodyArm_Aim;
         case 3: return kBodyArm_Loose;
         case 5: return kBodyArm_Readying;
         default: return kBodyArm_None;
      }
   }
   // drkwswd_.c: PlyrSword 0 ready, 1 wind-up, 2 swing (PlyrSwordSwing 0
   // short, 1 medium, 2 long), 3 block, 4 begin, 5 end, 6 directed block
   cTagSet tags;
   if (!pA->GetMotionTags(skill, &tags))
      return s_sword;
   switch (BodyTagValue(tags, "PlyrSword"))
   {
      case 0: case 1: s_sword = kBodyArm_Ready; break;
      case 2:
         s_sword = kBodyArm_Swing;
         s_swing = BodyTagValue(tags, "PlyrSwordSwing");
         if (s_swing < 0) s_swing = 0;
         break;
      case 3: case 6: s_sword = kBodyArm_Block; break;
      case 4: s_sword = kBodyArm_Readying; break;
      case 5: s_sword = kBodyArm_None; break;
      default: break;
   }
   *swing = s_swing;
   return s_sword;
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
         if (g_BodyObj == OBJ_NULL || !model)
         {
            if (!PlayerBodyMake(model))
            {
               g_BodyGaveUp = TRUE;
               return;
            }
         }
         else if (strcmp(model, g_BodyModel) != 0)
         {
            // the same creature, another model (a humanoid's both): it goes
            // on with what it was doing -- made anew, it stood in its bind
            // pose (arms out) until its first motion
            ObjSetModelName(g_BodyObj, model);
            strncpy(g_BodyModel, model, sizeof(g_BodyModel) - 1);
            g_BodyModel[sizeof(g_BodyModel) - 1] = 0;
            mprintf("Garrett's shadow: his body double is now a \"%s\"\n", model);
         }
         g_BodyWeapon = weapon;
         const BOOL bow = strcmp(g_BodyModel, "thiefbow") == 0;
         g_BodyShowsWeapon = (weapon == 2 && bow) || (weapon == 1 && !bow);
         if (weapon == 3)
            PlayerBodyItemMake(player);
         else
            PlayerBodyItemDestroy();
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
   // What he does: with his sword out, as a guard fights -- stepping
   // forward, back and to the side facing ahead (the schemas' sword
   // fighting moves), swinging, blocking; with his bow, an archer's
   // shot as he draws it; drawing either, a guard's readying of it. Else
   // (and running, crouching): walking. Facing where he looks, whichever
   // way he goes: sideways, walking on (a humanoid has no side step but
   // the sword fighter's) -- turned toward the way he went, it swung round
   // as he stopped or changed sides. Backing away: walking backwards.
   int swing = 0;
   const BOOL armed = g_BodyShowsWeapon || (g_BodyWeapon == 3 && g_BodyItem != OBJ_NULL);
   const int arm = armed ? PlayerBodyArmAction(g_BodyWeapon, &swing) : kBodyArm_None;
   // (the bow held drawn: let go -- loosed, relaxed, put away -- the shot
   // goes on from where it stopped)
   g_BodyAiming = arm == kBodyArm_Aim && !crouched;
   if (g_BodyHeld && !g_BodyAiming)
   {
      CreatureUnFreeze(g_BodyObj);
      g_BodyHeld = FALSE;
   }
   int state;
   const BOOL backing = ahead > 0x6000 || ahead < -0x6000;
   if (speed < 1.0f)
      state = crouched ? 4 : 0;
   else if (backing)
      state = crouched ? 5 : 3;
   else
      state = crouched ? 5 : speed > 11.0f ? 2 : 1;
   char act[96] = "";
   BOOL oneShot = FALSE, stepping = FALSE;
   if (!crouched && state != 2)
   {
      const char *item = g_BodyWeapon == 2 ? "ItemBow" : "ItemSword";
      switch (arm)
      {
         case kBodyArm_Swing:
            // (short: at the middle; medium: high; long: low; the
            // blackjack: high, over his head)
            sprintf(act, "MeleeCombat 0, Attack 0, Direction %d",
                    g_BodyWeapon == 3 || swing == 1 ? 5 : swing == 2 ? 6 : 0);
            oneShot = TRUE;
            break;
         case kBodyArm_Block:
            if (g_BodyWeapon == 1)
               strcpy(act, "MeleeCombat 0, Block 0");
            break;
         case kBodyArm_Readying:
            sprintf(act, "ReadyItem 0, %s 0", item);
            oneShot = TRUE;
            break;
         case kBodyArm_Aim:
         case kBodyArm_Loose:
            strcpy(act, "RangedCombat 0");
            oneShot = TRUE;
            break;
         case kBodyArm_Ready:
            // (the blackjack held as he walks: no fighting stance)
            if (g_BodyWeapon == 3)
               break;
            if (speed < 1.0f)
               strcpy(act, "MeleeCombat 0, Stand 0");
            else
            {
               // Direction 1 left, 2 right, 3 forward, 4 backward
               const int dir = (ahead < 0x2000 && ahead > -0x2000) ? 3
                             : (ahead > 0x6000 || ahead < -0x6000) ? 4
                             : ahead > 0 ? 1 : 2;
               sprintf(act, "MeleeCombat 0, Locomote 0, Direction %d", dir);
               stepping = TRUE;
            }
            break;
      }
      // (a swing, a shot, a readying once through: what he does besides,
      // until his hands do something else)
      static int s_lastArm = -1;
      static char s_done[96] = "";
      if (arm != s_lastArm)
         s_done[0] = 0, s_lastArm = arm;
      if (act[0] && g_BodyOneShot && g_pBodyMCoord->GetStatus() == kMCoord_Idle &&
          strncmp(g_BodyTags, act, strlen(act)) == 0)
         strcpy(s_done, act);
      if (oneShot && strcmp(act, s_done) == 0)
         act[0] = 0, oneShot = FALSE;
   }
   // turning as a body turns, not at once: up to a turn and a half a second
   static mxs_ang s_facing = 0;
   static BOOL s_facingSet = FALSE;
   const mxs_ang want = look;
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

   // what it is doing: as the AIs' bodies are told (cAIMoveEnactor), with
   // the weapon he holds as an armed AI's persistent tag
   const char *with = !armed ? "" : g_BodyWeapon == 2 ? ", WithBow 0" : ", WithSword 0";
   char tags[128];
   snprintf(tags, sizeof(tags), "%s%s", act[0] ? act : BodyStateTags(state), with);
   const eMCoordStatus status = g_pBodyMCoord->GetStatus();
   // (a swing, a shot, a readying not cut short: his own swing is over
   // well before a guard's, and his hands back at the ready cut it to a
   // flick -- only another of them, or putting the weapon away, does)
   const BOOL busy = g_BodyOneShot && status != kMCoord_Idle && g_BodyTags[0] && armed &&
                     !(oneShot && strncmp(g_BodyTags, act, strlen(act)) != 0);
   if (!busy && (strcmp(tags, g_BodyTags) != 0 || status == kMCoord_Idle || status == kMCoord_ActiveInterrupt))
   {
      sMcMoveParams params;
      params.mask = kMotParmFlag_Direction | kMotParmFlag_Duration | kMotParmFlag_Facing;
      params.direction.tx = params.direction.ty = 0;
      const BOOL moving = stepping || (!act[0] && (state == 1 || state == 2 || state == 3 || state == 5));
      // (the way he goes, stepping or backing away; else ahead -- walking
      // sideways he walks on facing ahead)
      params.direction.tz = (stepping || (moving && backing && !act[0])) ? going : facingTo;
      params.facing = fac;
      params.duration = dt;
      if (moving)
      {
         params.distance = speed * 0.5f;
         params.mask |= kMotParmFlag_Distance;
      }
      // what it is asked, then without the weapon's tag, then the walk
      // (crouched: standing, walking) -- the first the schemas have
      char tries[4][128];
      int n = 0;
      strcpy(tries[n++], tags);
      if (with[0])
         strcpy(tries[n++], act[0] ? act : BodyStateTags(state));
      if (act[0])
         strcpy(tries[n++], BodyStateTags(state));
      if (state == 4 || state == 5)
         strcpy(tries[n++], BodyStateTags(state == 4 ? 0 : 1));
      IMotionPlan *pPlan = NULL;
      int got = -1;
      for (int i = 0; i < n && !pPlan; ++i)
      {
         params.tags = cTagSet(tries[i]);
         pPlan = g_pBodyMCoord->BuildPlan(g_pBodyMCoord->GetNextEndState(), &params);
         if (pPlan)
            got = i;
      }
      if (pPlan)
      {
         g_pBodyMCoord->SetCurrentManeuver(pPlan->PopFirstManeuver());
         delete pPlan;
      }
      static int s_logged = 0;
      if (strcmp(tags, g_BodyTags) != 0 && s_logged < 200)
      {
         ++s_logged;
         if (got == 0)
            mprintf("PlyrBody: %s\n", tags);
         else if (got > 0)
            mprintf("PlyrBody: %s -- no schema, so %s\n", tags, tries[got]);
         else
            mprintf("PlyrBody: %s -- no schema (nor for its fallbacks)\n", tags);
      }
      strcpy(g_BodyTags, tags);
      g_BodyOneShot = oneShot;
      g_BodyState = state;
   }
   if (g_BodyState != -1 && g_BodyPosed < 1000)
      ++g_BodyPosed;
}

static void PlayerBodyReset()
{
   PlayerBodyDestroy();
   g_BodyGaveUp = FALSE;
}

ObjID PlayerBodyDouble()
{
   // (not until its first motion has posed it)
   return (g_BodyObj != OBJ_NULL && ObjectExists(g_BodyObj) && g_BodyPosed >= 2) ? g_BodyObj : OBJ_NULL;
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


