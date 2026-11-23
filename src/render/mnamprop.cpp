/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

// $Header: r:/t2repos/thief2/src/render/mnamprop.cpp,v 1.13 1998/12/07 16:50:43 dc Exp $

//
// Model name property
//

// (the C++ library first, at its own packing: the engine's headers pack
// tighter, and a map laid out so here broke the one in rt_materials.h)
#pragma pack(push, 8)
#include <string>
#include <unordered_map>
#pragma pack(pop)

#include <mnamprop.h>
#include <mnumprop.h>

#include <property.h>
#include <propface.h>
#include <propbase.h>
#include <propfac_.h>

#include <osysbase.h>

#include <appagg.h>

#include <traitman.h>
#include <traitbas.h>
#include <objquery.h>

#include <objmodel.h>
#include <mprintf.h>

#include <report.h>

#ifdef REPORT

#include <dlist.h>
#include <iobjsys.h>
#include <reputil.h>

#endif

#include <propert_.h>
#include <dataops_.h>
#include <sdesc.h>
#include <sdesbase.h>
#include <wrtype.h>
#include <objpos.h>
#include <config.h>
#include <matrix.h>
#include <string.h>
#include <stdio.h>

#include <dbmem.h>

static ILabelProperty* modelnameprop = NULL;

////////////////////////////////////////////////////////////
// MODEL NAME PROPERTY CREATION 
//

#define MODELNAMEPROP_IMPL kPropertyImplDense

static sPropertyConstraint modelnameprop_const[] =
{
   { kPropertyNullConstraint, NULL }
};

static sPropertyDesc modelnameprop_desc =
{
   PROP_MODELNAME_NAME,
   0,
   modelnameprop_const,
   0,0, // verison
   { "Shape", "Model Name" }, 
};


class cModelNameProp : public cGenericLabelProperty
{
public:
   cModelNameProp() : cGenericLabelProperty(&modelnameprop_desc,MODELNAMEPROP_IMPL)
   {
      SetRebuildConcretes(TRUE); 
   }; 

   void RebuildConcrete(ObjID obj, BOOL fIsRelevant, uPropListenerValue val , ObjID donor)
   {
      if (fIsRelevant)
         ObjLoadModel(donor);
      cGenericLabelProperty::RebuildConcrete(obj,fIsRelevant,val,donor); 
   }

};

// Reporting

#ifdef REPORT

// list for sorting names
class cModelNameNode: public cDListNode<cModelNameNode,1>
{
public:
   Label m_modelName;
   ObjID m_objID;
};

class cModelNameList: public cDList<cModelNameNode,1>
{
public:
   cModelNameNode *Find(const char *pModelName) const
   {
      cModelNameNode *pNode = GetFirst();
      
      while (pNode != NULL)
      {
         if (!strcmp((const char*)(pNode->m_modelName.text), pModelName))
            return pNode;
         pNode = pNode->GetNext();
      }
      return NULL;
   }
   void InsertSorted(cModelNameNode *pInsertNode)
   {
      cModelNameNode *pNode = GetFirst();
      while (pNode != NULL)
      {
         if (stricmp((const char*)(pNode->m_modelName.text), (const char*)(pInsertNode->m_modelName.text))>0)
         {
            InsertBefore(pNode, pInsertNode);
            break;
         }
         pNode = pNode->GetNext();
      }
      if (pNode == NULL)
         Append(pInsertNode);
   }
};

