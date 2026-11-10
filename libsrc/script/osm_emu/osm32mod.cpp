// Native Dark Engine adapters for portable 32-bit OSM modules.

#include <osm_emu/osm32mod.h>

#include <osm_emu/osm32env.h>
#include <appagg.h>
#include <bintrait.h>
#include <linkman.h>
#include <linkid.h>
#include <linkbase.h>
#include <mprintf.h>
#include <property.h>
#include <propman.h>
#include <relation.h>
#include <scrptsrv.h>
#include <arscrs.h>
#include <arscrm.h>
#include <atkscrpt.h>
#include <bodscrpt.h>
#include <contscrm.h>
#include <damgscrm.h>
#include <datascrs.h>
#include <frobscrm.h>
#include <linkscpt.h>
#include <lockscpt.h>
#include <objscrpt.h>
#include <propscpt.h>
#include <rooscrpt.h>
#include <scrptmsg.h>
#include <simscrm.h>
#include <sndscrpt.h>

#pragma pack(push, 8)
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#pragma pack(pop)

#include <aiscrptapi.h>
#include <traitman.h>
#include <traittyp.h>
#include <traitbas.h>
#include <dspchdef.h>
#include <engscrpt.h>
#include <objquery.h>
#include <simman.h>
#include <propbase.h>
#include <propface.h>
#include <pfaceiid.h>
#include <map>
#include <drkuiscrapi.h>

// (live guest link queries, across every script module: for perf.log)
extern "C" { long g_Osm32LinkQueries = 0; }

namespace
{

const uint32_t kLinkQueryTrap = 0xf3000000;
const uint32_t kLinkQueryTrapStride = 16;
const uint32_t kReportBufferCapacity = 256 * 1024;

struct sNativeQuestMessage : public sScrMsg
{
   const char *name;
   int oldValue;
   int newValue;
};

struct sNativeDoorMessage : public sScrMsg
{
   int actionType;
   int previousActionType;
   BOOL isProxy;
};

struct sNativeMediumTransitionMessage : public sScrMsg
{
   int fromType;
   int toType;
};

struct sNativeTweqMessage : public sScrMsg
{
   int type;
   int operation;
   int direction;
};

struct sNativePhysMessage : public sScrMsg
{
   int Submod;
   int collType;
   ObjID collObj;
   int collSubmod;
   float collMomentum;
   mxs_vector collNormal;
   mxs_vector collPt;
   int contactType;
   ObjID contactObj;
   int contactSubmod;
   ObjID transObj;
   int transSubmod;
};

struct sNativeMovingTerrainMessage : public sScrMsg
{
   ObjID waypoint;
};

struct sNativeWaypointMessage : public sScrMsg
{
   ObjID moving_terrain;
};

// Keep message decoding independent of subsystem-private headers. These
// mirrors contain only the public fields following the native sScrMsg base;
// guest layout is written explicitly below and never inferred from sizeof.
struct sNativeDarkGameModeMessage : public sScrMsg
{
   int resuming;
   int suspending;
};

struct sNativeAISignalMessage : public sScrMsg
{
   cScrStr signal;
};

struct sNativeAIPatrolPointMessage : public sScrMsg
{
   ObjID patrolObj;
};

struct sNativeAIAlertMessage : public sScrMsg
{
   int level;
   int oldLevel;
};

struct sNativeAIModeChangeMessage : public sScrMsg
{
   int mode;
   int previousMode;
};

struct sNativeAIObjActResultMessage : public sScrMsg
{
   int action;
   int result;
   cMultiParm data;
   ObjID target;
};

struct sNativeDifficultyMessage : public sScrMsg
{
   int difficulty;
};

struct sNativeReportMessage : public sScrMsg
{
   int warnLevel;
   int flags;
   int types;
   char *textBuffer;
};

struct sNativePickStateMessage : public sScrMsg
{
   int previousState;
   int currentState;
};

bool ToGuestValue(const sMultiParm &source, osm32::sValue *destination)
{
   destination->type = static_cast<osm32::eValueType>(source.type);
   destination->bits = 0;
   destination->text.clear();
   destination->vector[0] = destination->vector[1] =
      destination->vector[2] = 0.0f;

   switch (source.type)
   {
      case kMT_Undef:
         return true;
      case kMT_Int:
         destination->bits = static_cast<uint32_t>(source.i);
         return true;
      case kMT_Float:
         memcpy(&destination->bits, &source.f, sizeof(destination->bits));
         return true;
      case kMT_String:
         destination->text = source.psz ? source.psz : "";
         return true;
      case kMT_Vector:
         if (source.pVec)
         {
            destination->vector[0] = source.pVec->x;
            destination->vector[1] = source.pVec->y;
            destination->vector[2] = source.pVec->z;
         }
         return true;
      case kMT_Obj:
         destination->bits = static_cast<uint32_t>(source.o);
         return true;
      default:
         return false;
   }
}

bool FromGuestValue(const osm32::sValue &source, sMultiParm *destination)
{
   if (!destination)
      return true;

   ClearParm(destination);
   switch (source.type)
   {
      case osm32::kValueUndefined:
         return true;
      case osm32::kValueInteger:
         destination->i = static_cast<int32_t>(source.bits);
         destination->type = kMT_Int;
         return true;
      case osm32::kValueFloat:
         memcpy(&destination->f, &source.bits, sizeof(destination->f));
         destination->type = kMT_Float;
         return true;
      case osm32::kValueString:
         InitStrParm(destination, source.text.c_str());
         return true;
      case osm32::kValueVector:
      {
         mxs_vector vector;
         vector.x = source.vector[0];
         vector.y = source.vector[1];
         vector.z = source.vector[2];
         InitVecParm(destination, vector);
         return true;
      }
      case osm32::kValueObject:
         destination->o = static_cast<ObjID>(source.bits);
         destination->type = kMT_Obj;
         return true;
      default:
         return false;
   }
}

void Append32(std::vector<uint8_t> *target, uint32_t value)
{
   target->push_back(static_cast<uint8_t>(value));
   target->push_back(static_cast<uint8_t>(value >> 8));
   target->push_back(static_cast<uint8_t>(value >> 16));
   target->push_back(static_cast<uint8_t>(value >> 24));
}

void AppendFloat(std::vector<uint8_t> *target, float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   Append32(target, bits);
}

void AppendString(std::vector<uint8_t> *target,
                  std::vector<osm32::sMessage::sStringPatch> *patches,
                  const char *value)
{
   osm32::sMessage::sStringPatch patch;
   patch.offset = static_cast<uint32_t>(target->size());
   patch.value = value ? value : "";
   Append32(target, 0);
   patches->push_back(patch);
}

void AppendValue(std::vector<uint8_t> *target,
                 std::vector<osm32::sMessage::sValuePatch> *patches,
                 const sMultiParm &value)
{
   osm32::sMessage::sValuePatch patch;
   patch.offset = static_cast<uint32_t>(target->size());
   ToGuestValue(value, &patch.value);
   Append32(target, 0);
   Append32(target, 0);
   patches->push_back(patch);
}

void MarshalMessageExtension(const sScrMsg *message,
                             osm32::sMessage *guest)
{
   const char *name = message->message ? message->message : "";
   if (!strcmp(name, "PlayerRoomEnter") ||
       !strcmp(name, "PlayerRoomExit") ||
       !strcmp(name, "RemotePlayerRoomEnter") ||
       !strcmp(name, "RemotePlayerRoomExit") ||
       !strcmp(name, "CreatureRoomEnter") ||
       !strcmp(name, "CreatureRoomExit") ||
       !strcmp(name, "ObjectRoomEnter") ||
       !strcmp(name, "ObjectRoomExit") ||
       !strcmp(name, "ObjRoomTransit"))
   {
      const sRoomMsg *room = static_cast<const sRoomMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(room->FromObjId));
      Append32(&guest->extension, static_cast<uint32_t>(room->ToObjId));
      Append32(&guest->extension, static_cast<uint32_t>(room->MoveObjId));
      Append32(&guest->extension, static_cast<uint32_t>(room->ObjType));
      Append32(&guest->extension,
               static_cast<uint32_t>(room->TransitionType));
   }
   else if (!strcmp(name, "Sim"))
   {
      const sSimMsg *sim = static_cast<const sSimMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(sim->starting));
   }
   else if (!strcmp(name, "Timer"))
   {
      const sScrTimerMsg *timer = static_cast<const sScrTimerMsg *>(message);
      AppendString(&guest->extension, &guest->extensionStrings, timer->name);
   }
   else if (!strcmp(name, "ReportMessage"))
   {
      const sNativeReportMessage *report =
         reinterpret_cast<const sNativeReportMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(report->warnLevel));
      Append32(&guest->extension, static_cast<uint32_t>(report->flags));
      Append32(&guest->extension, static_cast<uint32_t>(report->types));
      osm32::sMessage::sBufferPatch patch;
      patch.offset = static_cast<uint32_t>(guest->extension.size());
      // Report.cpp owns a 256 KiB output buffer for each ReportMessage.
      patch.capacity = kReportBufferCapacity;
      patch.destination = report->textBuffer;
      Append32(&guest->extension, 0);
      if (patch.destination)
         guest->extensionBuffers.push_back(patch);
   }
   else if (!strcmp(name, "FrobToolBegin") ||
            !strcmp(name, "FrobToolEnd") ||
            !strcmp(name, "FrobWorldBegin") ||
            !strcmp(name, "FrobWorldEnd") ||
            !strcmp(name, "FrobInvBegin") ||
            !strcmp(name, "FrobInvEnd"))
   {
      const sFrobMsg *frob = static_cast<const sFrobMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(frob->SrcObjId));
      Append32(&guest->extension, static_cast<uint32_t>(frob->DstObjId));
      Append32(&guest->extension, static_cast<uint32_t>(frob->Frobber));
      Append32(&guest->extension, static_cast<uint32_t>(frob->SrcLoc));
      Append32(&guest->extension, static_cast<uint32_t>(frob->DstLoc));
      AppendFloat(&guest->extension, frob->Sec);
      Append32(&guest->extension, static_cast<uint32_t>(frob->Abort));
   }
   else if (!strcmp(name, "Container"))
   {
      const sContainerScrMsg *container =
         static_cast<const sContainerScrMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(container->event));
      Append32(&guest->extension,
               static_cast<uint32_t>(container->containee));
   }
   else if (!strcmp(name, "Contained"))
   {
      const sContainedScrMsg *contained =
         static_cast<const sContainedScrMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(contained->event));
      Append32(&guest->extension,
               static_cast<uint32_t>(contained->container));
   }
   else if (!strcmp(name, "Combine"))
   {
      const sCombineScrMsg *combine =
         static_cast<const sCombineScrMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(combine->combiner));
   }
   else if (!strcmp(name, "Damage"))
   {
      const sDamageScrMsg *damage =
         static_cast<const sDamageScrMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(damage->kind));
      Append32(&guest->extension, static_cast<uint32_t>(damage->damage));
      Append32(&guest->extension, static_cast<uint32_t>(damage->culprit));
   }
   else if (!strcmp(name, "Slain"))
   {
      const sSlayMsg *slain = static_cast<const sSlayMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(slain->culprit));
      Append32(&guest->extension, static_cast<uint32_t>(slain->kind));
   }
   else if (!strcmp(name, "TweqComplete"))
   {
      const sNativeTweqMessage *tweq =
         reinterpret_cast<const sNativeTweqMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(tweq->type));
      Append32(&guest->extension, static_cast<uint32_t>(tweq->operation));
      Append32(&guest->extension, static_cast<uint32_t>(tweq->direction));
   }
   else if (!strcmp(name, "DoorOpen") || !strcmp(name, "DoorClose") ||
            !strcmp(name, "DoorOpening") ||
            !strcmp(name, "DoorClosing") || !strcmp(name, "DoorHalt"))
   {
      const sNativeDoorMessage *door =
         reinterpret_cast<const sNativeDoorMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(door->actionType));
      Append32(&guest->extension,
               static_cast<uint32_t>(door->previousActionType));
      Append32(&guest->extension, static_cast<uint32_t>(door->isProxy));
   }
   else if (!strcmp(name, "SoundDone") || !strcmp(name, "SchemaDone"))
   {
      if (!strcmp(name, "SoundDone"))
      {
         const sSoundDoneMsg *sound =
            static_cast<const sSoundDoneMsg *>(message);
         AppendFloat(&guest->extension, sound->coordinates.x);
         AppendFloat(&guest->extension, sound->coordinates.y);
         AppendFloat(&guest->extension, sound->coordinates.z);
         Append32(&guest->extension,
                  static_cast<uint32_t>(sound->targetObject));
         AppendString(&guest->extension, &guest->extensionStrings,
                      sound->name);
      }
      else
      {
         const sSchemaDoneMsg *schema =
            static_cast<const sSchemaDoneMsg *>(message);
         AppendFloat(&guest->extension, schema->coordinates.x);
         AppendFloat(&guest->extension, schema->coordinates.y);
         AppendFloat(&guest->extension, schema->coordinates.z);
         Append32(&guest->extension,
                  static_cast<uint32_t>(schema->targetObject));
         AppendString(&guest->extension, &guest->extensionStrings,
                      schema->name);
      }
   }
   else if (!strcmp(name, "MotionStart") ||
            !strcmp(name, "MotionEnd") ||
            !strcmp(name, "MotionFlagReached"))
   {
      const sBodyMsg *body = static_cast<const sBodyMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(body->ActionType));
      AppendString(&guest->extension, &guest->extensionStrings,
                   body->MotionName);
      Append32(&guest->extension, static_cast<uint32_t>(body->FlagValue));
   }
   else if (!strcmp(name, "PhysCollision") ||
            !strcmp(name, "PhysContactCreate") ||
            !strcmp(name, "PhysContactDestroy") ||
            !strcmp(name, "PhysEnter") || !strcmp(name, "PhysExit") ||
            !strcmp(name, "PhysFellAsleep") ||
            !strcmp(name, "PhysWokeUp") ||
            !strcmp(name, "PhysMadePhysical") ||
            !strcmp(name, "PhysMadeNonPhysical"))
   {
      // A guest sPhysMsg inherits sPhysMsgBase after the 56-byte x86
      // sScrMsg base.  Omitting this extension made collision scripts read
      // whichever guest allocation followed the message as collObj/transObj.
      const sNativePhysMessage *physics =
         reinterpret_cast<const sNativePhysMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(physics->Submod));
      Append32(&guest->extension, static_cast<uint32_t>(physics->collType));
      Append32(&guest->extension, static_cast<uint32_t>(physics->collObj));
      Append32(&guest->extension, static_cast<uint32_t>(physics->collSubmod));
      AppendFloat(&guest->extension, physics->collMomentum);
      AppendFloat(&guest->extension, physics->collNormal.x);
      AppendFloat(&guest->extension, physics->collNormal.y);
      AppendFloat(&guest->extension, physics->collNormal.z);
      AppendFloat(&guest->extension, physics->collPt.x);
      AppendFloat(&guest->extension, physics->collPt.y);
      AppendFloat(&guest->extension, physics->collPt.z);
      Append32(&guest->extension, static_cast<uint32_t>(physics->contactType));
      Append32(&guest->extension, static_cast<uint32_t>(physics->contactObj));
      Append32(&guest->extension,
               static_cast<uint32_t>(physics->contactSubmod));
      Append32(&guest->extension, static_cast<uint32_t>(physics->transObj));
      Append32(&guest->extension,
               static_cast<uint32_t>(physics->transSubmod));
   }
   else if (!strcmp(name, "StartWindup") ||
            !strcmp(name, "StartAttack") || !strcmp(name, "EndAttack"))
   {
      const sAttackMsg *attack = static_cast<const sAttackMsg *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(attack->weapon));
   }
   else if (!strcmp(name, "MediumTransition"))
   {
      const sNativeMediumTransitionMessage *medium =
         reinterpret_cast<const sNativeMediumTransitionMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(medium->fromType));
      Append32(&guest->extension, static_cast<uint32_t>(medium->toType));
   }
   else if (strlen(name) >= 8 &&
            !strcmp(name + strlen(name) - 8, "Stimulus"))
   {
      const sStimMsg *stimulus = static_cast<const sStimMsg *>(message);
      Append32(&guest->extension,
               static_cast<uint32_t>(static_cast<int>(stimulus->stimulus)));
      AppendFloat(&guest->extension, stimulus->intensity);
      Append32(&guest->extension, static_cast<uint32_t>(stimulus->sensor));
      Append32(&guest->extension, static_cast<uint32_t>(stimulus->source));
   }
   else if (!strcmp(name, "QuestChange"))
   {
      const sNativeQuestMessage *quest =
         reinterpret_cast<const sNativeQuestMessage *>(message);
      Append32(&guest->extension, 0);
      Append32(&guest->extension,
               static_cast<uint32_t>(quest->oldValue));
      Append32(&guest->extension,
               static_cast<uint32_t>(quest->newValue));
      osm32::sMessage::sStringPatch patch;
      patch.offset = 0;
      patch.value = quest->name ? quest->name : "";
      guest->extensionStrings.push_back(patch);
   }
   else if (!strcmp(name, "DarkGameModeChange"))
   {
      const sNativeDarkGameModeMessage *mode =
         reinterpret_cast<const sNativeDarkGameModeMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(mode->resuming));
      Append32(&guest->extension, static_cast<uint32_t>(mode->suspending));
   }
   else if (!strcmp(name, "SignalAI"))
   {
      const sNativeAISignalMessage *signal =
         reinterpret_cast<const sNativeAISignalMessage *>(message);
      AppendString(&guest->extension, &guest->extensionStrings,
                   signal->signal);
   }
   else if (!strcmp(name, "PatrolPoint"))
   {
      const sNativeAIPatrolPointMessage *patrol =
         reinterpret_cast<const sNativeAIPatrolPointMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(patrol->patrolObj));
   }
   else if (!strcmp(name, "Alertness") || !strcmp(name, "HighAlert"))
   {
      if (!strcmp(name, "Alertness"))
      {
         const sNativeAIAlertMessage *alert =
            reinterpret_cast<const sNativeAIAlertMessage *>(message);
         Append32(&guest->extension, static_cast<uint32_t>(alert->level));
         Append32(&guest->extension, static_cast<uint32_t>(alert->oldLevel));
      }
      else
      {
         const sNativeAIAlertMessage *alert =
            reinterpret_cast<const sNativeAIAlertMessage *>(message);
         Append32(&guest->extension, static_cast<uint32_t>(alert->level));
         Append32(&guest->extension, static_cast<uint32_t>(alert->oldLevel));
      }
   }
   else if (!strcmp(name, "AIModeChange"))
   {
      const sNativeAIModeChangeMessage *mode =
         reinterpret_cast<const sNativeAIModeChangeMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(mode->mode));
      Append32(&guest->extension,
               static_cast<uint32_t>(mode->previousMode));
   }
   else if (!strcmp(name, "ObjActResult"))
   {
      const sNativeAIObjActResultMessage *result =
         reinterpret_cast<const sNativeAIObjActResultMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(result->action));
      Append32(&guest->extension, static_cast<uint32_t>(result->result));
      AppendValue(&guest->extension, &guest->extensionValues, result->data);
      Append32(&guest->extension, static_cast<uint32_t>(result->target));
   }
   else if (!strcmp(name, "Difficulty"))
   {
      const sNativeDifficultyMessage *difficulty =
         reinterpret_cast<const sNativeDifficultyMessage *>(message);
      Append32(&guest->extension,
               static_cast<uint32_t>(difficulty->difficulty));
   }
   else if (!strcmp(name, "PickStateChange"))
   {
      const sNativePickStateMessage *pick =
         reinterpret_cast<const sNativePickStateMessage *>(message);
      Append32(&guest->extension,
               static_cast<uint32_t>(pick->previousState));
      Append32(&guest->extension,
               static_cast<uint32_t>(pick->currentState));
   }
   else if (!strcmp(name, "MovingTerrainWaypoint"))
   {
      const sNativeMovingTerrainMessage *terrain =
         reinterpret_cast<const sNativeMovingTerrainMessage *>(message);
      Append32(&guest->extension, static_cast<uint32_t>(terrain->waypoint));
   }
   else if (!strcmp(name, "WaypointReached"))
   {
      const sNativeWaypointMessage *waypoint =
         reinterpret_cast<const sNativeWaypointMessage *>(message);
      Append32(&guest->extension,
               static_cast<uint32_t>(waypoint->moving_terrain));
   }
}

bool ReadGuestString(osm32::cMemory *memory, uint32_t address,
                     std::string *value, std::string *error)
{
   value->clear();
   for (unsigned i = 0; i < 0x00100000; ++i)
   {
      uint8_t character;
      if (!memory->Read8(address + i, &character, error))
         return false;
      if (!character)
         return true;
      value->push_back(static_cast<char>(character));
   }
   if (error)
      *error = "unterminated guest service string";
   return false;
}

