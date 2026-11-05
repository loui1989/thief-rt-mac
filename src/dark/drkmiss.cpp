/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

// $Header: r:/t2repos/thief2/src/dark/drkmiss.cpp,v 1.5 2000/02/19 12:27:22 toml Exp $
#include <drkmiss.h>
#include <filevar.h>
#include <appagg.h>
#include <sdestool.h>
#include <sdesbase.h>
#include <dbasemsg.h>

#include <tagpatch.h>
#include <dbfile.h>
#include <dbtagfil.h>
#include <contentctx.h>
#include <mprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <memall.h>
#include <dbmem.h>   // must be last header! 

//
// SDESC
//

static sFieldDesc mission_fields[] = 
{
   { "Mission Number",  kFieldTypeInt, FieldLocation(sMissionData,num) }, 
   { "Mission Pathname",  kFieldTypeString, FieldLocation(sMissionData,path) }, 

};


static sStructDesc mission_sdesc = StructDescBuild(sMissionData,kStructFlagNone,mission_fields); 

//
// VAR DESCRIPTOR
//


sFileVarDesc gMissionVarDesc = 
{
   kMissionVar,         // Where do I get saved?
   "DARKMISS",          // Tag file tag
   "Dark Mission Description",     // friendly name
   FILEVAR_TYPE(sMissionData),  // Type (for editing)
   { 1, 0},             // version
   { 1, 0},             // last valid version 
   "dark"
}; 


//
// Actually use the defaults when resetting
//

class cMissionData : public cFileVar<sMissionData,&gMissionVarDesc>
{

};

//
// The variable itself 
//
static cMissionData gMissionData; 

const sMissionData* GetMissionData()
{
   return &gMissionData; 
}

void LoadMissionData(ITagFile* file)
{
   gMissionData.DatabaseMsg(kDatabaseLoad|kDBMission,file); 
}

void SaveMissionData(ITagFile* file)
{
   gMissionData.DatabaseMsg(kDatabaseSave|kDBMission,file); 
}


void MissionDataInit()
{
   AutoAppIPtr_(StructDescTools,pTools); 
   pTools->Register(&mission_sdesc); 
}

void MissionDataTerm()
{
}

////////////////////////////////////////////////////////////
// MISSION DML (NewDark's text patches beside a mission): TagBlocks
//
// A mod's "missN.mis.dml" (and "miss_all.dml") in a folder of the mission
// path -- the HD pack's t2skies, for one, which gives each original mission
// Thief 2's sky: a cloud deck, the sky's colours, stars, a moon -- sets the
// mission's file variables by their editor names:
//
//    TagBlock "SKYOBJVAR"
//    {
//       "Enable New Sky" TRUE
//       "Pole color" 0.36, 0.45, 0.55
//    }
//
// Applied after each load (a fresh mission or a saved game), the folders of
// least precedence first. The DML's other commands (object properties and
// links) are not read yet.
//

static char *dml_trim(char *s)
{
   char *e;
   while (*s && isspace((unsigned char)*s))
      ++s;
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1]))
      *--e = 0;
   return s;
}

static cFileVarBase *dml_find_var(const char *tag)
{
   cFileVarList *list = cFileVarBase::AllFileVars();
   if (!list)
      return NULL;
   for (cFileVarList::cIter iter = list->Iter(); !iter.Done(); iter.Next())
   {
      cFileVarBase *fv = iter.Value();
      if (fv->mDesc && !_strnicmp(fv->mDesc->tag.label, tag, sizeof(fv->mDesc->tag.label)))
         return fv;
   }
   return NULL;
}