// Report prints out all models used by abstract objects
static void ModelNameReport(int WarnLevel, void *data, char *buffer)
{
   IObjectQuery* pQuery;
   IObjectSystem* pOS = AppGetObj(IObjectSystem);
   cModelNameList modelNameList;
   cModelNameNode* pModelNameNode;
   cModelNameNode* pNext;
   ObjID obj;
   int count = 0;

   rsprintf(&buffer,"\nReport on Object Model Names\n");

   // okay, sift through every object in the world and collect stats
   pQuery = pOS->Iter(kObjectAbstract);
   pModelNameNode = new cModelNameNode;
   while (!pQuery->Done())
   {
      obj = pQuery->Object();
      if (ObjGetModelName(obj, (char *)(pModelNameNode->m_modelName.text)))
      {
         pModelNameNode->m_objID = obj;
         // count unique
         if (!modelNameList.Find(pModelNameNode->m_modelName.text))
            ++count;
         // add sorted
         modelNameList.InsertSorted(pModelNameNode);
         pModelNameNode = new cModelNameNode;
      }
      pQuery->Next();
   }
   delete pModelNameNode;

   // now print out
   rsprintf(&buffer, "Total models used: %d\n\n", count);
   pModelNameNode = modelNameList.GetFirst();
   while (pModelNameNode != NULL)
   {
      rsprintf(&buffer, "Model %s used by obj %s\n", pModelNameNode->m_modelName.text, pOS->GetName(pModelNameNode->m_objID));
      // move on to next, deleting as we go
      pNext = pModelNameNode->GetNext();
      delete pModelNameNode;
      pModelNameNode = pNext;
   }

   SafeRelease(pQuery);
   SafeRelease(pOS);
}
#endif

////////////////////////////////////////////////////////////
//
// Model LOD (NewDark's Shape->"Model LOD"): the model an object is drawn
// with by how far it is from the camera -- "LOD 0 Model" near (normally its
// Model Name), "LOD 1 Model" past "LOD 0->1 Dist", "LOD 2 Model" (if it has
// one) past "LOD 1->2 Dist", in feet; at least two models, or it is not used.
// cam.cfg's model_lod_dist_factor (1.0) scales the distances, model_lod_force_low
// never draws LOD 0. The Thief II HD Mod's grass clumps: 9900 vertices near,
// a quarter of that past 30 feet, a few cards past 45 -- each drawn at its
// most detailed, everywhere, kept the frame on the processor.
//

struct sModelLOD
{
   Label model[3];
   float dist01, dist12;
};

F_DECLARE_INTERFACE(IModelLODProperty);
#undef INTERFACE
#define INTERFACE IModelLODProperty
DECLARE_PROPERTY_INTERFACE(IModelLODProperty)
{
   DECLARE_UNKNOWN_PURE();
   DECLARE_PROPERTY_PURE();
   DECLARE_PROPERTY_ACCESSORS(sModelLOD*);
};
#undef INTERFACE
DEFINE_GUID_UNCONDITIONAL(IID_IModelLODProperty, MAKE_LG_GUID_HIGH_DWORD(0x6e7), (0x7a80 + 0x6e7), (0x11cf + 0x6e7),
                          0x83, 0x48, 0x00, 0xaa, 0x00, 0xa8, 0x2b, 0x51);

typedef cGenericProperty<IModelLODProperty, &IID_IModelLODProperty, sModelLOD*> cModelLODPropertyBase;

class cModelLODProperty : public cModelLODPropertyBase
{
   cClassDataOps<sModelLOD> mOps;

public:
   cModelLODProperty(const sPropertyDesc* desc, ePropertyImpl impl)
      : cModelLODPropertyBase(desc, CreateGenericPropertyStore(impl))
   {
      SetOps(&mOps);
      SetRebuildConcretes(TRUE);
   }

   // its models loaded with the mission, not as it is first drawn
   void RebuildConcrete(ObjID obj, BOOL fIsRelevant, uPropListenerValue val, ObjID donor)
   {
      sModelLOD* lod;
      if (fIsRelevant && Get(donor, &lod))
         for (int i = 0; i < 3; ++i)
            if (lod->model[i].text[0] && objmodelGetIdx(lod->model[i].text) == MIDX_INVALID)
               objmodelLoadModel(lod->model[i].text);
      cModelLODPropertyBase::RebuildConcrete(obj, fIsRelevant, val, donor);
   }

   STANDARD_DESCRIBE_TYPE(sModelLOD);
};

static sFieldDesc modellod_fields[] =
{
   { "LOD 0 Model", kFieldTypeString, FieldLocation(sModelLOD, model[0]) },
   { "LOD 1 Model", kFieldTypeString, FieldLocation(sModelLOD, model[1]) },
   { "LOD 2 Model", kFieldTypeString, FieldLocation(sModelLOD, model[2]) },
   { "LOD 0->1 Dist", kFieldTypeFloat, FieldLocation(sModelLOD, dist01) },
   { "LOD 1->2 Dist", kFieldTypeFloat, FieldLocation(sModelLOD, dist12) },
};