bool ReadGuestParm(osm32::cMemory *memory, uint32_t address,
                   cMultiParm *value, std::string *error)
{
   if (!address)
   {
      value->Clear();
      return true;
   }
   uint32_t data, type;
   if (!memory->Read32(address, &data, error) ||
       !memory->Read32(address + 4, &type, error))
      return false;
   switch (type)
   {
      case kMT_Undef:
         value->Clear();
         return true;
      case kMT_Int:
         *value = static_cast<int32_t>(data);
         return true;
      case kMT_Float:
      {
         float number;
         memcpy(&number, &data, sizeof(number));
         *value = number;
         return true;
      }
      case kMT_String:
      {
         std::string text;
         if (!ReadGuestString(memory, data, &text, error))
            return false;
         *value = text.c_str();
         return true;
      }
      case kMT_Vector:
      {
         uint32_t bits[3];
         mxs_vector vector;
         if (!memory->Read32(data, &bits[0], error) ||
             !memory->Read32(data + 4, &bits[1], error) ||
             !memory->Read32(data + 8, &bits[2], error))
            return false;
         memcpy(&vector, bits, sizeof(bits));
         *value = vector;
         return true;
      }
      case kMT_Obj:
         value->Clear();
         value->o = static_cast<ObjID>(data);
         value->type = kMT_Obj;
         return true;
      default:
         if (error)
         {
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "invalid guest service multiparm type %u at 0x%08x "
                     "(data 0x%08x)", type, address, data);
            *error = detail;
         }
         return false;
   }
}

class cOsm32Environment : public osm32::cEnvironment
{
public:
   cOsm32Environment(IScriptMan *scriptManager, osm32::eGuestAbi guestAbi)
    : m_scriptManager(scriptManager),
      m_guestAbi(guestAbi),
      m_linkQueryVtable(0),
      m_linkQuerySerial(0),
      m_runtime(0)
   {
   }

   virtual ~cOsm32Environment()
   {
      DropListeners();
      for (std::map<uint32_t, sLinkQueryProxy>::iterator i =
              m_linkQueries.begin(); i != m_linkQueries.end(); ++i)
         while (i->second.references)
         {
            --i->second.references;
            i->second.query->Release();
         }
   }

   void SetRuntime(osm32::cModule *runtime) { m_runtime = runtime; }

   // The guest's listeners (the sim's messages, a property's, a relation's,
   // the archetype hierarchy's): each a native listener of the engine's that
   // calls the guest's function back with the message copied to it.
   enum eListenerKind { kListenSim, kListenProperty, kListenRelation, kListenHierarchy };
   struct sGuestListener
   {
      cOsm32Environment *owner;   // 0: its module gone (the engine may still call)
      eListenerKind kind;
      uint32_t func, data, desc, token;
      IUnknown *native;           // the property or relation listened to (AddRef'd)
      PropListenerHandle handle;
      GUID id;
      sDispatchListenerDesc hostDesc;
   };
   std::vector<sGuestListener *> m_listeners;
   uint32_t m_nextListenerToken = 1;

   void DropListeners()
   {
      for (sGuestListener *l : m_listeners)
      {
         if (!l->owner)
            continue;
         if (l->kind == kListenProperty && l->native)
            static_cast<IProperty *>(l->native)->Unlisten(l->handle);
         if (l->kind == kListenSim)
            if (ISimManager *sim = AppGetObj(ISimManager))
            {
               sim->Unlisten(&l->id);
               sim->Release();
            }
         // (relations and the hierarchy cannot be unlistened: the engine
         // keeps calling, and the listener, ownerless, does nothing -- kept)
         l->owner = 0;
      }
      m_listeners.clear();
   }

   // the guest's function called with its message (bytes copied to the
   // guest, freed after) and its data
   void CallListener(sGuestListener *l, const void *message, uint32_t size, uint32_t second)
   {
      if (!l->owner || !m_runtime)
         return;
      std::string error;
      const uint32_t guestMessage = AllocGuest(size, &error);
      if (!guestMessage)
         return;
      osm32::cMemory &memory = m_runtime->Memory();
      bool ok = memory.Load(guestMessage, message, size, &error);
      uint32_t ignored;
      const uint32_t args[2] = {guestMessage, second};
      ok = ok && m_runtime->CallFunction(l->func, args, 2, &ignored, &error);
      FreeGuest(guestMessage, 0);
      if (!ok)
      {
         static unsigned s_told = 0;
         if (s_told++ < 20)
            fprintf(stderr, "OSM32 '%s': a script library's listener failed: %s\n",
                    m_runtime->Name().c_str(), error.c_str());
      }
   }

   static void SimListener(const sDispatchMsg *msg, const sDispatchListenerDesc *desc)
   {
      sGuestListener *l = static_cast<sGuestListener *>(desc->data);
      if (!l || !l->owner || !msg) return;
      const uint32_t guest[2] = {static_cast<uint32_t>(msg->kind), 0};
      l->owner->CallListener(l, guest, sizeof(guest), l->desc);
   }

   static void LGAPI RelationListener(sRelationListenMsg *msg, RelationListenerData data)
   {
      sGuestListener *l = static_cast<sGuestListener *>(data);
      if (!l || !l->owner || !msg) return;
      const uint32_t guest[5] = {static_cast<uint32_t>(msg->type), static_cast<uint32_t>(msg->id),
                                 static_cast<uint32_t>(msg->link.source), static_cast<uint32_t>(msg->link.dest),
                                 static_cast<uint32_t>(static_cast<int32_t>(msg->link.flavor))};
      l->owner->CallListener(l, guest, sizeof(guest), l->data);
   }

   static void LGAPI HierarchyListener(const sHierarchyMsg *msg, HierarchyListenerData data)
   {
      sGuestListener *l = static_cast<sGuestListener *>(data);
      if (!l || !l->owner || !msg) return;
      const uint32_t guest[3] = {static_cast<uint32_t>(msg->kind), static_cast<uint32_t>(msg->obj),
                                 static_cast<uint32_t>(msg->donor)};
      l->owner->CallListener(l, guest, sizeof(guest), l->data);
   }

   static void LGAPI PropertyListener(sPropertyListenMsg *msg, PropListenerData data)
   {
      sGuestListener *l = reinterpret_cast<sGuestListener *>(data);
      if (!l || !l->owner || !msg) return;
      l->owner->DeliverProperty(l, msg);
   }

   void DeliverProperty(sGuestListener *l, sPropertyListenMsg *msg)
   {
      // the new value: an int, BOOL or float as it is; a string or a
      // structure by pointer, copied to the guest (as Get gives it)
      IProperty *prop = static_cast<IProperty *>(l->native);
      uint32_t value = static_cast<uint32_t>(msg->value.intval);
      bool scalar = false;
      IUnknown *probe = 0;
      static const GUID *const kScalars[] = {&IID_IIntProperty, &IID_IBoolProperty, &IID_IFloatProperty};
      for (const GUID *iid : kScalars)
         if (!scalar && SUCCEEDED(prop->QueryInterface(*iid, reinterpret_cast<void **>(&probe))) && probe)
         {
            scalar = true;
            probe->Release();
         }
      if (!scalar)
      {
         value = 0;
         std::string error;
         osm32::cMemory &memory = m_runtime->Memory();
         const int64_t key = (static_cast<int64_t>(0x7d000000) | static_cast<uint16_t>(msg->property)) << 32 |
                             static_cast<uint32_t>(msg->obj);
         if (msg->value.ptrval)
         {
            IStringProperty *text = 0;
            if (SUCCEEDED(prop->QueryInterface(IID_IStringProperty, reinterpret_cast<void **>(&text))) && text)
            {
               text->Release();
               GuestText(key, static_cast<const char *>(msg->value.ptrval), &memory, &value, &error);
            }
            else if (const sPropertyTypeDesc *type = prop->DescribeType())
               if (type->size && type->size <= 0x10000)
                  GuestBytes(key, msg->value.ptrval, static_cast<uint32_t>(type->size), &memory, &value, &error);
         }
      }
      const uint32_t guest[5] = {static_cast<uint32_t>(msg->type), static_cast<uint32_t>(msg->property),
                                 static_cast<uint32_t>(msg->obj), value, static_cast<uint32_t>(msg->donor)};
      CallListener(l, guest, sizeof(guest), l->data);
   }

   sGuestListener *NewListener(eListenerKind kind, uint32_t func, uint32_t data)
   {
      sGuestListener *l = new sGuestListener();
      l->owner = this;
      l->kind = kind;
      l->func = func;
      l->data = data;
      l->desc = 0;
      l->token = m_nextListenerToken++;
      l->native = 0;
      l->handle = 0;
      m_listeners.push_back(l);
      return l;
   }

   osm32::eGuestAbi GuestAbi() const { return m_guestAbi; }

   uint64_t LinkQueryCheckpoint() const
   {
      return m_linkQuerySerial;
   }

   // Released queries are dropped from the table (and their guest memory
   // freed) so that lookups don't slow down over a long session.
   void DropLinkQuery(uint32_t object)
   {
      std::map<uint32_t, sLinkQueryProxy>::iterator i =
         m_linkQueries.find(object);
      if (i == m_linkQueries.end())
         return;
      while (i->second.references)
      {
         --i->second.references;
         i->second.query->Release();
      }
      if (i->second.data) FreeGuest(i->second.data, 0);
      FreeGuest(i->first, 0);
      m_linkQueries.erase(i);
      --g_Osm32LinkQueries;
   }

   void ReleaseLinkQueriesCreatedSince(uint64_t checkpoint)
   {
      for (std::map<uint32_t, sLinkQueryProxy>::iterator i =
              m_linkQueries.begin(); i != m_linkQueries.end();)
      {
         std::map<uint32_t, sLinkQueryProxy>::iterator next = i;
         ++next;
         if (i->second.serial >= checkpoint)
            DropLinkQuery(i->first);
         i = next;
      }
   }

   virtual bool Handles(uint32_t address) const
   {
      return (address >= kLinkQueryTrap &&
              address < kLinkQueryTrap + 9 * kLinkQueryTrapStride &&
              (address - kLinkQueryTrap) % kLinkQueryTrapStride == 0) ||
             cEnvironment::Handles(address);
   }

   virtual bool Invoke(uint32_t address, osm32::sCpuState *state,
                       osm32::cMemory *memory, std::string *error)
   {
      if (address >= kLinkQueryTrap &&
          address < kLinkQueryTrap + 9 * kLinkQueryTrapStride)
         return InvokeLinkQuery(
            (address - kLinkQueryTrap) / kLinkQueryTrapStride,
            state, memory, error);
      return cEnvironment::Invoke(address, state, memory, error);
   }

protected:
   virtual bool InvokeScriptManager(unsigned slot, osm32::sCpuState *state,
                                    osm32::cMemory *memory,
                                    std::string *error)
   {
      const bool invoked = InvokeScriptManagerImpl(slot, state, memory,
                                                   error);
      if (!invoked && error &&
          error->find("IScriptMan slot ") == std::string::npos)
      {
         char context[48];
         snprintf(context, sizeof(context), "IScriptMan slot %u: ", slot);
         *error = context + *error;
      }
      return invoked;
   }

   bool InvokeScriptManagerImpl(unsigned slot, osm32::sCpuState *state,
                                osm32::cMemory *memory,
                                std::string *error)
   {
      if (slot == 22)
      {
         uint32_t timer;
         if (!Argument(*state, *memory, 1, &timer, error)) return false;
         m_scriptManager->KillTimedMessage(
            static_cast<tScrTimer>(static_cast<uintptr_t>(timer)));
         return Return(state, memory, 8, 0, error);
      }
      if (slot == 26 || slot == 27)
      {
         const bool hasResult = slot == 26;
         const bool hasFlags = osm32::GuestPostMessageHasFlags(m_guestAbi);
         const unsigned offset = hasResult ? 1 : 0;
         uint32_t resultAddress = 0, from, to, parmAddresses[3], flags = 0;
         std::string message;
         if ((hasResult && !Argument(*state, *memory, 1, &resultAddress,
                                     error)) ||
             !Argument(*state, *memory, 1 + offset, &from, error) ||
             !Argument(*state, *memory, 2 + offset, &to, error) ||
             !StringArgument(*state, memory, 3 + offset, &message, error))
            return false;
         cMultiParm parms[3];
         for (unsigned i = 0; i < 3; ++i)
         {
            if (!Argument(*state, *memory, 4 + offset + i,
                          &parmAddresses[i], error) ||
                !ReadGuestParm(memory, parmAddresses[i], &parms[i], error))
               return false;
         }
         if (hasResult)
         {
            cMultiParm result = m_scriptManager->SendMessage2(
               static_cast<ObjID>(from), static_cast<ObjID>(to),
               message.c_str(), parms[0], parms[1], parms[2]);
            return WriteGuestParm(memory, resultAddress, result, error) &&
                   Return(state, memory, 32, resultAddress, error);
         }
         // Stock Thief 1 modules use the pre-flags PostMessage2 ABI.  Its
         // call has six explicit parameters; treating the next stack local
         // as flags and popping it shifts ESP, corrupting every destructor
         // that follows the post.
         if (hasFlags && !Argument(*state, *memory, 7, &flags, error))
            return false;
         m_scriptManager->PostMessage2(
            static_cast<ObjID>(from), static_cast<ObjID>(to), message.c_str(),
            parms[0], parms[1], parms[2], static_cast<ulong>(flags));
         return Return(state, memory, hasFlags ? 32 : 28, 0, error);
      }
      if (slot == 28)
      {
         uint32_t to, time, kind, parmAddress;
         std::string name;
         if (!Argument(*state, *memory, 1, &to, error) ||
             !StringArgument(*state, memory, 2, &name, error) ||
             !Argument(*state, *memory, 3, &time, error) ||
             !Argument(*state, *memory, 4, &kind, error) ||
             !Argument(*state, *memory, 5, &parmAddress, error))
            return false;
         cMultiParm data;
         if (!ReadGuestParm(memory, parmAddress, &data, error)) return false;
         const tScrTimer result = m_scriptManager->SetTimedMessage2(
            static_cast<ObjID>(to), name.c_str(), static_cast<ulong>(time),
            static_cast<eScrTimedMsgKind>(kind), data);
         return Return(state, memory, 24, static_cast<uint32_t>(result), error);
      }
      if (slot < 29 || slot > 32)
         return cEnvironment::InvokeScriptManager(slot, state, memory, error);

      uint32_t tagAddress, parmAddress = 0;
      sScrDatumTag tag;
      std::string className, datumName;
      if (!Argument(*state, *memory, 1, &tagAddress, error) ||
          !ReadGuestTag(memory, tagAddress, &tag, &className, &datumName,
                        error))
         return false;

      if (slot == 29)
         return Return(state, memory, 8,
                       m_scriptManager->IsScriptDataSet(&tag), error);

      if (!Argument(*state, *memory, 2, &parmAddress, error))
         return false;
      cMultiParm parm;
      HRESULT result;
      if (slot == 30)
      {
         result = m_scriptManager->GetScriptData(&tag, &parm);
         if (SUCCEEDED(result) && !WriteGuestParm(memory, parmAddress, parm,
                                                  error))
            return false;
      }
      else if (slot == 31)
      {
         if (!ReadGuestParm(memory, parmAddress, &parm, error)) return false;
         result = m_scriptManager->SetScriptData(&tag, &parm);
      }
      else
      {
         result = m_scriptManager->ClearScriptData(&tag, &parm);
         if (SUCCEEDED(result) && !WriteGuestParm(memory, parmAddress, parm,
                                                  error))
            return false;
      }
      return Return(state, memory, 12, result, error);
   }

   // The engine's own interfaces a guest asked the script manager for
   // (Telliamed's and NamelessVoice's script libraries: the trait manager,
   // for what inherits from what), and the object queries they return.
   virtual bool EngineInterfaceAvailable(uint32_t lgNumber)
   {
      return lgNumber == 0x8f ||    // ITraitManager
             lgNumber == 0x149 ||   // ISimManager
             lgNumber == 0x78 ||    // IPropertyManager
             lgNumber == 0x82;      // ILinkManager
   }

   virtual void ReleaseProxyNative(uint16_t id, void *native)
   {
      if (id == (kEngineObject | 1) && native)
         static_cast<IObjectQuery *>(native)->Release();
      if (id == (kEngineObject | 2) && native)
         static_cast<IProperty *>(native)->Release();
      if (id == (kEngineObject | 3) && native)
         static_cast<IRelation *>(native)->Release();
   }

   // a guest copy of native bytes (a link's data, a description), kept per
   // key and reused while it fits
   bool GuestBytes(int64_t key, const void *bytes, uint32_t size, osm32::cMemory *memory,
                   uint32_t *address, std::string *error)
   {
      sGuestText &t = m_guestTexts[key];
      if (t.capacity < size)
      {
         if (t.address) FreeGuest(t.address, 0);
         t.address = AllocGuest(size, error);
         t.capacity = t.address ? size : 0;
         if (!t.address) return false;
      }
      for (uint32_t i = 0; i < size; ++i)
         if (!memory->Write8(t.address + i, static_cast<const uint8_t *>(bytes)[i], error)) return false;
      *address = t.address;
      return true;
   }

   static uint32_t RelationDataSize(IRelation *rel)
   {
      const sRelationDataDesc *desc = rel ? rel->DescribeData() : 0;
      return desc && desc->size <= 0x10000 ? static_cast<uint32_t>(desc->size) : 0;
   }

   bool ReadGuestBytes(osm32::cMemory *memory, uint32_t address, uint32_t size,
                       std::vector<uint8_t> *bytes, std::string *error)
   {
      bytes->assign(size, 0);
      for (uint32_t i = 0; i < size; ++i)
         if (!memory->Read8(address + i, &(*bytes)[i], error)) return false;
      return true;
   }

   bool WriteGuestLink(osm32::cMemory *memory, uint32_t address, const sLink &link, std::string *error)
   {
      return memory->Write32(address, static_cast<uint32_t>(link.source), error) &&
             memory->Write32(address + 4, static_cast<uint32_t>(link.dest), error) &&
             memory->Write32(address + 8, static_cast<uint32_t>(static_cast<int32_t>(link.flavor)), error);
   }