static void dml_apply_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   char line[512];
   cFileVarBase *var = NULL;
   const sStructDesc *sdesc = NULL;
   BOOL inBlock = FALSE;
   int blocks = 0, fields = 0, unknown = 0;
   if (!f)
      return;
   AutoAppIPtr_(StructDescTools, pTools);
   while (fgets(line, sizeof(line), f))
   {
      char *s, *c;
      // (a // comment, outside a quoted name)
      for (c = line; *c; ++c)
      {
         if (*c == '"')
         {
            c = strchr(c + 1, '"');
            if (!c)
               break;
         }
         else if (c[0] == '/' && c[1] == '/')
         {
            *c = 0;
            break;
         }
      }
      s = dml_trim(line);
      if (!*s)
         continue;
      if (!_strnicmp(s, "TagBlock", 8))
      {
         char tag[32] = "";
         const char *q = strchr(s, '"');
         const char *e = q ? strchr(q + 1, '"') : NULL;
         if (q && e && e - q - 1 < (int)sizeof(tag))
         {
            memcpy(tag, q + 1, e - q - 1);
            tag[e - q - 1] = 0;
         }
         var = tag[0] ? dml_find_var(tag) : NULL;
         sdesc = (var && var->mDesc->type) ? pTools->Lookup(var->mDesc->type) : NULL;
         if (tag[0] && (!var || !sdesc))
            mprintf("DML: %s: no file variable %s here\n", path, tag);
         else if (var)
            ++blocks;
         inBlock = strchr(s, '{') != NULL;
         continue;
      }
      if (*s == '{')
      {
         inBlock = TRUE;
         continue;
      }
      if (*s == '}')
      {
         if (var)
            var->Update();
         var = NULL;
         sdesc = NULL;
         inBlock = FALSE;
         continue;
      }
      if (inBlock && var && sdesc && *s == '"')
      {
         char *e = strchr(s + 1, '"');
         const sFieldDesc *field;
         char *value;
         if (!e)
            continue;
         *e = 0;
         value = dml_trim(e + 1);
         if (*value == '"')
         {
            char *ve = strrchr(value + 1, '"');
            ++value;
            if (ve)
               *ve = 0;
         }
         field = pTools->GetFieldNamed(sdesc, s + 1);
         if (!field)
         {
            ++unknown;
            mprintf("DML: %s: %s has no field \"%s\"\n", path, var->mDesc->tag.label, s + 1);
            continue;
         }
         pTools->ParseField(field, value, var->mBlock);
         ++fields;
      }
   }
   fclose(f);
   mprintf("DML: %s: %d blocks, %d values%s\n", path, blocks, fields,
           unknown ? " (some unknown)" : "");
}

void MissionApplyDMLs(void)
{
   const char *list = DarkContentSearchPath(kDarkContentMissions);
   const char *base = DarkContentBasePath();
   char dirs[32][260];
   int ndirs = 0, d;
   char name[32];
   if (!list || !*list || gMissionData.num <= 0)
      return;
   sprintf(name, "miss%d.mis.dml", gMissionData.num);
   // the folders, in the list's order (the first one wins)
   while (*list && ndirs < 32)
   {
      const char *e = strchr(list, '+');
      int n = e ? (int)(e - list) : (int)strlen(list);
      char entry[260];
      if (n > 0 && n < (int)sizeof(entry))
      {
         memcpy(entry, list, n);
         entry[n] = 0;
         for (char *c = entry; *c; ++c)
            if (*c == '\\')
               *c = '/';
         if (entry[0] == '/' || !base || !*base)
            snprintf(dirs[ndirs], sizeof(dirs[0]), "%s", entry);
         else
            snprintf(dirs[ndirs], sizeof(dirs[0]), "%s/%s", base, entry);
         ++ndirs;
      }
      list += n;
      if (*list == '+')
         ++list;
   }
   // least precedence first, so the first folder's values stand
   for (d = ndirs - 1; d >= 0; --d)
   {
      char path[300];
      snprintf(path, sizeof(path), "%s/miss_all.dml", dirs[d]);
      dml_apply_file(path);
      snprintf(path, sizeof(path), "%s/%s", dirs[d], name);
      dml_apply_file(path);
   }
}

static ITagFile* find_and_open_file(const char* filename)
{
   char full[256]; 
   if (dbFind(filename,full))
      return dbOpenFile(full,kTagOpenRead);
   return NULL; 
   
}

ITagFile* OpenMissionFile(int missnum)
{
   // construct the base filename
   char base[16]; 
   sprintf(base,"miss%d.ext",missnum); 

   // find the extension 
   char* ext = strchr(base,'.') + 1; 
   
   // Open the mis file
   strcpy(ext,"mis");  
   ITagFile* basefile = find_and_open_file(base); 

   if (!basefile)
      return NULL; 

   // Open the dif file
   strcpy(ext,"dif"); 
   ITagFile* diffile = find_and_open_file(base); 

   if (diffile)
   {
      ITagFile* result = CreatePatchedTagFile(basefile,diffile); 
      SafeRelease(basefile); 
      SafeRelease(diffile);
      return result;
   }
   else 
      return basefile; 
}