static sStructDesc modellod_sdesc = StructDescBuild(sModelLOD, kStructFlagNone, modellod_fields);

static sPropertyDesc modellod_desc =
{
   "ModelLOD",
   0,
   NULL,
   0, 0,
   { "Shape", "Model LOD" },
};

static cModelLODProperty* modellodprop = NULL;

extern "C" mxs_vector portal_camera_loc;

// the model to draw an object with now (its Model LOD's, else idx, its own)
extern "C" int ObjLODModelNumber(ObjID obj, int idx)
{
   sModelLOD* lod;
   if (!modellodprop || !modellodprop->Get(obj, &lod) || !lod->model[0].text[0] || !lod->model[1].text[0])
      return idx;
   static float s_factor = -1.0f;
   static BOOL s_forceLow = FALSE;
   if (s_factor < 0.0f)
   {
      s_factor = 1.0f;
      config_get_float("model_lod_dist_factor", &s_factor);
      if (s_factor <= 0.0f)
         s_factor = 1.0f;
      s_forceLow = config_is_defined("model_lod_force_low");
   }
   const ObjPos* pos = ObjPosGet(obj);
   if (!pos)
      return idx;
   const float d = mx_dist_vec((mxs_vector*)&pos->loc.vec, &portal_camera_loc);
   // (a record without its distances: NewDark's own, as near as known)
   const float d01 = (lod->dist01 > 0.0f ? lod->dist01 : 30.0f) * s_factor;
   const float d12 = (lod->dist12 > 0.0f ? lod->dist12 : 60.0f) * s_factor;
   // a little held at each change, so one at the edge does not swap back and forth
   struct sLevel { int level = 0; };   // (types of this file's own, in its containers)
   static std::unordered_map<ObjID, sLevel> s_level;
   int& was = s_level[obj].level;
   const float hold = 1.5f;
   int level = 0;
   if (lod->model[2].text[0] && d > d12 + (was >= 2 ? -hold : hold))
      level = 2;
   else if (d > d01 + (was >= 1 ? -hold : hold))
      level = 1;
   if (s_forceLow && level == 0)
      level = 1;
   was = level;
   if (level == 0)
      return idx;
   // its model's number (loaded once: a model kept by name, checked again
   // in case a mission's models were let go since)
   struct sIndex { int at = 0; };
   static std::unordered_map<std::string, sIndex> s_index;
   const char* name = lod->model[level].text;
   char key[17];
   strncpy(key, name, 16);
   key[16] = 0;
   int& at = s_index[key].at;
   const char* had = at > 0 || at == 0 ? objmodelGetName(at) : NULL;
   if (!had || stricmp(had, key) != 0)
   {
      at = objmodelGetIdx(key);
      if (at == MIDX_INVALID)
         at = objmodelLoadModel(key);
      fprintf(stderr, "Model LOD: %s, model %d (first for object %d, %.0f ft away)%s\n", key, at, obj, d,
              at == MIDX_INVALID ? " -- not found" : "");
   }
   return at == MIDX_INVALID ? idx : at;
}

// Init the property
void ModelNamePropInit(void)
{
   StructDescRegister(&modellod_sdesc);
   modellodprop = new cModelLODProperty(&modellod_desc, kPropertyImplSparseHash);
   modelnameprop = new cModelNameProp; 
   ReportRegisterGenCallback(ModelNameReport,kReportModels,"ModelName",NULL);   
}

void ModelNamePropTerm(void)
{
   SafeRelease(modelnameprop);   
   SafeRelease(modellodprop);
   ReportUnRegisterGenCallback(ModelNameReport,kReportModels,NULL);
}

// get and set functions
BOOL ObjGetModelName(ObjID obj, char *name)
{
   Assert_(modelnameprop);
   char *temp;
   BOOL retval = modelnameprop->Get(obj, (Label**)&temp);
   if (retval) // hope name long enough!
      strcpy(name,temp);
   return retval;
}

void ObjSetModelName(ObjID obj, const char *name)
{
   Assert_(modelnameprop);
   modelnameprop->Set(obj, (Label*)name);
}