   // ILinkManager and IRelation (tnhScript: relations by name, their links,
   // their data)
   bool InvokeLinkInterface(bool manager, IUnknown *native, unsigned slot, osm32::sCpuState *state,
                            osm32::cMemory *memory, std::string *error)
   {
      uint32_t arg[5] = {0, 0, 0, 0, 0};
      for (unsigned i = 0; i < 5; ++i)
         Argument(*state, *memory, i + 1, &arg[i], 0);
      auto obj = [](uint32_t v) { return static_cast<ObjID>(static_cast<int32_t>(v)); };
      ILinkManager *links = manager ? static_cast<ILinkManager *>(native) : 0;
      IRelation *rel = manager ? 0 : static_cast<IRelation *>(native);
      // (the manager's methods take a relation where the relation's do not:
      // its arguments from the relation's on, one later)
      const unsigned shift = manager ? 1 : 0;
      auto relationOf = [&](LinkID id) -> IRelation * {
         if (rel) { rel->AddRef(); return rel; }
         return links->GetRelation(LINKID_RELATION(id));
      };
      if (manager && (slot == 3 || slot == 4))
      {
         IRelation *found = 0;
         if (slot == 3)
            found = links->GetRelation(static_cast<RelationID>(static_cast<int32_t>(arg[0])));
         else
         {
            std::string name;
            if (!ReadGuestString(memory, arg[0], &name, error)) return false;
            found = links->GetRelationNamed(name.c_str());
         }
         uint32_t result = 0;
         if (found && !(result = CreateInterfaceProxy(kEngineObject | 3, found, error)))
         {
            found->Release();
            return false;
         }
         return Return(state, memory, 8, result, error);
      }
      if (!manager)
      {
         if (slot == 3 || slot == 6)
         {
            // Describe, DescribeData: copied out (32-bit fields; no data ops)
            uint8_t bytes[48] = {};
            uint32_t size = 0;
            if (slot == 3)
            {
               const sRelationDesc *d = rel->Describe();
               if (d)
               {
                  memcpy(bytes, d->name, 32);
                  const uint32_t f[4] = {static_cast<uint32_t>(d->flags), static_cast<uint32_t>(d->version),
                                         static_cast<uint32_t>(d->oldest), static_cast<uint32_t>(d->read_version)};
                  memcpy(bytes + 32, f, 16);
                  size = 48;
               }
            }
            else if (const sRelationDataDesc *d = rel->DescribeData())
            {
               memcpy(bytes, d->type, 32);
               const uint32_t f[3] = {static_cast<uint32_t>(d->size), static_cast<uint32_t>(d->flags), 0};
               memcpy(bytes + 32, f, 12);
               size = 44;
            }
            uint32_t address = 0;
            if (size && !GuestBytes((static_cast<int64_t>(0x7e000000 + slot) << 32) | static_cast<uint16_t>(rel->GetID()),
                                    bytes, size, memory, &address, error))
               return false;
            return Return(state, memory, 4, address, error);
         }
         if (slot == 4) return Return(state, memory, 4, static_cast<uint32_t>(static_cast<int32_t>(rel->GetID())), error);
         if (slot == 16)
         {
            // Listen(interests, func, data)
            if (arg[1])
            {
               sGuestListener *l = NewListener(kListenRelation, arg[1], arg[2]);
               rel->AddRef();
               l->native = rel;
               return Return(state, memory, 16,
                             static_cast<uint32_t>(rel->Listen(static_cast<RelationListenMsgSet>(arg[0]),
                                                               RelationListener, l)), error);
            }
            return Return(state, memory, 16, 0, error);
         }
         if (slot == 7)
         {
            IRelation *inverse = rel->Inverse();
            uint32_t result = 0;
            if (inverse && !(result = CreateInterfaceProxy(kEngineObject | 3, inverse, error)))
            {
               inverse->Release();
               return false;
            }
            return Return(state, memory, 4, result, error);
         }
      }
      // the shared ones: Add, AddFull, Remove, Get, SetData, GetData, Query,
      // GetSingleLink, AnyLinks
      const unsigned base = slot;   // (the same slot numbers, as it happens, but for the offset arguments)
      const RelationID relId = manager ? static_cast<RelationID>(static_cast<int32_t>(arg[2])) : 0;
      if ((manager && base == 9) || (!manager && base == 8))
      {
         const LinkID id = manager ? links->Add(obj(arg[0]), obj(arg[1]), relId) : rel->Add(obj(arg[0]), obj(arg[1]));
         return Return(state, memory, manager ? 16 : 12, static_cast<uint32_t>(id), error);
      }
      if ((manager && base == 10) || (!manager && base == 9))
      {
         IRelation *r = manager ? links->GetRelation(relId) : (rel->AddRef(), rel);
         const uint32_t size = RelationDataSize(r);
         std::vector<uint8_t> data;
         const uint32_t dataAddress = arg[2 + shift];
         if (dataAddress && size && !ReadGuestBytes(memory, dataAddress, size, &data, error))
         {
            if (r) r->Release();
            return false;
         }
         void *pdata = data.empty() ? 0 : data.data();
         const LinkID id = manager ? links->AddFull(obj(arg[0]), obj(arg[1]), relId, pdata)
                                   : rel->AddFull(obj(arg[0]), obj(arg[1]), pdata);
         if (r) r->Release();
         return Return(state, memory, manager ? 20 : 16, static_cast<uint32_t>(id), error);
      }
      if ((manager && base == 11) || (!manager && base == 10))
         return Return(state, memory, 8,
                       static_cast<uint32_t>(manager ? links->Remove(static_cast<LinkID>(arg[0]))
                                                     : rel->Remove(static_cast<LinkID>(arg[0]))), error);
      if ((manager && base == 12) || (!manager && base == 11))
      {
         sLink link = {};
         const BOOL got = manager ? links->Get(static_cast<LinkID>(arg[0]), &link)
                                  : rel->Get(static_cast<LinkID>(arg[0]), &link);
         return (!got || !arg[1] || WriteGuestLink(memory, arg[1], link, error)) &&
                Return(state, memory, 12, got ? 1 : 0, error);
      }
      if ((manager && base == 13) || (!manager && base == 12))
      {
         IRelation *r = relationOf(static_cast<LinkID>(arg[0]));
         const uint32_t size = RelationDataSize(r);
         if (r) r->Release();
         std::vector<uint8_t> data;
         if (arg[1] && size && !ReadGuestBytes(memory, arg[1], size, &data, error)) return false;
         void *pdata = data.empty() ? 0 : data.data();
         return Return(state, memory, 12,
                       static_cast<uint32_t>(manager ? links->SetData(static_cast<LinkID>(arg[0]), pdata)
                                                     : rel->SetData(static_cast<LinkID>(arg[0]), pdata)), error);
      }
      if ((manager && base == 14) || (!manager && base == 13))
      {
         const LinkID id = static_cast<LinkID>(arg[0]);
         void *data = manager ? links->GetData(id) : rel->GetData(id);
         IRelation *r = relationOf(id);
         const uint32_t size = RelationDataSize(r);
         if (r) r->Release();
         uint32_t address = 0;
         if (data && size && !GuestBytes((static_cast<int64_t>(0x7f000000) << 32) | static_cast<uint32_t>(id),
                                         data, size, memory, &address, error))
            return false;
         return Return(state, memory, 8, address, error);
      }
      if ((manager && base == 15) || (!manager && base == 14))
      {
         ILinkQuery *query = manager ? links->Query(obj(arg[0]), obj(arg[1]), relId)
                                     : rel->Query(obj(arg[0]), obj(arg[1]));
         uint32_t result = 0;
         if (query)
         {
            result = CreateLinkQuery(query, memory, error);
            query->Release();
            if (!result) return false;
         }
         return Return(state, memory, manager ? 16 : 12, result, error);
      }
      if ((manager && base == 17) || (!manager && base == 15 + 2))
      {
         const LinkID id = manager ? links->GetSingleLink(static_cast<RelationID>(static_cast<int32_t>(arg[0])),
                                                          obj(arg[1]), obj(arg[2]))
                                   : rel->GetSingleLink(obj(arg[0]), obj(arg[1]));
         return Return(state, memory, manager ? 16 : 12, static_cast<uint32_t>(id), error);
      }
      if ((manager && base == 18) || (!manager && base == 18))
      {
         const BOOL any = manager ? links->AnyLinks(static_cast<RelationID>(static_cast<int32_t>(arg[0])),
                                                    obj(arg[1]), obj(arg[2]))
                                  : rel->AnyLinks(obj(arg[0]), obj(arg[1]));
         return Return(state, memory, manager ? 16 : 12, any ? 1 : 0, error);
      }
      char message[96];
      snprintf(message, sizeof(message), "unsupported %s method %u", manager ? "link manager" : "relation", slot);
      if (error) *error = message;
      return false;
   }

   // a string property's text, copied out to the guest (one buffer an
   // object, reused while the text fits)
   struct sGuestText { uint32_t address; uint32_t capacity; };
   std::map<int64_t, sGuestText> m_guestTexts;

   bool GuestText(int64_t key, const char *text, osm32::cMemory *memory, uint32_t *address,
                  std::string *error)
   {
      const uint32_t length = static_cast<uint32_t>(strlen(text ? text : ""));
      sGuestText &t = m_guestTexts[key];
      if (t.capacity < length + 1)
      {
         if (t.address)
            FreeGuest(t.address, 0);
         t.capacity = length + 1 < 64 ? 64 : length + 1;
         t.address = AllocGuest(t.capacity, error);
         if (!t.address)
         {
            t.capacity = 0;
            return false;
         }
      }
      for (uint32_t i = 0; i <= length; ++i)
         if (!memory->Write8(t.address + i, i < length ? static_cast<uint8_t>(text[i]) : 0, error))
            return false;
      *address = t.address;
      return true;
   }

   // A property's value through its typed interface (every one has the same
   // shape: Set, TouchValue, Get, GetSimple at 18-21): an int, a BOOL, a
   // float -- the value itself; a structure (a vector, any other) -- a
   // pointer to it, its bytes copied to the guest (Get) or from it (Set),
   // as large as the property's type says.
   bool InvokeTypedProperty(IProperty *prop, unsigned slot, ObjID obj, uint32_t value,
                            osm32::sCpuState *state, osm32::cMemory *memory, std::string *error)
   {
      const sPropertyTypeDesc *type = prop->DescribeType();
      const sPropertyDesc *desc = prop->Describe();
      const char *name = desc ? desc->name : "?";
      void **vtable = *reinterpret_cast<void ***>(prop);
      IUnknown *probe = 0;
      bool scalar = false;
      static const GUID *const kScalars[] = {&IID_IIntProperty, &IID_IBoolProperty, &IID_IFloatProperty};
      for (const GUID *iid : kScalars)
         if (!scalar && SUCCEEDED(prop->QueryInterface(*iid, reinterpret_cast<void **>(&probe))) && probe)
         {
            scalar = true;
            probe->Release();
         }
      const uint32_t size = type ? static_cast<uint32_t>(type->size) : 0;
      if (!scalar && (!size || size > 0x10000))
      {
         char message[160];
         snprintf(message, sizeof(message), "property %s: its type (%s, %u bytes) is not given to scripts here",
                  name, type ? type->type : "?", size);
         if (error) *error = message;
         return false;
      }
      if (slot == 18 || slot == 19)
      {
         uint32_t result;
         if (scalar)
            result = slot == 18 ? static_cast<uint32_t>(reinterpret_cast<HRESULT (LGAPI *)(void *, ObjID, uint32_t)>(
                                     vtable[slot])(prop, obj, value))
                                : (reinterpret_cast<BOOL (LGAPI *)(void *, ObjID, uint32_t)>(vtable[slot])(prop, obj, value) ? 1 : 0);
         else
         {
            std::vector<uint8_t> bytes(size);
            for (uint32_t i = 0; i < size; ++i)
               if (!memory->Read8(value + i, &bytes[i], error)) return false;
            result = slot == 18 ? static_cast<uint32_t>(reinterpret_cast<HRESULT (LGAPI *)(void *, ObjID, void *)>(
                                     vtable[slot])(prop, obj, bytes.data()))
                                : (reinterpret_cast<BOOL (LGAPI *)(void *, ObjID, void *)>(vtable[slot])(prop, obj, bytes.data()) ? 1 : 0);
         }
         return Return(state, memory, 12, result, error);
      }
      // Get, GetSimple
      if (scalar)
      {
         uint32_t got = 0;
         const BOOL found = reinterpret_cast<BOOL (LGAPI *)(void *, ObjID, uint32_t *)>(vtable[slot])(prop, obj, &got);
         return (!found || !value || memory->Write32(value, got, error)) &&
                Return(state, memory, 12, found ? 1 : 0, error);
      }
      void *data = 0;
      const BOOL found = reinterpret_cast<BOOL (LGAPI *)(void *, ObjID, void **)>(vtable[slot])(prop, obj, &data);
      if (!found || !data || !value)
         return Return(state, memory, 12, found ? 1 : 0, error);
      sGuestText &t = m_guestTexts[static_cast<int64_t>(prop->GetID()) << 32 | static_cast<uint32_t>(obj)];
      if (t.capacity < size)
      {
         if (t.address) FreeGuest(t.address, 0);
         t.address = AllocGuest(size, error);
         t.capacity = t.address ? size : 0;
         if (!t.address) return false;
      }
      const uint8_t *bytes = static_cast<const uint8_t *>(data);
      for (uint32_t i = 0; i < size; ++i)
         if (!memory->Write8(t.address + i, bytes[i], error)) return false;
      return memory->Write32(value, t.address, error) && Return(state, memory, 12, 1, error);
   }

   bool InvokeEngineInterface(uint16_t id, uint32_t guestObject, unsigned slot,
                              osm32::sCpuState *state, osm32::cMemory *memory,
                              std::string *error)
   {
      uint32_t a = 0, b = 0, c = 0;
      Argument(*state, *memory, 1, &a, 0);
      Argument(*state, *memory, 2, &b, 0);
      Argument(*state, *memory, 3, &c, 0);
      if (id == (kEngineObject | 1))
      {
         // IObjectQuery: Done, Object, Next
         IObjectQuery *query = static_cast<IObjectQuery *>(GetProxyNative(guestObject));
         if (!query)
         {
            if (error) *error = "object query proxy without its query";
            return false;
         }
         if (slot == 3) return Return(state, memory, 4, query->Done() ? 1 : 0, error);
         if (slot == 4) return Return(state, memory, 4, static_cast<uint32_t>(query->Object()), error);
         if (slot == 5) return Return(state, memory, 4, static_cast<uint32_t>(query->Next()), error);
      }
      else if (id == (kEngineInterface | 0x82))
      {
         ILinkManager *links = AppGetObj(ILinkManager);
         if (!links)
         {
            if (error) *error = "native link manager is unavailable";
            return false;
         }
         const bool ok = InvokeLinkInterface(true, links, slot, state, memory, error);
         links->Release();
         return ok;
      }
      else if (id == (kEngineObject | 3))
      {
         IRelation *rel = static_cast<IRelation *>(GetProxyNative(guestObject));
         if (!rel)
         {
            if (error) *error = "relation proxy without its relation";
            return false;
         }
         return InvokeLinkInterface(false, rel, slot, state, memory, error);
      }
      else if (id == (kEngineInterface | 0x149))
      {
         // ISimManager: a guest's listener for the sim's messages is taken
         // and never called (its library's caches are only ever fresh)
         if (slot == 3)
         {
            // Listen: the guest's sDispatchListenerDesc {pID, interests, func, data}
            uint32_t idAddress = 0, interests = 0, func = 0, data = 0;
            if (!memory->Read32(a, &idAddress, error) || !memory->Read32(a + 4, &interests, error) ||
                !memory->Read32(a + 8, &func, error) || !memory->Read32(a + 12, &data, error))
               return false;
            ISimManager *sim = AppGetObj(ISimManager);
            if (!sim || !func)
            {
               if (sim) sim->Release();
               return Return(state, memory, 8, 0, error);
            }
            sGuestListener *l = NewListener(kListenSim, func, data);
            uint32_t words[4] = {0, 0, 0, 0};
            for (unsigned i = 0; i < 4 && idAddress; ++i)
               if (!memory->Read32(idAddress + i * 4, &words[i], error)) { sim->Release(); return false; }
            memcpy(&l->id, words, sizeof(GUID));
            // The description the guest's function is told of, a copy of
            // its own: as the engine keeps one -- the guest's was on its
            // stack, gone by the time a message came (NVScript's and
            // tnhScript's listeners read garbage at the sim's end)
            l->desc = AllocGuest(32, error);
            if (!l->desc ||
                !memory->Write32(l->desc, l->desc + 16, error) || !memory->Write32(l->desc + 4, interests, error) ||
                !memory->Write32(l->desc + 8, func, error) || !memory->Write32(l->desc + 12, data, error))
            {
               sim->Release();
               return false;
            }
            for (unsigned i = 0; i < 4; ++i)
               if (!memory->Write32(l->desc + 16 + i * 4, words[i], error)) { sim->Release(); return false; }
            l->hostDesc.pID = &l->id;
            l->hostDesc.interests = interests;
            l->hostDesc.func = SimListener;
            l->hostDesc.data = l;
            const HRESULT hr = sim->Listen(&l->hostDesc);
            sim->Release();
            return Return(state, memory, 8, static_cast<uint32_t>(hr), error);
         }
         if (slot == 4)
         {
            // Unlisten, by the guest's id
            uint32_t words[4] = {0, 0, 0, 0};
            for (unsigned i = 0; i < 4 && a; ++i)
               if (!memory->Read32(a + i * 4, &words[i], error)) return false;
            for (size_t i = 0; i < m_listeners.size(); ++i)
               if (m_listeners[i]->kind == kListenSim && !memcmp(&m_listeners[i]->id, words, sizeof(GUID)))
               {
                  if (ISimManager *sim = AppGetObj(ISimManager))
                  {
                     sim->Unlisten(&m_listeners[i]->id);
                     sim->Release();
                  }
                  m_listeners[i]->owner = 0;
                  m_listeners.erase(m_listeners.begin() + i);
                  break;
               }
            return Return(state, memory, 8, 0, error);
         }
         if (slot == 9)
         {
            ISimManager *sim = AppGetObj(ISimManager);
            const uint32_t last = sim ? static_cast<uint32_t>(sim->LastMsg()) : 0;
            if (sim) sim->Release();
            return Return(state, memory, 4, last, error);
         }
      }
      else if (id == (kEngineInterface | 0x78) && (slot == 3 || slot == 4))
      {
         // IPropertyManager: GetProperty, GetPropertyNamed
         IPropertyManager *props = AppGetObj(IPropertyManager);
         if (!props)
         {
            if (error) *error = "native property manager is unavailable";
            return false;
         }
         IProperty *prop = 0;
         if (slot == 3)
            prop = props->GetProperty(static_cast<PropertyID>(a));
         else
         {
            std::string name;
            if (!ReadGuestString(memory, a, &name, error))
            {
               props->Release();
               return false;
            }
            prop = props->GetPropertyNamed(name.c_str());
         }
         props->Release();
         uint32_t result = 0;
         if (prop && !(result = CreateInterfaceProxy(kEngineObject | 2, prop, error)))
         {
            prop->Release();
            return false;
         }
         return Return(state, memory, 8, result, error);
      }
      else if (id == (kEngineObject | 2))
      {
         // IProperty, and as IStringProperty (the Design Note's text)
         IProperty *prop = static_cast<IProperty *>(GetProxyNative(guestObject));
         if (!prop)
         {
            if (error) *error = "property proxy without its property";
            return false;
         }
         const ObjID obj = static_cast<ObjID>(static_cast<int32_t>(a));
         switch (slot)
         {
            case 4: return Return(state, memory, 4, static_cast<uint32_t>(prop->GetID()), error);
            case 6: return Return(state, memory, 8, static_cast<uint32_t>(prop->Create(obj)), error);
            case 7:
               return Return(state, memory, 12,
                             static_cast<uint32_t>(prop->Copy(obj, static_cast<ObjID>(static_cast<int32_t>(b)))), error);
            case 8: return Return(state, memory, 8, static_cast<uint32_t>(prop->Delete(obj)), error);
            case 9: return Return(state, memory, 8, prop->IsRelevant(obj) ? 1 : 0, error);
            case 10: return Return(state, memory, 8, prop->IsSimplyRelevant(obj) ? 1 : 0, error);
            case 11: return Return(state, memory, 8, prop->Touch(obj) ? 1 : 0, error);
            case 13:
            {
               // Listen(interests, func, data): the guest's function told of
               // every change, through a native listener; a token its handle
               if (!b) return Return(state, memory, 16, 0, error);
               sGuestListener *l = NewListener(kListenProperty, b, c);
               prop->AddRef();
               l->native = prop;
               l->handle = prop->Listen(static_cast<PropertyListenMsgSet>(a), PropertyListener,
                                        reinterpret_cast<PropListenerData>(l));
               return Return(state, memory, 16, l->token, error);
            }
            case 14:
            {
               for (size_t i = 0; i < m_listeners.size(); ++i)
                  if (m_listeners[i]->kind == kListenProperty && m_listeners[i]->token == a)
                  {
                     sGuestListener *l = m_listeners[i];
                     static_cast<IProperty *>(l->native)->Unlisten(l->handle);
                     l->owner = 0;
                     m_listeners.erase(m_listeners.begin() + i);
                     break;
                  }
               return Return(state, memory, 8, 0, error);
            }
            default: break;
         }
         if (slot >= 18 && slot <= 21)
         {
            IStringProperty *text = 0;
            if (FAILED(prop->QueryInterface(IID_IStringProperty, reinterpret_cast<void **>(&text))) || !text)
               return InvokeTypedProperty(prop, slot, obj, b, state, memory, error);
            uint32_t result = 0;
            bool ok = true;
            if (slot == 18 || slot == 19)
            {
               std::string value;
               ok = !b || ReadGuestString(memory, b, &value, error);
               if (ok)
                  result = slot == 18 ? static_cast<uint32_t>(text->Set(obj, value.c_str()))
                                      : (text->TouchValue(obj, value.c_str()) ? 1 : 0);
            }
            else
            {
               const char *value = 0;
               const BOOL got = slot == 20 ? text->Get(obj, &value) : text->GetSimple(obj, &value);
               result = got ? 1 : 0;
               uint32_t address = 0;
               if (got && b)
                  ok = GuestText(static_cast<int64_t>(prop->GetID()) << 32 | static_cast<uint32_t>(obj),
                                 value, memory, &address, error) &&
                       memory->Write32(b, address, error);
            }
            text->Release();
            return ok && Return(state, memory, 12, result, error);
         }
      }
      else if (id == (kEngineInterface | 0x8f))
      {
         ITraitManager *traits = AppGetObj(ITraitManager);
         if (!traits)
         {
            if (error) *error = "native trait manager is unavailable";
            return false;
         }
         const ObjID objA = static_cast<ObjID>(static_cast<int32_t>(a));
         const ObjID objB = static_cast<ObjID>(static_cast<int32_t>(b));
         bool done = true;
         uint32_t result = 0, bytes = 4;
         switch (slot)
         {
            case 6: result = static_cast<uint32_t>(traits->GetArchetype(objA)); bytes = 8; break;
            case 7: result = static_cast<uint32_t>(traits->SetArchetype(objA, objB)); bytes = 12; break;
            case 8: result = traits->IsArchetype(objA) ? 1 : 0; bytes = 8; break;
            case 9: result = static_cast<uint32_t>(traits->RootMetaProperty()); bytes = 4; break;
            case 11: result = traits->IsMetaProperty(objA) ? 1 : 0; bytes = 8; break;
            case 12: result = static_cast<uint32_t>(traits->AddObjMetaProperty(objA, objB)); bytes = 12; break;
            case 13:
               result = static_cast<uint32_t>(traits->AddObjMetaPropertyPrioritized(
                  objA, objB, static_cast<tMetaPropertyPriority>(c)));
               bytes = 16;
               break;
            case 14: result = static_cast<uint32_t>(traits->RemoveObjMetaProperty(objA, objB)); bytes = 12; break;
            case 15:
               result = static_cast<uint32_t>(traits->RemoveObjMetaPropertyPrioritized(
                  objA, objB, static_cast<tMetaPropertyPriority>(c)));
               bytes = 16;
               break;
            case 16: result = traits->ObjHasDonor(objA, objB) ? 1 : 0; bytes = 12; break;
            case 17: result = traits->ObjHasDonorIntrinsically(objA, objB) ? 1 : 0; bytes = 12; break;
            case 18: result = traits->ObjIsDonor(objA) ? 1 : 0; bytes = 8; break;
            case 21:
               // Listen(func, data): the archetype hierarchy's changes
               bytes = 12;
               if (a)
                  result = static_cast<uint32_t>(traits->Listen(HierarchyListener, NewListener(kListenHierarchy, a, b)));
               break;
            case 19:
            {
               IObjectQuery *query = traits->Query(objA, static_cast<eTraitQueryType>(b));
               bytes = 12;
               if (query)
               {
                  result = CreateInterfaceProxy(kEngineObject | 1, query, error);
                  if (!result)
                  {
                     query->Release();
                     traits->Release();
                     return false;
                  }
               }
               break;
            }
            default: done = false; break;
         }
         traits->Release();
         if (done)
            return Return(state, memory, bytes, result, error);
      }
      char message[96];
      snprintf(message, sizeof(message), "unsupported engine interface %#x method %u", id, slot);
      if (error) *error = message;
      return false;
   }

   virtual bool InvokeScriptService(unsigned slot, osm32::sCpuState *state,
                                    osm32::cMemory *memory,
                                    std::string *error)
   {
      if (slot <= 2)
         return cEnvironment::InvokeScriptService(slot, state, memory, error);

      uint32_t guestObject;
      uint16_t serviceId;
      if (!osm32::ReadStackArgument(*state, *memory, 0, &guestObject,
                                    error) ||
          !GetScriptServiceId(guestObject, &serviceId))
         return false;
      if (serviceId & kEngineInterface)
         return InvokeEngineInterface(serviceId, guestObject, slot, state, memory, error);

      if (!osm32::GuestServiceMethodExists(m_guestAbi, serviceId, slot))
      {
         if (error) *error = "method is absent from the guest service ABI";
         return false;
      }

      GUID guid;
      MAKE_LG_GUID(&guid, serviceId);
      if (getenv("DARK_OSM32_TRACE"))
      {
         fprintf(stderr, "OSM32 service 0x%04x slot %u eip=%08x\n",
                 static_cast<unsigned>(serviceId), slot, state->eip);
         fflush(stderr);
      }
      IUnknown *service = m_scriptManager ?
         m_scriptManager->GetService(&guid) : 0;
      if (!service)
      {
         if (error) *error = "native script service is unavailable";
         return false;
      }

      if (slot == 3 || slot == 4)
      {
         void **vtable = *reinterpret_cast<void ***>(service);
         reinterpret_cast<void (LGAPI *)(void *)>(vtable[slot])(service);
         service->Release();
         return Return(state, memory, 4, 0, error);
      }

      bool invoked = false;
      if (serviceId == 0x00d7)
         invoked = InvokeDebugService(service, slot, state, memory, error);
      else if (serviceId == 0x00df)
         invoked = InvokeObjectService(
            reinterpret_cast<IObjectScriptService *>(service), slot,
            state, memory, error);
      else if (serviceId == 0x00da)
         invoked = InvokePropertyService(
            reinterpret_cast<IPropertyScriptService *>(service), slot,
            state, memory, error);
      else if (serviceId == 0x00f6)
         invoked = InvokeDoorService(service, slot, state, memory, error);
      else if (serviceId == 0x00ee)
         invoked = InvokeLinkService(
            reinterpret_cast<ILinkScriptService *>(service), slot, state,
            memory, error);
      else if (serviceId == 0x0152)
         invoked = InvokeQuestService(service, slot, state, memory, error);
      else if (serviceId == 0x0141)
         invoked = InvokePhysicsService(service, slot, state, memory, error);
      else if (serviceId == 0x00ef)
         invoked = InvokeLinkToolsService(
            reinterpret_cast<ILinkToolsScriptService *>(service), slot,
            state, memory, error);
      else if (serviceId == 0x01b4)
         invoked = InvokeDarkGameService(service, slot, state, memory, error);
      else if (serviceId == 0x00f4)
         invoked = InvokeActReactService(
            reinterpret_cast<IActReactScriptService *>(service), slot, state,
            memory, error);
      else if (serviceId == 0x00f1)
         invoked = InvokeSoundService(service, slot, state, memory, error);
      else if (serviceId == 0x00fb)
         invoked = InvokeLockedService(
            reinterpret_cast<ILockedScriptService *>(service), slot, state,
            memory, error);
      else if (serviceId == 0x016c)
         invoked = InvokeLightService(service, slot, state, memory, error);
      else if (serviceId == 0x01a0)
         invoked = InvokeDataService(
            reinterpret_cast<IDataScriptService *>(service), slot, state,
            memory, error);
      else if (serviceId == 0x00e5)
         invoked = InvokeAIService(service, slot, state, memory, error);
      else if (serviceId == 0x0115)
         invoked = InvokeBowService(service, slot, state, memory, error);
      else if (serviceId == 0x0140)
         invoked = InvokeCameraService(service, slot, state, memory, error);
      else if (serviceId == 0x017d)
         invoked = InvokeContainerService(service, slot, state, memory,
                                          error);
      else if (serviceId == 0x00fe)
         invoked = InvokeDamageService(service, slot, state, memory, error);
      else if (serviceId == 0x0153)
         invoked = InvokePowerupsService(service, slot, state, memory,
                                         error);
      else if (serviceId == 0x019f)
         invoked = InvokeDarkUIService(service, slot, state, memory, error);
      else if (serviceId == 0x0150)
         invoked = InvokeDrkInvService(service, slot, state, memory, error);
      else if (serviceId == 0x0111)
         invoked = InvokePickLockService(service, slot, state, memory, error);
      else if (serviceId == 0x015d)
         invoked = InvokeOneObjectHResultService(
            service, slot, 5, 8, state, memory, error);
      else if (serviceId == 0x010e)
         invoked = InvokeWeaponService(service, slot, state, memory, error);
      else if (serviceId == 0x016a)
         invoked = InvokeAnimTextureService(service, slot, state, memory,
                                            error);
      else if (serviceId == 0x0228)
         invoked = InvokeVersionService(service, slot, state, memory, error);
      else if (serviceId == 0x0229)
         invoked = InvokeEngineService(service, slot, state, memory, error);
      else if (serviceId == 0x0226)
         invoked = InvokeCDService(service, slot, state, memory, error);
      else if (serviceId == 0x010d)
         invoked = InvokeKeyService(service, slot, state, memory, error);
      else if (serviceId == 0x0225)
         invoked = InvokeNetworkingService(service, slot, state, memory,
                                           error);
      else if (serviceId == 0x01f8)
         invoked = InvokePGroupService(service, slot, state, memory, error);
      else if (serviceId == 0x00fd)
         invoked = InvokePuppetService(service, slot, state, memory, error);
      else if (serviceId == 0x00f2)
         invoked = InvokeNullService(service, slot, state, memory, error);
      else
         invoked = cEnvironment::InvokeScriptService(slot, state, memory,
                                                      error);
      service->Release();
      if (!invoked && error && error->find("service 0x") == std::string::npos)
      {
         char context[64];
         snprintf(context, sizeof(context), "service 0x%04x slot %u: ",
                  static_cast<unsigned>(serviceId), slot);
         *error = context + *error;
      }
      return invoked;
   }

private:
   static bool Argument(const osm32::sCpuState &state,
                        const osm32::cMemory &memory, unsigned index,
                        uint32_t *value, std::string *error)
   {
      return osm32::ReadStackArgument(state, memory, index, value, error);
   }

   static bool StringArgument(const osm32::sCpuState &state,
                              osm32::cMemory *memory, unsigned index,
                              std::string *value, std::string *error)
   {
      uint32_t address;
      return Argument(state, *memory, index, &address, error) &&
             ReadGuestString(memory, address, value, error);
   }

   static bool ScriptStringArgument(const osm32::sCpuState &state,
                                    osm32::cMemory *memory, unsigned index,
                                    std::string *value, std::string *error)
   {
      uint32_t objectAddress, textAddress;
      return Argument(state, *memory, index, &objectAddress, error) &&
             memory->Read32(objectAddress, &textAddress, error) &&
             ReadGuestString(memory, textAddress, value, error);
   }

   static bool Return(osm32::sCpuState *state, osm32::cMemory *memory,
                      unsigned argumentBytes, uint32_t result,
                      std::string *error)
   {
      return osm32::ReturnStdcall(state, memory, argumentBytes, result,
                                  error);
   }

   uint32_t CreateLinkQuery(ILinkQuery *query, osm32::cMemory *memory,
                            std::string *error)
   {
      if (!query)
      {
         if (error) *error = "native Link service returned a null query";
         return 0;
      }
      if (!m_linkQueryVtable)
      {
         m_linkQueryVtable = AllocGuest(9 * 4, error);
         if (!m_linkQueryVtable)
            return 0;
         for (unsigned slot = 0; slot < 9; ++slot)
            if (!memory->Write32(m_linkQueryVtable + slot * 4,
                                 kLinkQueryTrap +
                                    slot * kLinkQueryTrapStride,
                                 error))
               return 0;
      }

      sLinkQueryProxy proxy;
      proxy.object = AllocGuest(4, error);
      proxy.serial = m_linkQuerySerial++;
      proxy.query = query;
      proxy.references = 1;
      proxy.data = 0;
      proxy.dataCapacity = 0;
      if (!proxy.object ||
          !memory->Write32(proxy.object, m_linkQueryVtable, error))
         return 0;
      query->AddRef();
      m_linkQueries[proxy.object] = proxy;
      ++g_Osm32LinkQueries;
      return proxy.object;
   }

   bool InvokeLinkQuery(unsigned slot, osm32::sCpuState *state,
                        osm32::cMemory *memory, std::string *error)
   {
      uint32_t object;
      if (!Argument(*state, *memory, 0, &object, error)) return false;
      std::map<uint32_t, sLinkQueryProxy>::iterator found =
         m_linkQueries.find(object);
      sLinkQueryProxy *proxy =
         found != m_linkQueries.end() ? &found->second : 0;
      if (!proxy)
      {
         if (error) *error = "invalid or released guest link query";
         return false;
      }

      if (slot == 0)
      {
         uint32_t output;
         if (!Argument(*state, *memory, 2, &output, error) ||
             !memory->Write32(output, object, error)) return false;
         ++proxy->references;
         proxy->query->AddRef();
         return Return(state, memory, 12, S_OK, error);
      }
      if (slot == 1)
      {
         proxy->query->AddRef();
         return Return(state, memory, 4, ++proxy->references, error);
      }
      if (slot == 2)
      {
         const uint32_t references = --proxy->references;
         proxy->query->Release();
         if (!references)
            DropLinkQuery(object);
         return Return(state, memory, 4, references, error);
      }
      if (slot == 3)
         return Return(state, memory, 4, proxy->query->Done(), error);
      if (slot == 4)
      {
         uint32_t output;
         sLink link = {};
         if (!Argument(*state, *memory, 1, &output, error)) return false;
         const HRESULT result = proxy->query->Link(&link);
         return memory->Write32(output,
                                static_cast<uint32_t>(link.source), error) &&
                memory->Write32(output + 4,
                                static_cast<uint32_t>(link.dest), error) &&
                memory->Write32(output + 8,
                                static_cast<uint32_t>(link.flavor), error) &&
                Return(state, memory, 8, result, error);
      }
      if (slot == 5)
         return Return(state, memory, 4,
                       static_cast<uint32_t>(proxy->query->ID()), error);
      if (slot == 7)
         return Return(state, memory, 4, proxy->query->Next(), error);
      if (slot == 6)
      {
         // Data: the link's data copied to the guest (as large as its
         // relation's says); a copy -- written to, the link keeps its own
         const void *data = proxy->query->Data();
         if (!data)
            return Return(state, memory, 4, 0, error);
         uint32_t size = 0;
         if (ILinkManager *links = AppGetObj(ILinkManager))
         {
            const LinkID id = proxy->query->ID();
            if (IRelation *rel = links->GetRelation(LINKID_RELATION(id)))
            {
               if (const sRelationDataDesc *desc = rel->DescribeData())
                  size = static_cast<uint32_t>(desc->size);
               rel->Release();
            }
            links->Release();
         }
         if (!size || size > 0x10000)
            return Return(state, memory, 4, 0, error);
         if (proxy->dataCapacity < size)
         {
            if (proxy->data) FreeGuest(proxy->data, 0);
            proxy->data = AllocGuest(size, error);
            proxy->dataCapacity = proxy->data ? size : 0;
            if (!proxy->data) return false;
         }
         const uint8_t *bytes = static_cast<const uint8_t *>(data);
         for (uint32_t i = 0; i < size; ++i)
            if (!memory->Write8(proxy->data + i, bytes[i], error)) return false;
         return Return(state, memory, 4, proxy->data, error);
      }
      if (slot == 8)
      {
         // Inverse: a query of its own
         ILinkQuery *inverse = proxy->query->Inverse();
         if (!inverse)
            return Return(state, memory, 4, 0, error);
         const uint32_t guest = CreateLinkQuery(inverse, memory, error);
         inverse->Release();
         return guest && Return(state, memory, 4, guest, error);
      }
      if (error) *error = "unsupported guest ILinkQuery method";
      return false;
   }

   static bool ReturnBoolean(osm32::sCpuState *state,
                             osm32::cMemory *memory,
                             uint32_t resultAddress,
                             unsigned argumentBytes,
                             boolean result,
                             std::string *error)
   {
      // Dark's public `boolean` is the four-byte C++ true_bool wrapper.  The
      // VC6 OSM ABI returns even this small class through a caller-provided
      // hidden result pointer, unlike BOOL and HRESULT which return in EAX.
      return memory->Write32(resultAddress, result ? 1u : 0u, error) &&
             Return(state, memory, argumentBytes, resultAddress, error);
   }

   static bool ReturnFloat(osm32::sCpuState *state,
                           osm32::cMemory *memory,
                           unsigned argumentBytes, float result,
                           std::string *error)
   {
      if (state->x87Depth == 8)
      {
         if (error) *error = "guest x87 stack overflow on service return";
         return false;
      }
      for (unsigned i = state->x87Depth; i != 0; --i)
         state->x87[i] = state->x87[i - 1];
      state->x87[0] = result;
      ++state->x87Depth;
      return Return(state, memory, argumentBytes, 0, error);
   }

   static bool ReadGuestTag(osm32::cMemory *memory, uint32_t address,
                            sScrDatumTag *tag, std::string *className,
                            std::string *datumName, std::string *error)
   {
      uint32_t objectId, classAddress, nameAddress;
      if (!memory->Read32(address, &objectId, error) ||
          !memory->Read32(address + 4, &classAddress, error) ||
          !memory->Read32(address + 8, &nameAddress, error) ||
          !ReadGuestString(memory, classAddress, className, error) ||
          !ReadGuestString(memory, nameAddress, datumName, error))
         return false;
      tag->objId = static_cast<ObjID>(objectId);
      tag->pszClass = className->c_str();
      tag->pszName = datumName->c_str();
      return true;
   }

   bool WriteGuestParm(osm32::cMemory *memory, uint32_t address,
                       const sMultiParm &value, std::string *error)
   {
      uint32_t data = 0;
      switch (value.type)
      {
         case kMT_Undef:
            break;
         case kMT_Int:
            data = static_cast<uint32_t>(value.i);
            break;
         case kMT_Float:
            memcpy(&data, &value.f, sizeof(data));
            break;
         case kMT_Obj:
            data = static_cast<uint32_t>(value.o);
            break;
         case kMT_String:
         {
            const char *text = value.psz ? value.psz : "";
            const size_t size = strlen(text) + 1;
            if (size > 0xffffffffu)
            {
               if (error) *error = "native service string is too large";
               return false;
            }
            data = AllocGuest(static_cast<uint32_t>(size), error);
            if (!data || !memory->Load(data, text, size, error)) return false;
            break;
         }
         case kMT_Vector:
            data = AllocGuest(12, error);
            if (!data || !value.pVec ||
                !memory->Load(data, value.pVec, 12, error)) return false;
            break;
         default:
            if (error) *error = "native service returned invalid multiparm";
            return false;
      }
      return memory->Write32(address, data, error) &&
             memory->Write32(address + 4,
                             static_cast<uint32_t>(value.type), error);
   }

   bool WriteGuestStringObject(osm32::cMemory *memory, uint32_t address,
                               const char *text, std::string *error)
   {
      if (!text) text = "";
      const size_t size = strlen(text) + 1;
      if (size > 0xffffffffu)
      {
         if (error) *error = "native service string is too large";
         return false;
      }
      const uint32_t textAddress =
         AllocGuest(static_cast<uint32_t>(size), error);
      return textAddress && memory->Load(textAddress, text, size, error) &&
             memory->Write32(address, textAddress, error);
   }

   static bool ReadGuestVector(osm32::cMemory *memory, uint32_t address,
                               mxs_vector *vector, std::string *error)
   {
      uint32_t bits[3];
      if (!memory->Read32(address, &bits[0], error) ||
          !memory->Read32(address + 4, &bits[1], error) ||
          !memory->Read32(address + 8, &bits[2], error))
         return false;
      memcpy(vector, bits, sizeof(bits));
      return true;
   }

   static bool WriteGuestVector(osm32::cMemory *memory, uint32_t address,
                                const mxs_vector &vector,
                                std::string *error)
   {
      return memory->Load(address, &vector, 12, error);
   }

   static bool ReadGuestObjectReference(osm32::cMemory *memory,
                                        uint32_t address, object *value,
                                        std::string *error)
   {
      uint32_t objectId;
      if (!memory->Read32(address, &objectId, error))
         return false;
      *value = object(static_cast<int32_t>(objectId));
      return true;
   }

   static bool ReadFloatBits(uint32_t bits, float *value)
   {
      memcpy(value, &bits, sizeof(*value));
      return true;
   }

   static bool ReturnObject(osm32::sCpuState *state,
                            osm32::cMemory *memory,
                            uint32_t resultAddress,
                            unsigned argumentBytes,
                            const object &result,
                            std::string *error)
   {
      return memory->Write32(
                resultAddress,
                static_cast<uint32_t>(static_cast<int>(result)), error) &&
             Return(state, memory, argumentBytes, resultAddress, error);
   }

   bool InvokeObjectService(IObjectScriptService *service,
                            unsigned slot, osm32::sCpuState *state,
                            osm32::cMemory *memory,
                            std::string *error)
   {
      unsigned nativeSlot;
      if (!osm32::ResolveServiceMethod(m_guestAbi, osm32::kAbiServiceObject,
                                       slot, &nativeSlot))
      {
         if (error) *error = "Object method is absent from the guest ABI";
         return false;
      }
      slot = nativeSlot;
      uint32_t first, second;
      std::string text;
      if (slot == 9 || slot == 15 || slot == 16 || slot == 17 ||
          slot == 22 || slot == 26)
      {
         uint32_t resultAddress, objectId;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &objectId, error))
            return false;
         const object target(static_cast<int32_t>(objectId));
         boolean result;
         unsigned argumentBytes = 12;
         if (slot == 15 || slot == 16)
         {
            uint32_t otherId;
            if (!Argument(*state, *memory, 3, &otherId, error)) return false;
            const object other(static_cast<int32_t>(otherId));
            result = slot == 15 ? service->HasMetaProperty(target, other) :
                                  service->InheritsFrom(target, other);
            argumentBytes = 16;
         }
         else if (slot == 9)
            result = service->Exists(target);
         else if (slot == 17)
            result = service->IsTransient(target);
         else if (slot == 22)
            result = service->IsPositionValid(target);
         else
            result = service->RenderedThisFrame(target);
         return ReturnBoolean(state, memory, resultAddress, argumentBytes,
                              result, error);
      }
      if (slot == 5 || slot == 7)
      {
         uint32_t resultAddress, archetype;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &archetype, error))
            return false;
         const object result = slot == 5 ?
            service->BeginCreate(object(static_cast<int32_t>(archetype))) :
            service->Create(object(static_cast<int32_t>(archetype)));
         return memory->Write32(resultAddress,
                                static_cast<uint32_t>(static_cast<int>(result)),
                                error) &&
                Return(state, memory, 12, resultAddress, error);
      }
      if (slot == 12)
      {
         uint32_t resultAddress;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !StringArgument(*state, memory, 2, &text, error))
            return false;
         const object result = service->Named(text.c_str());
         return memory->Write32(resultAddress,
                                static_cast<uint32_t>(static_cast<int>(result)),
                                error) &&
                Return(state, memory, 12, resultAddress, error);
      }
      if (slot == 23)
      {
         uint32_t resultAddress, objectId;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &objectId, error) ||
             !StringArgument(*state, memory, 3, &text, error))
            return false;
         const object result = service->FindClosestObjectNamed(
            static_cast<ObjID>(objectId), text.c_str());
         return memory->Write32(resultAddress,
                                static_cast<uint32_t>(static_cast<int>(result)),
                                error) &&
                Return(state, memory, 16, resultAddress, error);
      }
      if (slot == 11)
      {
         uint32_t resultAddress, objectId;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &objectId, error)) return false;
         const cScrStr result = service->GetName(
            object(static_cast<int32_t>(objectId)));
         return WriteGuestStringObject(memory, resultAddress, result, error) &&
                Return(state, memory, 12, resultAddress, error);
      }
      if (slot == 19 || slot == 20)
      {
         uint32_t resultAddress, objectId;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &objectId, error)) return false;
         const cScrVec result = slot == 19 ?
            service->Position(object(static_cast<int32_t>(objectId))) :
            service->Facing(object(static_cast<int32_t>(objectId)));
         return WriteGuestVector(memory, resultAddress, result, error) &&
                Return(state, memory, 12, resultAddress, error);
      }
      if (slot == 21)
      {
         uint32_t objectId, positionAddress, facingAddress, reference;
         mxs_vector position, facing;
         if (!Argument(*state, *memory, 1, &objectId, error) ||
             !Argument(*state, *memory, 2, &positionAddress, error) ||
             !Argument(*state, *memory, 3, &facingAddress, error) ||
             !Argument(*state, *memory, 4, &reference, error) ||
             !ReadGuestVector(memory, positionAddress, &position, error) ||
             !ReadGuestVector(memory, facingAddress, &facing, error))
            return false;
         return Return(state, memory, 20,
            service->Teleport(object(static_cast<int32_t>(objectId)),
                              cScrVec(position), cScrVec(facing),
                              object(static_cast<int32_t>(reference))), error);
      }
      if (!Argument(*state, *memory, 1, &first, error))
         return false;
      const object object1(static_cast<int32_t>(first));
      switch (slot)
      {
         case 6:
            return Return(state, memory, 8, service->EndCreate(object1), error);
         case 8:
            return Return(state, memory, 8, service->Destroy(object1), error);
         case 10:
            if (!StringArgument(*state, memory, 2, &text, error)) return false;
            return Return(state, memory, 12,
                          service->SetName(object1, text.c_str()), error);
         case 13: case 14:
            if (!Argument(*state, *memory, 2, &second, error)) return false;
            if (slot == 13)
               first = service->AddMetaProperty(object1,
                         object(static_cast<int32_t>(second)));
            else
               first = service->RemoveMetaProperty(object1,
                         object(static_cast<int32_t>(second)));
            return Return(state, memory, 12, first, error);
         case 18:
            if (!Argument(*state, *memory, 2, &second, error)) return false;
            return Return(state, memory, 12,
                          service->SetTransience(object1, second), error);
         case 24: case 25:
            if (!ScriptStringArgument(*state, memory, 2, &text, error))
               return false;
            first = slot == 24 ?
               service->AddMetaPropertyToMany(object1, cScrStr(text.c_str())) :
               service->RemoveMetaPropertyFromMany(object1,
                                                    cScrStr(text.c_str()));
            return Return(state, memory, 12, first, error);
      }
      if (error) *error = "unsupported Object service return ABI";
      return false;
   }

   bool InvokePropertyService(IPropertyScriptService *service,
                              unsigned slot, osm32::sCpuState *state,
                              osm32::cMemory *memory,
                              std::string *error)
   {
      uint32_t objectId, third, fourth;
      std::string property, field;
      unsigned nativeSlot;
      if (!osm32::ResolveServiceMethod(m_guestAbi,
                                       osm32::kAbiServiceProperty,
                                       slot, &nativeSlot))
      {
         if (error) *error = "Property method is absent from the guest ABI";
         return false;
      }
      slot = nativeSlot;
      if (slot == 5)
      {
         uint32_t resultAddress, fieldAddress;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &objectId, error) ||
             !StringArgument(*state, memory, 3, &property, error) ||
             !Argument(*state, *memory, 4, &fieldAddress, error))
            return false;
         if (getenv("DARK_OSM32_TRACE"))
         {
            uint32_t args[7] = { 0 };
            for (unsigned i = 0; i < 7; ++i)
               Argument(*state, *memory, i, &args[i], 0);
            fprintf(stderr,
                    "OSM32 Property Get result=%08x target=%d "
                    "property='%s' field=%08x "
                    "args=%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                    resultAddress, static_cast<int32_t>(objectId),
                    property.c_str(), fieldAddress, args[0], args[1],
                    args[2], args[3], args[4], args[5], args[6]);
            fflush(stderr);
         }
         if (fieldAddress &&
             !ReadGuestString(memory, fieldAddress, &field, error))
            return false;
         cMultiParm result = service->Get(
            object(static_cast<int32_t>(objectId)), property.c_str(),
            fieldAddress ? field.c_str() : 0);
         return WriteGuestParm(memory, resultAddress, result, error) &&
                Return(state, memory, 20, resultAddress, error);
      }

      if (!Argument(*state, *memory, 1, &objectId, error) ||
          !StringArgument(*state, memory, 2, &property, error))
         return false;
      const object target(static_cast<int32_t>(objectId));

      // The VC guest vtable emits the field-taking Set overload before the
      // simple overload.  Gold has no SetLocal method, so its Add through
      // Possessed slots are one earlier than the native Dark 2 service.
      // Dispatch guest slots by the selected content profile rather than
      // forwarding their numbers directly.
      if (slot == 6 || slot == 8)
      {
         uint32_t fieldAddress;
         if (!Argument(*state, *memory, 3, &fieldAddress, error) ||
             (fieldAddress &&
              !ReadGuestString(memory, fieldAddress, &field, error)) ||
             !Argument(*state, *memory, 4, &fourth, error))
            return false;
         if (getenv("DARK_OSM32_TRACE") && slot == 8)
         {
            uint32_t args[7] = { 0 };
            uint32_t returnAddress = 0;
            for (unsigned i = 0; i < 7; ++i)
               Argument(*state, *memory, i, &args[i], 0);
            memory->Read32(state->reg[osm32::kEsp], &returnAddress, 0);
            uint32_t parmData = 0, parmType = 0;
            memory->Read32(fourth, &parmData, 0);
            memory->Read32(fourth + 4, &parmType, 0);
            fprintf(stderr,
                    "OSM32 Property SetLocal target=%d property='%s' "
                    "field='%s' parm=%08x[%08x,%08x] "
                    "return=%08x args=%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                    static_cast<int32_t>(objectId), property.c_str(),
                    fieldAddress ? field.c_str() : "", fourth, parmData,
                    parmType, returnAddress, args[0], args[1], args[2],
                    args[3], args[4], args[5], args[6]);
            fflush(stderr);
         }
         cMultiParm parm;
         if (!ReadGuestParm(memory, fourth, &parm, error))
         {
            uint32_t args[6] = { 0 };
            for (unsigned i = 0; i < 6; ++i)
               Argument(*state, *memory, i, &args[i], 0);
            char context[192];
            snprintf(context, sizeof(context),
                     " [stack this=%08x a1=%08x a2=%08x a3=%08x "
                     "a4=%08x a5=%08x]",
                     args[0], args[1], args[2], args[3], args[4], args[5]);
            if (error) *error += context;
            return false;
         }
         HRESULT result = slot == 6 ?
            service->Set(target, property.c_str(),
                         fieldAddress ? field.c_str() : 0, parm) :
            service->SetLocal(target, property.c_str(),
                              fieldAddress ? field.c_str() : 0, parm);
         return Return(state, memory, 20, result, error);
      }
      if (slot == 7)
      {
         if (!Argument(*state, *memory, 3, &third, error)) return false;
         cMultiParm parm;
         if (!ReadGuestParm(memory, third, &parm, error)) return false;
         return Return(state, memory, 16,
                       service->Set(target, property.c_str(), parm), error);
      }
      if (slot == 9)
         return Return(state, memory, 12,
                       service->Add(target, property.c_str()), error);
      if (slot == 10)
         return Return(state, memory, 12,
                       service->Remove(target, property.c_str()), error);
      if (slot == 11)
      {
         if (!Argument(*state, *memory, 3, &third, error)) return false;
         return Return(state, memory, 16,
                       service->CopyFrom(target, property.c_str(),
                          object(static_cast<int32_t>(third))), error);
      }
      if (slot == 12)
         return Return(state, memory, 12,
                       service->Possessed(target, property.c_str()), error);
      if (slot == 13)
      {
         IPropertyManager *manager = AppGetObj(IPropertyManager);
         if (!manager)
         {
            if (error) *error = "native property manager is unavailable";
            return false;
         }
         IProperty *nativeProperty = manager->GetPropertyNamed(
            property.c_str());
         manager->Release();
         const BOOL possessed = nativeProperty ?
            nativeProperty->IsSimplyRelevant(static_cast<ObjID>(objectId)) :
            FALSE;
         if (nativeProperty)
            nativeProperty->Release();
         return Return(state, memory, 12, possessed, error);
      }
      if (error)
      {
         uint32_t args[6] = { 0 };
         for (unsigned i = 0; i < 6; ++i)
            Argument(*state, *memory, i, &args[i], 0);
         char context[224];
         snprintf(context, sizeof(context),
                  "unsupported Property guest slot %u "
                  "[this=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x]",
                  slot, args[0], args[1], args[2], args[3], args[4], args[5]);
         *error = context;
      }
      return false;
   }

   static bool InvokeDoorService(IUnknown *service, unsigned slot,
                                 osm32::sCpuState *state,
                                 osm32::cMemory *memory,
                                 std::string *error)
   {
      uint32_t objectId, value;
      if (!Argument(*state, *memory, 1, &objectId, error)) return false;
      void **vtable = *reinterpret_cast<void ***>(service);
      typedef uint32_t (LGAPI *tOne)(void *, uint32_t);
      typedef uint32_t (LGAPI *tTwo)(void *, uint32_t, uint32_t);
      if (slot >= 5 && slot <= 8 || slot == 10)
      {
         value = reinterpret_cast<tOne>(vtable[slot])(service, objectId);
         return Return(state, memory, 8, value, error);
      }
      if (slot == 9)
      {
         if (!Argument(*state, *memory, 2, &value, error)) return false;
         value = reinterpret_cast<tTwo>(vtable[slot])(service, objectId,
                                                      value);
         return Return(state, memory, 12, value, error);
      }
      if (error) *error = "unsupported Door service method";
      return false;
   }

   static bool InvokeLockedService(ILockedScriptService *service,
                                   unsigned slot, osm32::sCpuState *state,
                                   osm32::cMemory *memory,
                                   std::string *error)
   {
      if (slot != 5)
      {
         if (error) *error = "unsupported Locked service method";
         return false;
      }
      // ILockedScriptService::IsLocked takes const object&, so the x86
      // caller passes the address of its four-byte object wrapper.
      uint32_t objectAddress, objectId;
      if (!Argument(*state, *memory, 1, &objectAddress, error) ||
          !memory->Read32(objectAddress, &objectId, error))
         return false;
      return Return(state, memory, 8,
                    service->IsLocked(
                       object(static_cast<int32_t>(objectId))), error);
   }

   static bool InvokeLightService(IUnknown *service,
                                  unsigned slot, osm32::sCpuState *state,
                                  osm32::cMemory *memory,
                                  std::string *error)
   {
      uint32_t objectAddress, objectId, mode;
      if (!Argument(*state, *memory, 1, &objectAddress, error) ||
          !memory->Read32(objectAddress, &objectId, error))
         return false;
      const object target(static_cast<int32_t>(objectId));
      void **vtable = *reinterpret_cast<void ***>(service);
      typedef void (LGAPI *tSet)(void *, const object *, int, float, float);
      typedef void (LGAPI *tSetMode)(void *, const object *, int);
      typedef void (LGAPI *tObject)(void *, const object *);
      typedef int (LGAPI *tGetMode)(void *, const object *);
      if (slot == 5)
      {
         uint32_t minBits, maxBits;
         float minimum, maximum;
         if (!Argument(*state, *memory, 2, &mode, error) ||
             !Argument(*state, *memory, 3, &minBits, error) ||
             !Argument(*state, *memory, 4, &maxBits, error))
            return false;
         memcpy(&minimum, &minBits, sizeof(minimum));
         memcpy(&maximum, &maxBits, sizeof(maximum));
         reinterpret_cast<tSet>(vtable[slot])(
            service, &target, static_cast<int32_t>(mode), minimum, maximum);
         return Return(state, memory, 20, 0, error);
      }
      if (slot == 6)
      {
         if (!Argument(*state, *memory, 2, &mode, error)) return false;
         reinterpret_cast<tSetMode>(vtable[slot])(
            service, &target, static_cast<int32_t>(mode));
         return Return(state, memory, 12, 0, error);
      }
      if (slot >= 7 && slot <= 10)
      {
         reinterpret_cast<tObject>(vtable[slot])(service, &target);
         return Return(state, memory, 8, 0, error);
      }
      if (slot == 11)
         return Return(state, memory, 8,
                       reinterpret_cast<tGetMode>(vtable[slot])(
                          service, &target), error);
      if (error) *error = "unsupported Light service method";
      return false;
   }

   bool InvokeDataService(IDataScriptService *service,
                          unsigned slot, osm32::sCpuState *state,
                          osm32::cMemory *memory,
                          std::string *error)
   {
      if (slot == 5)
      {
         uint32_t resultAddress;
         std::string table, name, fallback, path;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !StringArgument(*state, memory, 2, &table, error) ||
             !StringArgument(*state, memory, 3, &name, error) ||
             !StringArgument(*state, memory, 4, &fallback, error) ||
             !StringArgument(*state, memory, 5, &path, error))
            return false;
         const string result = service->GetString(
            table.c_str(), name.c_str(), fallback.c_str(), path.c_str());
         return WriteGuestStringObject(memory, resultAddress, result, error) &&
                Return(state, memory, 24, resultAddress, error);
      }
      if (slot == 6)
      {
         uint32_t resultAddress, objectId;
         std::string table;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &objectId, error) ||
             !StringArgument(*state, memory, 3, &table, error))
            return false;
         const string result = service->GetObjString(
            static_cast<ObjID>(objectId), table.c_str());
         return WriteGuestStringObject(memory, resultAddress, result, error) &&
                Return(state, memory, 16, resultAddress, error);
      }
      if (slot == 7)
         return Return(state, memory, 4, service->DirectRand(), error);
      if (slot == 8)
      {
         uint32_t low, high;
         if (!Argument(*state, *memory, 1, &low, error) ||
             !Argument(*state, *memory, 2, &high, error)) return false;
         return Return(state, memory, 12,
                       service->RandInt(static_cast<int32_t>(low),
                                        static_cast<int32_t>(high)), error);
      }
      if (slot == 9)
         return ReturnFloat(state, memory, 4, service->RandFlt0to1(), error);
      if (slot == 10)
         return ReturnFloat(state, memory, 4, service->RandFltNeg1to1(),
                            error);
      if (error) *error = "unsupported Data service method";
      return false;
   }

   bool InvokeLinkService(ILinkScriptService *service, unsigned slot,
                          osm32::sCpuState *state,
                          osm32::cMemory *memory,
                          std::string *error)
   {
      uint32_t first, second, third, fourth;
      if (!Argument(*state, *memory, 1, &first, error)) return false;
      if (slot == 5 || slot == 9)
      {
         uint32_t resultAddress;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &first, error) ||
             !Argument(*state, *memory, 3, &second, error) ||
             !Argument(*state, *memory, 4, &third, error))
            return false;
         const linkkind kind(static_cast<int32_t>(first));
         const object from(static_cast<int32_t>(second));
         const object to(static_cast<int32_t>(third));
         const link result = slot == 5 ? service->Create(kind, from, to) :
                                         service->GetOne(kind, from, to);
          return memory->Write32(resultAddress,
                                 static_cast<uint32_t>(static_cast<LinkID>(result)),
                                error) &&
                Return(state, memory, 20, resultAddress, error);
      }
      if (slot == 8)
      {
         uint32_t resultAddress;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &first, error) ||
             !Argument(*state, *memory, 3, &second, error) ||
             !Argument(*state, *memory, 4, &third, error))
            return false;

         ILinkManager *manager = AppGetObj(ILinkManager);
         if (!manager)
         {
            if (error) *error = "native link manager is unavailable";
            return false;
         }
         ILinkQuery *nativeQuery = manager->Query(
            static_cast<ObjID>(second), static_cast<ObjID>(third),
            static_cast<RelationID>(first));
         manager->Release();
         const uint32_t guestQuery = CreateLinkQuery(nativeQuery, memory,
                                                      error);
         if (nativeQuery)
            nativeQuery->Release();
         return guestQuery &&
                memory->Write32(resultAddress, guestQuery, error) &&
                Return(state, memory, 20, resultAddress, error);
      }
      if (slot == 14 || slot == 15)
      {
         uint32_t resultAddress;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &first, error) ||
             !Argument(*state, *memory, 3, &second, error) ||
             !Argument(*state, *memory, 4, &third, error))
            return false;
         const linkkind kind(static_cast<int32_t>(first));
         const object from(static_cast<int32_t>(second));
         const object to(static_cast<int32_t>(third));
         ILinkManager *manager = AppGetObj(ILinkManager);
         if (!manager)
         {
            if (error) *error = "native link manager is unavailable";
            return false;
         }
         IRelation *relation = manager->GetRelation(
            static_cast<RelationID>(static_cast<int32_t>(kind)));
         manager->Release();
         if (!relation)
         {
            if (error) *error = "native inherited-link relation is unavailable";
            return false;
         }
         ILinkQuery *nativeQuery = slot == 14 ?
            QueryInheritedLinksUncached(relation, from, to) :
            QueryInheritedLinksSingleUncached(relation, from, to);
         relation->Release();
         const uint32_t guestQuery = CreateLinkQuery(nativeQuery, memory,
                                                      error);
         if (nativeQuery)
            nativeQuery->Release();
         return guestQuery &&
                memory->Write32(resultAddress, guestQuery, error) &&
                Return(state, memory, 20, resultAddress, error);
      }
      if (slot == 7)
      {
         uint32_t resultAddress;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &first, error) ||
             !Argument(*state, *memory, 3, &second, error) ||
             !Argument(*state, *memory, 4, &third, error)) return false;
         const linkkind kind(static_cast<int32_t>(first));
         const object from(static_cast<int32_t>(second));
         const object to(static_cast<int32_t>(third));
         return ReturnBoolean(state, memory, resultAddress, 20,
                              service->AnyExist(kind, from, to), error);
      }
      if (slot == 6)
         return Return(state, memory, 8,
                       service->Destroy(link(static_cast<int32_t>(first))),
                       error);
      if (slot == 10 || slot == 11)
      {
         uint32_t selfId;
         std::string message;
         if (!memory->Read32(first, &selfId, error) ||
             !StringArgument(*state, memory, 2, &message, error) ||
             !Argument(*state, *memory, 3, &third, error)) return false;
         const object self(static_cast<int32_t>(selfId));
         const linkkind recipients(static_cast<int32_t>(third));
         HRESULT result;
         // MSVC emits the overload without link data first in the guest
         // vtable despite the source declaration order.
         if (slot == 11)
         {
            if (!Argument(*state, *memory, 4, &fourth, error)) return false;
            cMultiParm data;
            if (!ReadGuestParm(memory, fourth, &data, error)) return false;
            result = service->BroadcastOnAllLinks(self, message.c_str(),
                                                  recipients, data);
         }
         else
            result = service->BroadcastOnAllLinks(self, message.c_str(),
                                                  recipients);
         return Return(state, memory, slot == 11 ? 20 : 16, result, error);
      }
      if (slot == 12 || slot == 13)
      {
         std::string fromSet, toSet;
         if (!ScriptStringArgument(*state, memory, 2, &fromSet, error) ||
             !ScriptStringArgument(*state, memory, 3, &toSet, error))
            return false;
         const linkkind kind(static_cast<int32_t>(first));
         const cScrStr nativeFrom(fromSet.c_str());
         const cScrStr nativeTo(toSet.c_str());
         const HRESULT result = slot == 12 ?
            service->CreateMany(kind, nativeFrom, nativeTo) :
            service->DestroyMany(kind, nativeFrom, nativeTo);
         return Return(state, memory, 16, result, error);
      }
      if (error) *error = "unsupported Link service proxy return ABI";
      return false;
   }

   bool InvokeLinkToolsService(ILinkToolsScriptService *service,
                               unsigned slot, osm32::sCpuState *state,
                               osm32::cMemory *memory,
                               std::string *error)
   {
      uint32_t first, second, third;
      std::string text;
      if (slot == 5)
      {
         if (!StringArgument(*state, memory, 1, &text, error)) return false;
         return Return(state, memory, 8,
                       service->LinkKindNamed(text.c_str()), error);
      }
      if (slot == 6)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         const string result = service->LinkKindName(
            static_cast<int32_t>(second));
         return WriteGuestStringObject(memory, first, result, error) &&
                Return(state, memory, 12, first, error);
      }
      if (slot == 7)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         sLink result = {};
         const HRESULT status = service->LinkGet(
            static_cast<int32_t>(first), result);
         return memory->Write32(second,
                                static_cast<uint32_t>(result.source), error) &&
                memory->Write32(second + 4,
                                static_cast<uint32_t>(result.dest), error) &&
                memory->Write32(second + 8,
                                static_cast<uint32_t>(result.flavor), error) &&
                Return(state, memory, 12, status, error);
      }
      if (slot == 8)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error) ||
             !Argument(*state, *memory, 3, &third, error)) return false;
         if (third && !ReadGuestString(memory, third, &text, error))
            return false;
         const cMultiParm result = service->LinkGetData(
            static_cast<int32_t>(second), third ? text.c_str() : 0);
         return WriteGuestParm(memory, first, result, error) &&
                Return(state, memory, 16, first, error);
      }
      if (slot == 9)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error) ||
             !Argument(*state, *memory, 3, &third, error)) return false;
         if (second && !ReadGuestString(memory, second, &text, error))
            return false;
         cMultiParm value;
         if (!ReadGuestParm(memory, third, &value, error)) return false;
         const HRESULT status = service->LinkSetData(
            static_cast<int32_t>(first), second ? text.c_str() : 0, value);
         return Return(state, memory, 16, status, error);
      }
      if (error) *error = "unsupported LinkTools service method";
      return false;
   }

   static bool InvokeQuestService(IUnknown *service, unsigned slot,
                                  osm32::sCpuState *state,
                                  osm32::cMemory *memory,
                                  std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(service);
      uint32_t first, second, third;
      std::string name;
      typedef uint32_t (LGAPI *tString)(void *, const char *);
      typedef uint32_t (LGAPI *tObjString)(void *, uint32_t, const char *);
      typedef uint32_t (LGAPI *tObjStringType)(void *, uint32_t,
                                               const char *, uint32_t);
      typedef uint32_t (LGAPI *tStringIntType)(void *, const char *,
                                               uint32_t, uint32_t);
      if (slot == 5)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !StringArgument(*state, memory, 2, &name, error) ||
             !Argument(*state, *memory, 3, &third, error)) return false;
         first = reinterpret_cast<tObjStringType>(vtable[slot])(
            service, first, name.c_str(), third);
         return Return(state, memory, 16, first, error);
      }
      if (slot == 6)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !StringArgument(*state, memory, 2, &name, error)) return false;
         first = reinterpret_cast<tObjString>(vtable[slot])(
            service, first, name.c_str());
         return Return(state, memory, 12, first, error);
      }
      if (slot == 7)
      {
         if (!StringArgument(*state, memory, 1, &name, error) ||
             !Argument(*state, *memory, 2, &second, error) ||
             !Argument(*state, *memory, 3, &third, error)) return false;
         first = reinterpret_cast<tStringIntType>(vtable[slot])(
            service, name.c_str(), second, third);
         return Return(state, memory, 16, first, error);
      }
      if (slot >= 8 && slot <= 10)
      {
         if (!StringArgument(*state, memory, 1, &name, error)) return false;
         first = reinterpret_cast<tString>(vtable[slot])(service,
                                                         name.c_str());
         return Return(state, memory, 8, first, error);
      }
      if (error) *error = "unsupported Quest service method";
      return false;
   }

   bool InvokeActReactService(IActReactScriptService *service, unsigned slot,
                              osm32::sCpuState *state,
                              osm32::cMemory *memory,
                              std::string *error)
   {
      uint32_t args[12];
      if (slot == 5)
      {
         for (unsigned i = 0; i < 12; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         float intensity;
         memcpy(&intensity, &args[1], sizeof(intensity));
         cMultiParm parms[8];
         for (unsigned i = 0; i < 8; ++i)
            if (!ReadGuestParm(memory, args[i + 4], &parms[i], error))
               return false;
         const HRESULT result = service->React(
            reaction_kind(static_cast<int32_t>(args[0])), intensity,
            object(static_cast<int32_t>(args[2])),
            object(static_cast<int32_t>(args[3])), parms[0], parms[1],
            parms[2], parms[3], parms[4], parms[5], parms[6], parms[7]);
         return Return(state, memory, 52, result, error);
      }
      if (slot == 6)
      {
         const bool hasSource =
            osm32::GuestStimulateHasSourceArgument(m_guestAbi);
         const unsigned count = hasSource ? 4 : 3;
         for (unsigned i = 0; i < count; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         float intensity;
         memcpy(&intensity, &args[2], sizeof(intensity));
         const HRESULT result = service->Stimulate(
            object(static_cast<int32_t>(args[0])),
            stimulus_kind(static_cast<int32_t>(args[1])), intensity,
            object(hasSource ? static_cast<int32_t>(args[3]) : 0));
         return Return(state, memory, 4 + count * 4, result, error);
      }
      if (slot == 7)
      {
         std::string name;
         if (!StringArgument(*state, memory, 1, &name, error)) return false;
         return Return(state, memory, 8,
                       service->GetReactionNamed(name.c_str()), error);
      }
      if (slot == 8)
      {
         uint32_t resultAddress, id;
         if (!Argument(*state, *memory, 1, &resultAddress, error) ||
             !Argument(*state, *memory, 2, &id, error)) return false;
         const cScrStr result = service->GetReactionName(
            static_cast<int32_t>(id));
         return WriteGuestStringObject(memory, resultAddress, result, error) &&
                Return(state, memory, 12, resultAddress, error);
      }
      if (slot >= 9 && slot <= 13)
      {
         if (!Argument(*state, *memory, 1, &args[0], error) ||
             !Argument(*state, *memory, 2, &args[1], error)) return false;
         HRESULT result;
         const object first(static_cast<int32_t>(args[0]));
         const object second(static_cast<int32_t>(args[1]));
         if (slot == 9)
            result = service->SubscribeToStimulus(first, second);
         else if (slot == 10)
            result = service->UnsubscribeToStimulus(first, second);
         else if (slot == 11)
            result = service->BeginContact(first, second);
         else if (slot == 12)
            result = service->EndContact(first, second);
         else
            result = service->SetSingleSensorContact(first, second);
         return Return(state, memory, 12, result, error);
      }
      if (error) *error = "unsupported ActReact service method";
      return false;
   }

   bool InvokeAIService(IUnknown *service, unsigned slot,
                        osm32::sCpuState *state, osm32::cMemory *memory,
                        std::string *error)
   {
      IAIScriptService *ai = reinterpret_cast<IAIScriptService *>(service);
      unsigned nativeSlot;
      if (!osm32::ResolveServiceMethod(m_guestAbi, osm32::kAbiServiceAI,
                                       slot, &nativeSlot))
      {
         if (error) *error = "AI method is absent from the guest ABI";
         return false;
      }
      slot = nativeSlot;
      uint32_t args[7] = {};
      if (slot >= 5 && slot <= 7)
      {
         const unsigned count = slot == 5 ? 6 : (slot == 6 ? 5 : 6);
         for (unsigned i = 0; i < count; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         object target, with;
         cMultiParm data;
         boolean result;
         if (!ReadGuestObjectReference(memory, args[2], &target, error))
            return false;
         if (slot == 5)
         {
            if (!ReadGuestParm(memory, args[5], &data, error)) return false;
            result = ai->MakeGotoObjLoc(
               static_cast<ObjID>(args[1]), target,
               static_cast<eAIScriptSpeed>(args[3]),
               static_cast<eAIActionPriority>(args[4]), data);
         }
         else if (slot == 6)
         {
            if (!ReadGuestParm(memory, args[4], &data, error)) return false;
            result = ai->MakeFrobObj(
               static_cast<ObjID>(args[1]), target,
               static_cast<eAIActionPriority>(args[3]), data);
         }
         else
         {
            if (!ReadGuestObjectReference(memory, args[3], &with, error) ||
                !ReadGuestParm(memory, args[5], &data, error)) return false;
            result = ai->MakeFrobObj(
               static_cast<ObjID>(args[1]), target, with,
               static_cast<eAIActionPriority>(args[4]), data);
         }
         return ReturnBoolean(state, memory, args[0], 4 + count * 4,
                              result, error);
      }
      if (slot == 14)
      {
         if (!Argument(*state, *memory, 1, &args[0], error) ||
             !Argument(*state, *memory, 2, &args[1], error)) return false;
         boolean result;
         result = ai->StartConversation(static_cast<ObjID>(args[1]));
         return ReturnBoolean(state, memory, args[0], 12, result, error);
      }
      if (!Argument(*state, *memory, 1, &args[0], error)) return false;
      switch (slot)
      {
         case 8:
            return Return(state, memory, 8,
                          ai->GetAlertLevel(static_cast<ObjID>(args[0])),
                          error);
         case 9:
            if (!Argument(*state, *memory, 2, &args[1], error)) return false;
            ai->SetMinimumAlert(static_cast<ObjID>(args[0]),
                                static_cast<eAIScriptAlertLevel>(args[1]));
            return Return(state, memory, 12, 0, error);
         case 10:
            ai->ClearGoals(static_cast<ObjID>(args[0]));
            return Return(state, memory, 8, 0, error);
         case 11:
            if (!Argument(*state, *memory, 2, &args[1], error)) return false;
            ai->SetScriptFlags(static_cast<ObjID>(args[0]),
                               static_cast<int32_t>(args[1]));
            return Return(state, memory, 12, 0, error);
         case 12:
            ai->ClearAlertness(static_cast<ObjID>(args[0]));
            return Return(state, memory, 8, 0, error);
         case 13:
         {
            std::string signal;
            if (!ScriptStringArgument(*state, memory, 2, &signal, error))
               return false;
            const cScrStr nativeSignal(signal.c_str());
            ai->Signal(static_cast<ObjID>(args[0]), nativeSignal);
            return Return(state, memory, 12, 0, error);
         }
      }
      if (error) *error = "unsupported AI service method";
      return false;
   }

   static bool InvokeDrkInvService(IUnknown *service, unsigned slot,
                                   osm32::sCpuState *state,
                                   osm32::cMemory *memory,
                                   std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(service);
      uint32_t first, second, third;
      if (slot == 5)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         reinterpret_cast<void (LGAPI *)(void *, uint32_t, uint32_t)>(
            vtable[slot])(service, first, second);
         return Return(state, memory, 12, 0, error);
      }
      std::string name;
      if ((slot != 6 && slot != 7) ||
          !StringArgument(*state, memory, 1, &name, error))
      {
         if (error && slot != 6 && slot != 7)
            *error = "unsupported DrkInv service method";
         return false;
      }
      if (slot == 7)
      {
         reinterpret_cast<void (LGAPI *)(void *, const char *)>(
            vtable[slot])(service, name.c_str());
         return Return(state, memory, 8, 0, error);
      }
      if (!Argument(*state, *memory, 2, &second, error) ||
          !Argument(*state, *memory, 3, &third, error)) return false;
      float speed, rotation;
      ReadFloatBits(second, &speed);
      ReadFloatBits(third, &rotation);
      reinterpret_cast<void (LGAPI *)(void *, const char *, float, float)>(
         vtable[slot])(service, name.c_str(), speed, rotation);
      return Return(state, memory, 16, 0, error);
   }

   static bool InvokePickLockService(IUnknown *service, unsigned slot,
                                     osm32::sCpuState *state,
                                     osm32::cMemory *memory,
                                     std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(service);
      uint32_t args[3] = {};
      unsigned count;
      if (slot == 5 || slot == 6) count = 2;
      else if (slot == 7 || slot == 9) count = 3;
      else if (slot == 8 || slot == 10) count = 1;
      else
      {
         if (error) *error = "unsupported PickLock service method";
         return false;
      }
      for (unsigned i = 0; i < count; ++i)
         if (!Argument(*state, *memory, i + 1, &args[i], error)) return false;
      BOOL result;
      if (slot == 5 || slot == 6)
         result = reinterpret_cast<BOOL (LGAPI *)(void *, object, object)>(
            vtable[slot])(
               service, object(static_cast<int32_t>(args[0])),
               object(static_cast<int32_t>(args[1])));
      else if (slot == 7)
         result = reinterpret_cast<BOOL (LGAPI *)(
            void *, object, object, object)>(vtable[slot])(
               service, object(static_cast<int32_t>(args[0])),
               object(static_cast<int32_t>(args[1])),
               object(static_cast<int32_t>(args[2])));
      else if (slot == 8)
         result = reinterpret_cast<BOOL (LGAPI *)(void *, object)>(
            vtable[slot])(
               service, object(static_cast<int32_t>(args[0])));
      else if (slot == 9)
         result = reinterpret_cast<BOOL (LGAPI *)(void *, object, object,
                                                  int)>(vtable[slot])(
            service, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])),
            static_cast<int32_t>(args[2]));
      else
         result = reinterpret_cast<BOOL (LGAPI *)(void *, BOOL)>(
            vtable[slot])(service, static_cast<BOOL>(args[0]));
      return Return(state, memory, 4 + count * 4, result, error);
   }

   static bool InvokeOneObjectHResultService(
      IUnknown *service, unsigned slot, unsigned firstSlot,
      unsigned lastSlot, osm32::sCpuState *state, osm32::cMemory *memory,
      std::string *error)
   {
      if (slot < firstSlot || slot > lastSlot)
      {
         if (error) *error = "unsupported one-object service method";
         return false;
      }
      uint32_t objectId;
      if (!Argument(*state, *memory, 1, &objectId, error)) return false;
      void **vtable = *reinterpret_cast<void ***>(service);
      return Return(state, memory, 8,
                    reinterpret_cast<HRESULT (LGAPI *)(void *, object)>(
                       vtable[slot])(
                          service, object(static_cast<int32_t>(objectId))),
                    error);
   }

   static bool InvokeWeaponService(IUnknown *service, unsigned slot,
                                   osm32::sCpuState *state,
                                   osm32::cMemory *memory,
                                   std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(service);
      uint32_t first, second;
      if (!Argument(*state, *memory, 1, &first, error)) return false;
      if (slot == 6)
         return Return(state, memory, 8,
                       reinterpret_cast<HRESULT (LGAPI *)(void *, object)>(
                          vtable[slot])(
                             service, object(static_cast<int32_t>(first))),
                       error);
      if (slot < 5 || slot > 9 ||
          !Argument(*state, *memory, 2, &second, error))
      {
         if (error && (slot < 5 || slot > 9))
            *error = "unsupported Weapon service method";
         return false;
      }
      if (slot == 5)
         return Return(state, memory, 12,
                       reinterpret_cast<HRESULT (LGAPI *)(
                          void *, object, int)>(vtable[slot])(
                             service, object(static_cast<int32_t>(first)),
                             static_cast<int32_t>(second)), error);
      typedef uint32_t (LGAPI *tMethod)(void *, object, object);
      return Return(state, memory, 12,
                    reinterpret_cast<tMethod>(vtable[slot])(
                       service, object(static_cast<int32_t>(first)),
                       object(static_cast<int32_t>(second))), error);
   }

   static bool InvokeAnimTextureService(IUnknown *service, unsigned slot,
                                        osm32::sCpuState *state,
                                        osm32::cMemory *memory,
                                        std::string *error)
   {
      if (slot != 5)
      {
         if (error) *error = "unsupported AnimTexture service method";
         return false;
      }
      uint32_t objectId;
      uint32_t addresses[4];
      std::string names[4];
      if (!Argument(*state, *memory, 1, &objectId, error)) return false;
      for (unsigned i = 0; i < 4; ++i)
         if (!Argument(*state, *memory, i + 2, &addresses[i], error) ||
             (addresses[i] &&
              !ReadGuestString(memory, addresses[i], &names[i], error)))
            return false;
      if (!addresses[1] || !addresses[3])
      {
         if (error) *error = "AnimTexture texture name is null";
         return false;
      }
      void **vtable = *reinterpret_cast<void ***>(service);
      typedef HRESULT (LGAPI *tMethod)(void *, object, const char *,
         const char *, const char *, const char *);
      return Return(state, memory, 24,
         reinterpret_cast<tMethod>(vtable[slot])(
            service, object(static_cast<int32_t>(objectId)),
            addresses[0] ? names[0].c_str() : 0, names[1].c_str(),
            addresses[2] ? names[2].c_str() : 0,
            names[3].c_str()), error);
   }

   // IEngineScriptService (NewDark): the configuration, key bindings, files
   // in the search paths, the canvas, fog (NVScript asks at start)
   bool InvokeEngineService(IUnknown *unknown, unsigned slot,
                            osm32::sCpuState *state, osm32::cMemory *memory,
                            std::string *error)
   {
      IEngineScriptService *engine = reinterpret_cast<IEngineScriptService *>(unknown);
      std::string name, file;
      uint32_t out = 0, out2 = 0, out3 = 0, out4 = 0;
      switch (slot)
      {
         case 5:   // ConfigIsDefined
            if (!StringArgument(*state, memory, 1, &name, error)) return false;
            return Return(state, memory, 8, engine->ConfigIsDefined(name.c_str()) ? 1 : 0, error);
         case 6:   // ConfigGetInt
         {
            int value = 0;
            if (!StringArgument(*state, memory, 1, &name, error) ||
                !Argument(*state, *memory, 2, &out, error)) return false;
            const BOOL found = engine->ConfigGetInt(name.c_str(), value);
            return (!found || memory->Write32(out, static_cast<uint32_t>(value), error)) &&
                   Return(state, memory, 12, found ? 1 : 0, error);
         }
         case 7:   // ConfigGetFloat
         {
            real value = 0;
            if (!StringArgument(*state, memory, 1, &name, error) ||
                !Argument(*state, *memory, 2, &out, error)) return false;
            const BOOL found = engine->ConfigGetFloat(name.c_str(), value);
            uint32_t bits;
            memcpy(&bits, &value, 4);
            return (!found || memory->Write32(out, bits, error)) &&
                   Return(state, memory, 12, found ? 1 : 0, error);
         }
         case 8:   // ConfigGetRaw
         {
            cScrStr value;
            if (!StringArgument(*state, memory, 1, &name, error) ||
                !Argument(*state, *memory, 2, &out, error)) return false;
            const BOOL found = engine->ConfigGetRaw(name.c_str(), value);
            return WriteGuestStringObject(memory, out, value, error) &&
                   Return(state, memory, 12, found ? 1 : 0, error);
         }
         case 9:   // BindingGetFloat
            if (!StringArgument(*state, memory, 1, &name, error)) return false;
            return ReturnFloat(state, memory, 8, engine->BindingGetFloat(name.c_str()), error);
         case 10:  // FindFileInPath
         {
            cScrStr value;
            if (!StringArgument(*state, memory, 1, &name, error) ||
                !StringArgument(*state, memory, 2, &file, error) ||
                !Argument(*state, *memory, 3, &out, error)) return false;
            const BOOL found = engine->FindFileInPath(name.c_str(), file.c_str(), value);
            return WriteGuestStringObject(memory, out, value, error) &&
                   Return(state, memory, 16, found ? 1 : 0, error);
         }
         case 11:  // IsRunningDX6
            return Return(state, memory, 4, engine->IsRunningDX6() ? 1 : 0, error);
         case 12:  // GetCanvasSize
         {
            int w = 0, h = 0;
            if (!Argument(*state, *memory, 1, &out, error) ||
                !Argument(*state, *memory, 2, &out2, error)) return false;
            engine->GetCanvasSize(w, h);
            return memory->Write32(out, static_cast<uint32_t>(w), error) &&
                   memory->Write32(out2, static_cast<uint32_t>(h), error) &&
                   Return(state, memory, 12, 0, error);
         }
         case 13:  // GetAspectRatio
            return ReturnFloat(state, memory, 4, engine->GetAspectRatio(), error);
         case 14:  // GetFog
         {
            int r = 0, g = 0, b = 0;
            float d = 0;
            uint32_t outD;
            if (!Argument(*state, *memory, 1, &out, error) || !Argument(*state, *memory, 2, &out2, error) ||
                !Argument(*state, *memory, 3, &out3, error) || !Argument(*state, *memory, 4, &outD, error))
               return false;
            engine->GetFog(r, g, b, d);
            uint32_t bits;
            memcpy(&bits, &d, 4);
            return memory->Write32(out, static_cast<uint32_t>(r), error) &&
                   memory->Write32(out2, static_cast<uint32_t>(g), error) &&
                   memory->Write32(out3, static_cast<uint32_t>(b), error) &&
                   memory->Write32(outD, bits, error) && Return(state, memory, 20, 0, error);
         }
         case 16:  // GetFogZone
         {
            int r = 0, g = 0, b = 0;
            float d = 0;
            uint32_t zone, outD;
            if (!Argument(*state, *memory, 1, &zone, error) || !Argument(*state, *memory, 2, &out, error) ||
                !Argument(*state, *memory, 3, &out2, error) || !Argument(*state, *memory, 4, &out3, error) ||
                !Argument(*state, *memory, 5, &outD, error))
               return false;
            engine->GetFogZone(static_cast<int>(zone), r, g, b, d);
            uint32_t bits;
            memcpy(&bits, &d, 4);
            return memory->Write32(out, static_cast<uint32_t>(r), error) &&
                   memory->Write32(out2, static_cast<uint32_t>(g), error) &&
                   memory->Write32(out3, static_cast<uint32_t>(b), error) &&
                   memory->Write32(outD, bits, error) && Return(state, memory, 24, 0, error);
         }
         default:
            break;
      }
      (void)out4;
      char message[64];
      snprintf(message, sizeof(message), "unsupported Engine service method %u", slot);
      if (error) *error = message;
      return false;
   }

   bool InvokeVersionService(IUnknown *unknown, unsigned slot,
                             osm32::sCpuState *state,
                             osm32::cMemory *memory,
                             std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(unknown);
      uint32_t first, second;
      cScrStr result;
      if (slot == 5)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         reinterpret_cast<void (LGAPI *)(void *, BOOL, cScrStr *)>(
            vtable[slot])(unknown, first != 0, &result);
         return WriteGuestStringObject(memory, second, result, error) &&
                Return(state, memory, 12, 0, error);
      }
      if (slot == 6)
      {
         uint32_t majorAddress, minorAddress;
         int major = 0, minor = 0;
         if (!Argument(*state, *memory, 1, &majorAddress, error) ||
             !Argument(*state, *memory, 2, &minorAddress, error)) return false;
         reinterpret_cast<void (LGAPI *)(void *, int *, int *)>(
            vtable[slot])(unknown, &major, &minor);
         return memory->Write32(majorAddress, static_cast<uint32_t>(major), error) &&
                memory->Write32(minorAddress, static_cast<uint32_t>(minor), error) &&
                Return(state, memory, 12, 0, error);
      }
      if (slot == 7)
         return Return(state, memory, 4,
            reinterpret_cast<int (LGAPI *)(void *)>(vtable[slot])(unknown),
            error);
      if (slot >= 8 && slot <= 12)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         HRESULT status = S_OK;
         if (slot <= 10)
            reinterpret_cast<void (LGAPI *)(void *, cScrStr *)>(
               vtable[slot])(unknown, &result);
         else
            status = reinterpret_cast<HRESULT (LGAPI *)(void *, cScrStr *)>(
               vtable[slot])(unknown, &result);
         return WriteGuestStringObject(memory, first, result, error) &&
                Return(state, memory, 8,
                       slot >= 11 ? static_cast<uint32_t>(status) : 0, error);
      }
      if (slot == 13 || slot == 14)
      {
         std::string path;
         if (!StringArgument(*state, memory, 1, &path, error) ||
             !Argument(*state, *memory, 2, &first, error)) return false;
         reinterpret_cast<void (LGAPI *)(void *, const char *, cScrStr *)>(
            vtable[slot])(unknown, path.c_str(), &result);
         return WriteGuestStringObject(memory, first, result, error) &&
                Return(state, memory, 12, 0, error);
      }
      if (slot == 15)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         const HRESULT status = reinterpret_cast<HRESULT (LGAPI *)(
            void *, cScrStr *, int)>(vtable[slot])(
               unknown, &result, static_cast<int32_t>(second));
         return WriteGuestStringObject(memory, first, result, error) &&
                Return(state, memory, 12, status, error);
      }
      if (error) *error = "unsupported guest Version service method";
      return false;
   }

   static bool InvokeCDService(IUnknown *service, unsigned slot,
                               osm32::sCpuState *state,
                               osm32::cMemory *memory,
                               std::string *error)
   {
      uint32_t track, flags = 0;
      if ((slot != 5 && slot != 6) ||
          !Argument(*state, *memory, 1, &track, error))
      {
         if (error && slot != 5 && slot != 6)
            *error = "unsupported CD service method";
         return false;
      }
      void **vtable = *reinterpret_cast<void ***>(service);
      if (slot == 5)
         return Return(state, memory, 8,
                       reinterpret_cast<HRESULT (LGAPI *)(void *, int)>(
                          vtable[slot])(
                             service, static_cast<int32_t>(track)), error);
      if (!Argument(*state, *memory, 2, &flags, error)) return false;
      return Return(state, memory, 12,
                    reinterpret_cast<HRESULT (LGAPI *)(
                       void *, int, uint)>(vtable[slot])(
                          service, static_cast<int32_t>(track), flags), error);
   }

   static bool InvokeKeyService(IUnknown *service, unsigned slot,
                                osm32::sCpuState *state,
                                osm32::cMemory *memory,
                                std::string *error)
   {
      uint32_t keyAddress, lockAddress, how;
      object key, lock;
      if (slot != 5 ||
          !Argument(*state, *memory, 1, &keyAddress, error) ||
          !Argument(*state, *memory, 2, &lockAddress, error) ||
          !Argument(*state, *memory, 3, &how, error) ||
          !ReadGuestObjectReference(memory, keyAddress, &key, error) ||
          !ReadGuestObjectReference(memory, lockAddress, &lock, error))
      {
         if (error && slot != 5) *error = "unsupported Key service method";
         return false;
      }
      void **vtable = *reinterpret_cast<void ***>(service);
      return Return(state, memory, 16,
                    reinterpret_cast<BOOL (LGAPI *)(
                       void *, const object *, const object *, uint32_t)>(
                          vtable[slot])(service, &key, &lock, how), error);
   }

   bool InvokeNetworkingService(IUnknown *service, unsigned slot,
                                osm32::sCpuState *state,
                                osm32::cMemory *memory,
                                std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(service);
      uint32_t args[6] = {};
      std::string message;
      if (slot == 5 || slot == 6)
      {
         const unsigned count = slot == 5 ? 4 : 4;
         for (unsigned i = 0; i < count; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         object first, second;
         cMultiParm data;
         if (!ReadGuestObjectReference(memory, args[0], &first, error))
            return false;
         if (slot == 5)
         {
            if (!ReadGuestString(memory, args[1], &message, error) ||
                !ReadGuestParm(memory, args[3], &data, error)) return false;
            typedef HRESULT (LGAPI *tMethod)(void *, const object *,
               const char *, BOOL, const cMultiParm *);
            return Return(state, memory, 20,
               reinterpret_cast<tMethod>(vtable[slot])(
                  service, &first, message.c_str(),
                  static_cast<BOOL>(args[2]), &data), error);
         }
         if (!ReadGuestObjectReference(memory, args[1], &second, error) ||
             !ReadGuestString(memory, args[2], &message, error) ||
             !ReadGuestParm(memory, args[3], &data, error)) return false;
         typedef HRESULT (LGAPI *tMethod)(void *, const object *,
            const object *, const char *, const cMultiParm *);
         return Return(state, memory, 20,
            reinterpret_cast<tMethod>(vtable[slot])(
               service, &first, &second, message.c_str(), &data), error);
      }
      if (slot == 7 || slot == 9 || (slot >= 16 && slot <= 18))
      {
         if (!Argument(*state, *memory, 1, &args[0], error)) return false;
         object target;
         if (!ReadGuestObjectReference(memory, args[0], &target, error))
            return false;
         typedef uint32_t (LGAPI *tMethod)(void *, const object *);
         return Return(state, memory, 8,
            reinterpret_cast<tMethod>(vtable[slot])(service, &target), error);
      }
      if (slot == 8)
      {
         object first, second;
         if (!Argument(*state, *memory, 1, &args[0], error) ||
             !Argument(*state, *memory, 2, &args[1], error) ||
             !ReadGuestObjectReference(memory, args[0], &first, error) ||
             !ReadGuestObjectReference(memory, args[1], &second, error))
            return false;
         typedef HRESULT (LGAPI *tMethod)(void *, const object *,
                                          const object *);
         return Return(state, memory, 12,
            reinterpret_cast<tMethod>(vtable[slot])(
               service, &first, &second), error);
      }
      if (slot == 10 || slot == 14 || slot == 15 || slot == 19)
      {
         typedef uint32_t (LGAPI *tMethod)(void *);
         return Return(state, memory, 4,
                       reinterpret_cast<tMethod>(vtable[slot])(service),
                       error);
      }
      if (slot == 11)
      {
         for (unsigned i = 0; i < 4; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         object target;
         cMultiParm data;
         float time;
         if (!ReadGuestObjectReference(memory, args[0], &target, error) ||
             !ReadGuestString(memory, args[1], &message, error) ||
             !ReadGuestParm(memory, args[3], &data, error)) return false;
         ReadFloatBits(args[2], &time);
         typedef int (LGAPI *tMethod)(void *, const object *, const char *,
                                      float, const cMultiParm *);
         return Return(state, memory, 20,
            reinterpret_cast<tMethod>(vtable[slot])(
               service, &target, message.c_str(), time, &data), error);
      }
      if (slot == 12 || slot == 13)
      {
         if (!Argument(*state, *memory, 1, &args[0], error)) return false;
         object result;
         reinterpret_cast<object *(LGAPI *)(void *, object *)>(vtable[slot])(
            service, &result);
         return ReturnObject(state, memory, args[0], 8, result, error);
      }
      if (slot == 20)
      {
         object target, result;
         if (!Argument(*state, *memory, 1, &args[0], error) ||
             !Argument(*state, *memory, 2, &args[1], error) ||
             !ReadGuestObjectReference(memory, args[1], &target, error))
            return false;
         reinterpret_cast<object *(LGAPI *)(
            void *, object *, const object *)>(vtable[slot])(
               service, &result, &target);
         return ReturnObject(state, memory, args[0], 12, result, error);
      }
      if (error) *error = "unsupported Networking service method";
      return false;
   }

   static bool InvokePGroupService(IUnknown *service, unsigned slot,
                                   osm32::sCpuState *state,
                                   osm32::cMemory *memory,
                                   std::string *error)
   {
      uint32_t objectId, active;
      if (slot != 5 ||
          !Argument(*state, *memory, 1, &objectId, error) ||
          !Argument(*state, *memory, 2, &active, error))
      {
         if (error && slot != 5) *error = "unsupported PGroup service method";
         return false;
      }
      void **vtable = *reinterpret_cast<void ***>(service);
      return Return(state, memory, 12,
                    reinterpret_cast<HRESULT (LGAPI *)(
                       void *, ObjID, BOOL)>(vtable[slot])(
                          service, static_cast<ObjID>(objectId),
                          static_cast<BOOL>(active)), error);
   }

   static bool InvokePuppetService(IUnknown *service, unsigned slot,
                                   osm32::sCpuState *state,
                                   osm32::cMemory *memory,
                                   std::string *error)
   {
      uint32_t resultAddress, objectId;
      std::string name;
      if (slot != 5 ||
          !Argument(*state, *memory, 1, &resultAddress, error) ||
          !Argument(*state, *memory, 2, &objectId, error) ||
          !StringArgument(*state, memory, 3, &name, error))
      {
         if (error && slot != 5) *error = "unsupported Puppet service method";
         return false;
      }
      void **vtable = *reinterpret_cast<void ***>(service);
      boolean result;
      reinterpret_cast<boolean *(LGAPI *)(
         void *, boolean *, object, const char *)>(vtable[slot])(
            service, &result, object(static_cast<int32_t>(objectId)),
            name.c_str());
      return ReturnBoolean(state, memory, resultAddress, 16, result, error);
   }

   static bool InvokeNullService(IUnknown *, unsigned slot,
                                 osm32::sCpuState *state,
                                 osm32::cMemory *memory,
                                 std::string *error)
   {
      if (slot < 5 || slot > 24)
      {
         if (error) *error = "unsupported Null service method";
         return false;
      }
      return Return(state, memory, 4, 0, error);
   }

   static bool InvokeBowService(IUnknown *unknown, unsigned slot,
                                osm32::sCpuState *state,
                                osm32::cMemory *memory,
                                std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(unknown);
      if (slot >= 5 && slot <= 10)
      {
         typedef uint32_t (LGAPI *tMethod)(void *);
         const uint32_t result =
            reinterpret_cast<tMethod>(vtable[slot])(unknown);
         return Return(state, memory, 4, result, error);
      }
      if (slot == 11)
      {
         uint32_t arrow;
         if (!Argument(*state, *memory, 1, &arrow, error)) return false;
         return Return(state, memory, 8,
                       reinterpret_cast<BOOL (LGAPI *)(void *, object)>(
                          vtable[slot])(
                             unknown, object(static_cast<int32_t>(arrow))),
                       error);
      }
      if (error) *error = "unsupported Bow service method";
      return false;
   }

   static bool InvokeCameraService(IUnknown *unknown, unsigned slot,
                                   osm32::sCpuState *state,
                                   osm32::cMemory *memory,
                                   std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(unknown);
      if (slot == 8)
         return Return(state, memory, 4,
                       reinterpret_cast<HRESULT (LGAPI *)(void *)>(
                          vtable[slot])(unknown), error);
      uint32_t objectId;
      if (slot < 5 || slot > 7 ||
          !Argument(*state, *memory, 1, &objectId, error))
      {
         if (error && (slot < 5 || slot > 7))
            *error = "unsupported Camera service method";
         return false;
      }
      typedef HRESULT (LGAPI *tMethod)(void *, object);
      const HRESULT result = reinterpret_cast<tMethod>(vtable[slot])(
         unknown, object(static_cast<int32_t>(objectId)));
      return Return(state, memory, 8, result, error);
   }

   static bool InvokeContainerService(IUnknown *unknown, unsigned slot,
                                      osm32::sCpuState *state,
                                      osm32::cMemory *memory,
                                      std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(unknown);
      uint32_t args[4] = {};
      unsigned count = 0;
      if (slot == 5) count = 4;
      else if (slot == 7) count = 3;
      else if (slot == 6 || slot == 8 || slot == 9) count = 2;
      else
      {
         if (error) *error = "unsupported Container service method";
         return false;
      }
      for (unsigned i = 0; i < count; ++i)
         if (!Argument(*state, *memory, i + 1, &args[i], error)) return false;
      uint32_t result;
      if (slot == 5)
      {
         typedef HRESULT (LGAPI *tMethod)(void *, object, object, int, int);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])),
            static_cast<int32_t>(args[2]), static_cast<int32_t>(args[3]));
      }
      else if (slot == 6)
      {
         typedef HRESULT (LGAPI *tMethod)(void *, object, object);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])));
      }
      else if (slot == 7)
      {
         typedef HRESULT (LGAPI *tMethod)(void *, object, object, int);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])),
            static_cast<int32_t>(args[2]));
      }
      else if (slot == 8)
      {
         typedef HRESULT (LGAPI *tMethod)(void *, object, int);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            static_cast<int32_t>(args[1]));
      }
      else
      {
         typedef uint32_t (LGAPI *tMethod)(void *, object, object);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])));
      }
      return Return(state, memory, 4 + count * 4, result, error);
   }

   static bool InvokeDamageService(IUnknown *unknown, unsigned slot,
                                   osm32::sCpuState *state,
                                   osm32::cMemory *memory,
                                   std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(unknown);
      uint32_t args[4] = {};
      const unsigned count = slot == 5 ? 4 : 2;
      if (slot < 5 || slot > 7)
      {
         if (error) *error = "unsupported Damage service method";
         return false;
      }
      for (unsigned i = 0; i < count; ++i)
         if (!Argument(*state, *memory, i + 1, &args[i], error)) return false;
      HRESULT result;
      if (slot == 5)
      {
         typedef HRESULT (LGAPI *tMethod)(void *, object, object,
                                          integer, integer);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])),
            integer(static_cast<int32_t>(args[2])),
            integer(static_cast<int32_t>(args[3])));
      }
      else if (slot == 6)
      {
         typedef HRESULT (LGAPI *tMethod)(void *, object, object);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])));
      }
      else
      {
         typedef HRESULT (LGAPI *tMethod)(void *, object, object);
         result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(args[0])),
            object(static_cast<int32_t>(args[1])));
      }
      return Return(state, memory, 4 + count * 4, result, error);
   }

   static bool InvokePowerupsService(IUnknown *unknown, unsigned slot,
                                     osm32::sCpuState *state,
                                     osm32::cMemory *memory,
                                     std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(unknown);
      uint32_t first, second;
      if (!Argument(*state, *memory, 1, &first, error)) return false;
      if (slot == 5)
      {
         reinterpret_cast<void (LGAPI *)(void *, object)>(vtable[slot])(
            unknown, object(static_cast<int32_t>(first)));
         return Return(state, memory, 8, 0, error);
      }
      if (!Argument(*state, *memory, 2, &second, error)) return false;
      if (slot == 6)
         return Return(state, memory, 12,
                       reinterpret_cast<BOOL (LGAPI *)(void *, object,
                                                       object)>(vtable[slot])(
                          unknown, object(static_cast<int32_t>(first)),
                          object(static_cast<int32_t>(second))), error);
      if (slot == 7)
      {
         float radius;
         ReadFloatBits(second, &radius);
         reinterpret_cast<void (LGAPI *)(void *, object, float)>(
            vtable[slot])(
               unknown, object(static_cast<int32_t>(first)), radius);
         return Return(state, memory, 12, 0, error);
      }
      if (error) *error = "unsupported DrkPowerups service method";
      return false;
   }

   bool InvokeDarkUIService(IUnknown *unknown, unsigned slot,
                            osm32::sCpuState *state,
                            osm32::cMemory *memory,
                            std::string *error)
   {
      IDarkUIScriptService *service =
         reinterpret_cast<IDarkUIScriptService *>(unknown);
      uint32_t first, second, third;
      std::string text, other;
      if (slot == 5)
      {
         if (!StringArgument(*state, memory, 1, &text, error) ||
             !Argument(*state, *memory, 2, &second, error) ||
             !Argument(*state, *memory, 3, &third, error)) return false;
         return Return(state, memory, 16,
                       service->TextMessage(text.c_str(),
                          static_cast<int32_t>(second),
                          static_cast<int32_t>(third)), error);
      }
      if (slot == 6)
      {
         if (!StringArgument(*state, memory, 1, &text, error) ||
             !StringArgument(*state, memory, 2, &other, error)) return false;
         return Return(state, memory, 12,
                       service->ReadBook(text.c_str(), other.c_str()), error);
      }
      if (slot == 7 || slot == 8)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         const object result = slot == 7 ? service->InvItem() :
                                           service->InvWeapon();
         return ReturnObject(state, memory, first, 8,
                             result, error);
      }
      if (slot == 9)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         return Return(state, memory, 8,
                       service->InvSelect(
                          object(static_cast<int32_t>(first))),
                       error);
      }
      if (slot == 10 || slot == 11)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !ScriptStringArgument(*state, memory, 2, &text, error))
            return false;
         const cScrStr command(text.c_str());
         if (slot == 10)
         {
            const boolean result = service->IsCommandBound(command);
            return ReturnBoolean(state, memory, first, 12, result, error);
         }
         const cScrStr result = service->DescribeKeyBinding(command);
         return WriteGuestStringObject(memory, first, result, error) &&
                Return(state, memory, 12, first, error);
      }
      if (error) *error = "unsupported DarkUI service method";
      return false;
   }

   static bool InvokeDarkGameService(IUnknown *service,
                                     unsigned slot,
                                     osm32::sCpuState *state,
                                     osm32::cMemory *memory,
                                     std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(service);
      uint32_t first, second;
      std::string name;
      if (slot == 5)
         return Return(state, memory, 4,
                       reinterpret_cast<HRESULT (LGAPI *)(void *)>(
                          vtable[slot])(service), error);
      if (slot == 6)
         return Return(state, memory, 4,
                       reinterpret_cast<HRESULT (LGAPI *)(void *)>(
                          vtable[slot])(service), error);
      if (slot == 7)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         float time;
         ReadFloatBits(first, &time);
         return Return(state, memory, 8,
                       reinterpret_cast<HRESULT (LGAPI *)(void *, float)>(
                          vtable[slot])(service, time), error);
      }
      if (slot == 8)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         return Return(state, memory, 8,
                       reinterpret_cast<HRESULT (LGAPI *)(void *, ObjID)>(
                          vtable[slot])(
                             service, static_cast<ObjID>(first)), error);
      }
      if (slot >= 9 && slot <= 12)
      {
         if (!StringArgument(*state, memory, 1, &name, error)) return false;
         if (slot == 9)
            return Return(state, memory, 8,
                          reinterpret_cast<BOOL (LGAPI *)(
                             void *, const char *)>(vtable[slot])(
                                service, name.c_str()), error);
         if (slot == 12)
            return ReturnFloat(state, memory, 8,
                               reinterpret_cast<float (LGAPI *)(
                                  void *, const char *)>(vtable[slot])(
                                     service, name.c_str()), error);
         if (!Argument(*state, *memory, 2, &second, error)) return false;
         if (slot == 10)
         {
            int value = 0;
            const BOOL result = reinterpret_cast<BOOL (LGAPI *)(
               void *, const char *, int *)>(vtable[slot])(
                  service, name.c_str(), &value);
            return memory->Write32(second, static_cast<uint32_t>(value),
                                   error) &&
                   Return(state, memory, 12, result, error);
         }
         float value = 0.0f;
         const BOOL result = reinterpret_cast<BOOL (LGAPI *)(
            void *, const char *, float *)>(vtable[slot])(
               service, name.c_str(), &value);
         uint32_t bits;
         memcpy(&bits, &value, sizeof(bits));
         return memory->Write32(second, bits, error) &&
                Return(state, memory, 12, result, error);
      }
      if (slot == 13 || slot == 14)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         if (slot == 13)
            return Return(state, memory, 12,
               reinterpret_cast<BOOL (LGAPI *)(void *, int, int)>(
                  vtable[slot])(
                     service, static_cast<int32_t>(first),
                     static_cast<int32_t>(second)),
               error);
         return Return(state, memory, 12,
            reinterpret_cast<HRESULT (LGAPI *)(void *, int, int)>(
               vtable[slot])(
                  service, static_cast<int32_t>(first),
                  static_cast<int32_t>(second)),
            error);
      }
      if (error) *error = "unsupported DarkGame service method";
      return false;
   }

   bool InvokePhysicsService(IUnknown *unknown, unsigned slot,
                                    osm32::sCpuState *state,
                                    osm32::cMemory *memory,
                                    std::string *error)
   {
      unsigned nativeSlot;
      if (!osm32::ResolveServiceMethod(m_guestAbi,
                                       osm32::kAbiServicePhysics,
                                       slot, &nativeSlot))
      {
         if (error) *error = "Physics method is absent from the guest ABI";
         return false;
      }
      slot = nativeSlot;
      void **vtable = *reinterpret_cast<void ***>(unknown);
      uint32_t first, second, third, fourth, fifth;
      if (slot == 5 || slot == 6)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         typedef HRESULT (LGAPI *tMethod)(void *, object, int);
         const HRESULT result = reinterpret_cast<tMethod>(vtable[slot])(
            unknown, object(static_cast<int32_t>(first)),
            static_cast<int32_t>(second));
         return Return(state, memory, 12, result, error);
      }
      if (slot == 7)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error) ||
             !Argument(*state, *memory, 3, &third, error) ||
             !Argument(*state, *memory, 4, &fourth, error) ||
             !Argument(*state, *memory, 5, &fifth, error)) return false;
         // Re-read the vector address separately; the fifth guest argument
         // is flags and the sixth is the vector reference.
         uint32_t vectorAddress;
         if (!Argument(*state, *memory, 6, &vectorAddress, error))
            return false;
         mxs_vector velocity;
         float power;
         ReadFloatBits(fourth, &power);
         if (!ReadGuestVector(memory, vectorAddress, &velocity, error))
            return false;
         const cScrVec nativeVelocity(velocity);
         object result;
         reinterpret_cast<object *(LGAPI *)(
            void *, object *, object, object, float, integer,
            const cScrVec *)>(vtable[slot])(
               unknown, &result, object(static_cast<int32_t>(second)),
               object(static_cast<int32_t>(third)), power,
               integer(static_cast<int32_t>(fifth)), &nativeVelocity);
         return ReturnObject(state, memory, first, 28, result, error);
      }
      if (slot >= 8 && slot <= 10)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         mxs_vector velocityVector;
         if (slot != 9 &&
             !ReadGuestVector(memory, second, &velocityVector, error))
            return false;
         HRESULT result;
         if (slot == 8)
         {
            const cScrVec nativeVector(velocityVector);
            result = reinterpret_cast<HRESULT (LGAPI *)(
               void *, object, const cScrVec *)>(vtable[slot])(
                  unknown, object(static_cast<int32_t>(first)),
                  &nativeVector);
         }
         else if (slot == 9)
         {
            cScrVec output;
            result = reinterpret_cast<HRESULT (LGAPI *)(
               void *, object, cScrVec *)>(vtable[slot])(
                  unknown, object(static_cast<int32_t>(first)), &output);
            if (SUCCEEDED(result) &&
                !WriteGuestVector(memory, second, output, error)) return false;
         }
         else
         {
            const cScrVec nativeVector(velocityVector);
            result = reinterpret_cast<HRESULT (LGAPI *)(
               void *, object, const cScrVec *)>(vtable[slot])(
                  unknown, object(static_cast<int32_t>(first)),
                  &nativeVector);
         }
         return Return(state, memory, 12, result, error);
      }
      if (slot == 12)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         float gravity;
         ReadFloatBits(second, &gravity);
         return Return(state, memory, 12,
                       reinterpret_cast<HRESULT (LGAPI *)(
                          void *, object, float)>(vtable[slot])(
                             unknown, object(static_cast<int32_t>(first)),
                             gravity),
                       error);
      }
      if (slot == 13)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         return ReturnFloat(state, memory, 8,
                            reinterpret_cast<float (LGAPI *)(
                               void *, object)>(vtable[slot])(
                                  unknown,
                                  object(static_cast<int32_t>(first))),
                            error);
      }
      if (slot >= 14 && slot <= 16)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         const object target(static_cast<int32_t>(first));
         const BOOL result = reinterpret_cast<BOOL (LGAPI *)(void *, object)>(
            vtable[slot])(unknown, target);
         return Return(state, memory, 8, result, error);
      }
      if (slot >= 11 && slot <= 20 && slot != 12 && slot != 13 &&
          slot != 14 && slot != 15 && slot != 16)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         const object target(static_cast<int32_t>(first));
         const HRESULT result = reinterpret_cast<HRESULT (LGAPI *)(
            void *, object)>(vtable[slot])(unknown, target);
         return Return(state, memory, 8, result, error);
      }
      if (slot == 21)
      {
         if (!Argument(*state, *memory, 1, &first, error) ||
             !Argument(*state, *memory, 2, &second, error)) return false;
         mxs_vector offset;
         if (!ReadGuestVector(memory, second, &offset, error)) return false;
         cScrVec nativeOffset(offset);
         reinterpret_cast<void (LGAPI *)(void *, int, cScrVec *)>(
            vtable[slot])(
               unknown, static_cast<int32_t>(first), &nativeOffset);
         return WriteGuestVector(memory, second, nativeOffset, error) &&
                Return(state, memory, 12, 0, error);
      }
      if (slot == 22 || slot == 23)
      {
         if (!Argument(*state, *memory, 1, &first, error)) return false;
         const object target(static_cast<int32_t>(first));
         return Return(state, memory, 8,
                       slot == 22 ?
                          reinterpret_cast<HRESULT (LGAPI *)(void *, object)>(
                             vtable[slot])(unknown, target) :
                          reinterpret_cast<BOOL (LGAPI *)(void *, object)>(
                             vtable[slot])(unknown, target), error);
      }
      if (error) *error = "unsupported Physics service method";
      return false;
   }

   static bool InvokeOneStringService(IUnknown *service, unsigned slot,
                                      unsigned supportedSlot,
                                      osm32::sCpuState *state,
                                      osm32::cMemory *memory,
                                      std::string *error)
   {
      if (slot != supportedSlot)
      {
         if (error) *error = "unsupported native service method";
         return false;
      }
      std::string text;
      if (!StringArgument(*state, memory, 1, &text, error)) return false;
      void **vtable = *reinterpret_cast<void ***>(service);
      typedef uint32_t (LGAPI *tMethod)(void *, const char *);
      const uint32_t result = reinterpret_cast<tMethod>(vtable[slot])(
         service, text.c_str());
      return Return(state, memory, 8, result, error);
   }

   bool InvokeSoundService(IUnknown *unknown, unsigned slot,
                           osm32::sCpuState *state,
                           osm32::cMemory *memory,
                           std::string *error)
   {
      ISoundScriptService *service =
         reinterpret_cast<ISoundScriptService *>(unknown);
      unsigned nativeSlot;
      if (!osm32::ResolveServiceMethod(m_guestAbi, osm32::kAbiServiceSound,
                                       slot, &nativeSlot))
      {
         if (error) *error = "Sound method is absent from the guest ABI";
         return false;
      }
      slot = nativeSlot;
      const bool hasNetwork =
         osm32::GuestSoundHasNetworkArgument(m_guestAbi);
      uint32_t args[8] = {};
      std::string text;
      if (slot >= 5 && slot <= 8)
      {
         const unsigned count = (slot == 6 || slot == 7) ?
            (hasNetwork ? 6 : 5) : (hasNetwork ? 5 : 4);
         for (unsigned i = 0; i < count; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         if (!ScriptStringArgument(*state, memory, 3, &text, error))
            return false;
         const object callback(static_cast<int32_t>(args[1]));
         const cScrStr name(text.c_str());
         boolean result;
         if (slot == 5)
            result = service->Play(callback, name,
               static_cast<eSoundSpecial>(args[3]),
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[4]));
         else if (slot == 6)
            result = service->Play(callback, name,
               object(static_cast<int32_t>(args[3])),
               static_cast<eSoundSpecial>(args[4]),
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[5]));
         else if (slot == 7)
         {
            mxs_vector position;
            if (!ReadGuestVector(memory, args[3], &position, error))
               return false;
            cScrVec nativePosition(position);
            result = service->Play(callback, name, nativePosition,
               static_cast<eSoundSpecial>(args[4]),
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[5]));
         }
         else
            result = service->PlayAmbient(callback, name,
               static_cast<eSoundSpecial>(args[3]),
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[4]));
         return ReturnBoolean(state, memory, args[0], 4 + count * 4,
                              result, error);
      }
      if (slot >= 9 && slot <= 12)
      {
         const unsigned count = (slot == 10 || slot == 11) ?
            (hasNetwork ? 5 : 4) : (hasNetwork ? 4 : 3);
         for (unsigned i = 0; i < count; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         const object callback(static_cast<int32_t>(args[1]));
         const object schema(static_cast<int32_t>(args[2]));
         boolean result;
         if (slot == 9)
            result = service->PlaySchema(
               callback, schema,
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[3]));
         else if (slot == 10)
            result = service->PlaySchema(
               callback, schema, object(static_cast<int32_t>(args[3])),
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[4]));
         else if (slot == 11)
         {
            mxs_vector position;
            if (!ReadGuestVector(memory, args[3], &position, error))
               return false;
            cScrVec nativePosition(position);
            result = service->PlaySchema(
               callback, schema, nativePosition,
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[4]));
         }
         else
            result = service->PlaySchemaAmbient(
               callback, schema,
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[3]));
         return ReturnBoolean(state, memory, args[0], 4 + count * 4,
                              result, error);
      }
      if (slot == 13)
      {
         const unsigned count = hasNetwork ? 7 : 6;
         for (unsigned i = 0; i < count; ++i)
            if (!Argument(*state, *memory, i + 1, &args[i], error))
               return false;
         if (!ScriptStringArgument(*state, memory, 3, &text, error))
            return false;
         return ReturnBoolean(state, memory, args[0], 4 + count * 4,
            service->PlayEnvSchema(
               object(static_cast<int32_t>(args[1])),
               cScrStr(text.c_str()),
               object(static_cast<int32_t>(args[3])),
               object(static_cast<int32_t>(args[4])),
               static_cast<eEnvSoundLoc>(args[5]),
               !hasNetwork ? kSoundNetDefault :
                        static_cast<eSoundNetwork>(args[6])), error);
      }
      if (slot == 14)
      {
         if (!Argument(*state, *memory, 1, &args[0], error) ||
             !Argument(*state, *memory, 2, &args[1], error) ||
             !Argument(*state, *memory, 3, &args[2], error)) return false;
         return ReturnBoolean(state, memory, args[0], 16,
            service->PlayVoiceOver(
               object(static_cast<int32_t>(args[1])),
               object(static_cast<int32_t>(args[2]))), error);
      }
      if (slot == 15 || slot == 16)
      {
         const unsigned offset = slot == 16 ? 1 : 0;
         if ((offset && !Argument(*state, *memory, 1, &args[0], error)) ||
             !Argument(*state, *memory, 1 + offset, &args[1], error) ||
             !ScriptStringArgument(*state, memory, 2 + offset, &text,
                                   error) ||
             !Argument(*state, *memory, 3 + offset, &args[2], error))
            return false;
         if (slot == 15)
            return Return(state, memory, 16,
               service->Halt(object(static_cast<int32_t>(args[1])),
                             cScrStr(text.c_str()),
                             object(static_cast<int32_t>(args[2]))), error);
         return ReturnBoolean(state, memory, args[0], 20,
            service->HaltSchema(object(static_cast<int32_t>(args[1])),
                                cScrStr(text.c_str()),
                                object(static_cast<int32_t>(args[2]))), error);
      }
      if (slot == 17)
      {
         if (!Argument(*state, *memory, 1, &args[0], error)) return false;
         return Return(state, memory, 8,
                       service->HaltSpeech(
                          object(static_cast<int32_t>(args[0]))), error);
      }
      if (slot == 18)
      {
         if (!Argument(*state, *memory, 1, &args[0], error) ||
             !ScriptStringArgument(*state, memory, 2, &text, error))
            return false;
         return ReturnBoolean(state, memory, args[0], 12,
                              service->PreLoad(cScrStr(text.c_str())), error);
      }
      if (error) *error = "unsupported Sound service method";
      return false;
   }

   bool InvokeDebugService(IUnknown *service, unsigned slot,
                           osm32::sCpuState *state,
                           osm32::cMemory *memory,
                           std::string *error)
   {
      void **vtable = *reinterpret_cast<void ***>(service);
      if (slot == 7)
      {
         typedef HRESULT (LGAPI *tBreak)(void *);
         return Return(state, memory, 4,
                       reinterpret_cast<tBreak>(vtable[slot])(service),
                       error);
      }
      if (slot != 5 && slot != 6 && slot != 8)
      {
         if (error) *error = "unsupported Debug service method";
         return false;
      }
      // (8: NewDark's Log, MPrint's arguments, to the log file -- here,
      // MPrint's monolog)
      const unsigned nativeSlot = slot == 8 ? 5 : slot;
      std::string values[8];
      for (unsigned i = 0; i < 8; ++i)
         if (!ScriptStringArgument(*state, memory, i + 1, &values[i],
                                   error))
            return false;
      cScrStr strings[8] = {
         cScrStr(values[0].c_str()), cScrStr(values[1].c_str()),
         cScrStr(values[2].c_str()), cScrStr(values[3].c_str()),
         cScrStr(values[4].c_str()), cScrStr(values[5].c_str()),
         cScrStr(values[6].c_str()), cScrStr(values[7].c_str())
      };
      typedef HRESULT (LGAPI *tPrint)(void *, const cScrStr *,
         const cScrStr *, const cScrStr *, const cScrStr *,
         const cScrStr *, const cScrStr *, const cScrStr *,
         const cScrStr *);
      const HRESULT result = reinterpret_cast<tPrint>(vtable[nativeSlot])(
         service, &strings[0], &strings[1], &strings[2], &strings[3],
         &strings[4], &strings[5], &strings[6], &strings[7]);
      return Return(state, memory, 36, result, error);
   }

   struct sLinkQueryProxy
   {
      uint32_t object;
      uint64_t serial;         // creation order, for message checkpoints
      ILinkQuery *query;
      uint32_t references;
      uint32_t data;           // the current link's data, copied out (Data)
      uint32_t dataCapacity;
   };

   IScriptMan *m_scriptManager;
   const osm32::eGuestAbi m_guestAbi;
   uint32_t m_linkQueryVtable;
   osm32::cModule *m_runtime;   // the guest, to call its listeners
   uint64_t m_linkQuerySerial;
   std::map<uint32_t, sLinkQueryProxy> m_linkQueries;   // by guest object
};

class cOsm32Module;
std::vector<cOsm32Module *> g_Modules;

class cOsm32Script : public cCTUnaggregated<IScript, &IID_IScript,
                                             kCTU_Default>
{
public:
   cOsm32Script(cOsm32Module *module, uint32_t guest,
                const char *className);
   virtual ~cOsm32Script();

   STDMETHOD_(const char *, GetClassName)();
   STDMETHOD(ReceiveMessage)(sScrMsg *message, sMultiParm *reply,
                             eScrTraceAction debugAction);

private:
   cOsm32Module *m_module;
   uint32_t m_guest;
   std::string m_className;
};

class cOsm32Module : public cCTUnaggregated<IScriptModule,
                                             &IID_IScriptModule,
                                             kCTU_Default>
{
public:
   cOsm32Module(IScriptMan *scriptManager, osm32::eGuestAbi guestAbi)
    : m_environment(scriptManager, guestAbi)
   {
      m_environment.SetRuntime(&m_runtime);
   }
   virtual ~cOsm32Module()
   {
      m_environment.DropListeners();
      g_Modules.erase(std::remove(g_Modules.begin(), g_Modules.end(), this),
                      g_Modules.end());
   }

   bool Load(const char *path, const char *moduleName, std::string *error)
   {
      if (getenv("DARK_OSM32_TRACE"))
      {
         fprintf(stderr, "OSM32 load '%s' from '%s' ABI=%s\n",
                 moduleName ? moduleName : "", path ? path : "",
                 osm32::GuestAbiName(m_environment.GuestAbi()));
         fflush(stderr);
      }
      if (!m_runtime.Load(path, moduleName, &m_environment, error))
         return false;
      const std::vector<osm32::sGuestClass> &guest = m_runtime.Classes();
      m_classes.resize(guest.size());
      for (size_t i = 0; i < guest.size(); ++i)
      {
         m_classes[i].module = guest[i].module;
         m_classes[i].name = guest[i].name;
         m_classes[i].base = guest[i].base;
         m_classes[i].guestIndex = i;
      }
      // Populate pointers only after the vector has reached its final size.
      for (size_t i = 0; i < m_classes.size(); ++i)
      {
         m_classes[i].descriptor.pszModule = m_classes[i].module.c_str();
         m_classes[i].descriptor.pszClass = m_classes[i].name.c_str();
         m_classes[i].descriptor.pszBaseClass = m_classes[i].base.c_str();
         m_classes[i].descriptor.pfnFactory = 0;
      }
      g_Modules.push_back(this);
      if (getenv("DARK_OSM32_TRACE"))
      {
         fprintf(stderr, "OSM32 loaded '%s' classes=%u\n",
                 m_runtime.Name().c_str(),
                 static_cast<unsigned>(m_classes.size()));
         fflush(stderr);
      }
      return true;
   }

   STDMETHOD_(const char *, GetName)()
   {
      return m_runtime.Name().c_str();
   }

   STDMETHOD_(const sScrClassDesc *, GetFirstClass)(tScrIter *iterator)
   {
      *iterator = reinterpret_cast<tScrIter>(static_cast<uintptr_t>(0));
      return m_classes.empty() ? 0 : &m_classes[0].descriptor;
   }

   STDMETHOD_(const sScrClassDesc *, GetNextClass)(tScrIter *iterator)
   {
      const uintptr_t next = reinterpret_cast<uintptr_t>(*iterator) + 1;
      *iterator = reinterpret_cast<tScrIter>(next);
      return next < m_classes.size() ? &m_classes[next].descriptor : 0;
   }

   STDMETHOD_(void, EndClassIter)(tScrIter *)
   {
   }

   bool Owns(const sScrClassDesc *descriptor, size_t *index) const
   {
      for (size_t i = 0; i < m_classes.size(); ++i)
         if (&m_classes[i].descriptor == descriptor)
         {
            if (index) *index = m_classes[i].guestIndex;
            return true;
         }
      return false;
   }

   IScript *Create(size_t index, const char *className, ObjID objectId)
   {
      if (index >= m_runtime.Classes().size())
         return 0;
      uint32_t guest;
      std::string error;
      if (getenv("DARK_OSM32_TRACE"))
      {
         fprintf(stderr, "OSM32 '%s' create '%s' object=%d\n",
                 m_runtime.Name().c_str(), className ? className : "",
                 objectId);
         fflush(stderr);
      }
      if (!m_runtime.CreateScript(m_runtime.Classes()[index], objectId,
                                  &guest, &error))
      {
         mprintf("OSM32 '%s' could not create '%s' on object %d: %s\n",
                 m_runtime.Name().c_str(), className ? className : "",
                 objectId, error.empty() ? "unknown guest failure" :
                                           error.c_str());
         if (getenv("DARK_OSM32_TRACE"))
         {
            fprintf(stderr,
                    "OSM32 '%s' could not create '%s' on object %d: %s\n",
                    m_runtime.Name().c_str(), className ? className : "",
                    objectId, error.empty() ? "unknown guest failure" :
                                              error.c_str());
            fflush(stderr);
         }
         return 0;
      }
      return new cOsm32Script(this, guest, className);
   }

   bool ReleaseGuest(uint32_t guest)
   {
      uint32_t ignored;
      std::string error;
      return m_runtime.CallCom(guest, 2, 0, 0, &ignored, &error);
   }

   HRESULT ReceiveMessage(uint32_t guest, sScrMsg *message,
                          sMultiParm *reply,
                          eScrTraceAction debugAction)
   {
      if (!message)
         return E_POINTER;

      osm32::sMessage guestMessage;
      guestMessage.from = message->from;
      guestMessage.to = message->to;
      guestMessage.name = message->message ? message->message : "";
      guestMessage.time = static_cast<uint32_t>(message->time);
      guestMessage.flags = message->flags;
      if (getenv("DARK_OSM32_TRACE"))
      {
         fprintf(stderr, "OSM32 '%s' begin message '%s' to=%d\n",
                 m_runtime.Name().c_str(), guestMessage.name.c_str(),
                 guestMessage.to);
         fflush(stderr);
      }
      MarshalMessageExtension(message, &guestMessage);
      if (!ToGuestValue(message->data, &guestMessage.data[0]) ||
          !ToGuestValue(message->data2, &guestMessage.data[1]) ||
          !ToGuestValue(message->data3, &guestMessage.data[2]))
         return E_INVALIDARG;

      osm32::sValue guestReply;
      if (reply && !ToGuestValue(*reply, &guestReply))
         return E_INVALIDARG;
      uint32_t result;
      std::string error;
      const uint64_t linkQueryCheckpoint = m_environment.LinkQueryCheckpoint();
      const bool received = m_runtime.ReceiveMessage(
         guest, guestMessage, reply ? &guestReply : 0,
         static_cast<uint32_t>(debugAction), &result, &error);
      if (!received)
         m_environment.ReleaseLinkQueriesCreatedSince(linkQueryCheckpoint);
      if (!received || (reply && !FromGuestValue(guestReply, reply)))
      {
         if (std::find(m_reportedErrors.begin(), m_reportedErrors.end(),
                       error) == m_reportedErrors.end())
         {
            m_reportedErrors.push_back(error);
            mprintf("OSM32 '%s' message '%s' failed: %s\n",
                    m_runtime.Name().c_str(), guestMessage.name.c_str(),
                    error.empty() ? "reply conversion failed" : error.c_str());
            fprintf(stderr, "OSM32 '%s' message '%s' failed: %s\n",
                    m_runtime.Name().c_str(), guestMessage.name.c_str(),
                    error.empty() ? "reply conversion failed" : error.c_str());
         }
         return E_FAIL;
      }
      if (getenv("DARK_OSM32_TRACE"))
      {
         fprintf(stderr, "OSM32 '%s' end message '%s' result=%08x\n",
                 m_runtime.Name().c_str(), guestMessage.name.c_str(), result);
         fflush(stderr);
      }
      return static_cast<HRESULT>(result);
   }

private:
   struct sClass
   {
      std::string module;
      std::string name;
      std::string base;
      sScrClassDesc descriptor;
      size_t guestIndex;
   };

   // Declaration order makes the runtime unload before its environment
   // (whose listeners are dropped first: the module's destructor).
   cOsm32Environment m_environment;
   osm32::cModule m_runtime;
   std::vector<sClass> m_classes;
   std::vector<std::string> m_reportedErrors;
};

cOsm32Script::cOsm32Script(cOsm32Module *module, uint32_t guest,
                           const char *className)
 : m_module(module),
   m_guest(guest),
   m_className(className ? className : "")
{
   m_module->AddRef();
}

cOsm32Script::~cOsm32Script()
{
   m_module->ReleaseGuest(m_guest);
   m_module->Release();
}

STDMETHODIMP_(const char *) cOsm32Script::GetClassName()
{
   return m_className.c_str();
}

STDMETHODIMP cOsm32Script::ReceiveMessage(sScrMsg *message,
                                          sMultiParm *reply,
                                          eScrTraceAction debugAction)
{
   return m_module->ReceiveMessage(m_guest, message, reply, debugAction);
}

} // namespace

BOOL LoadOsm32Module(const char *path, const char *moduleName,
                     IScriptMan *scriptManager, osm32::eGuestAbi guestAbi,
                     IScriptModule **module, std::string *error)
{
   if (!module)
      return FALSE;
   *module = 0;
   cOsm32Module *adapter = new cOsm32Module(scriptManager, guestAbi);
   if (!adapter->Load(path, moduleName, error))
   {
      adapter->Release();
      return FALSE;
   }
   *module = adapter;
   return TRUE;
}

IScript *CreateOsm32Script(const sScrClassDesc *descriptor,
                           const char *className, ObjID objectId,
                           BOOL *handled)
{
   if (handled)
      *handled = FALSE;
   for (size_t i = 0; i < g_Modules.size(); ++i)
   {
      size_t classIndex;
      if (g_Modules[i]->Owns(descriptor, &classIndex))
      {
         if (handled) *handled = TRUE;
         return g_Modules[i]->Create(classIndex, className, objectId);
      }
   }
   return 0;
}
