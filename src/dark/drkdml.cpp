// NewDark's DML files: text patches to a mission and its gamesys.
//
// A mod or a fan mission puts them beside the files they patch, in a folder of
// the mission path (the FM's own first, then the mods', then the game's):
//
//    missN.mis.dml     the mission (and miss_all.dml, every mission)
//    <gamesys>.dml     the gamesys (dark.gam.dml). (Not "gamesys.dml": the HD pack's
//                      is the source of its own dark.gam, by archetype numbers that
//                      are other things in a fan mission's gamesys.)
//
// Read here, after a load is done, the folders of least precedence first so
// the first folder's values stand:
//
//    TagBlock "SKYOBJVAR" { "Pole color" 0.36, 0.45, 0.55 }   a file variable
//    +ObjProp <obj> "<property>" { "<field>" <value> ... }    set (and add) a property
//    +ObjProp <obj> "<property>" = <value>                    (a one-value property)
//    -ObjProp <obj> "<property>"                              remove it
//    +Link / ++Link <from> <to> "<flavor>" { fields }         a link (++: another one)
//    -Link <from> <to> "<flavor>"                             remove those links
//    +MetaProp / -MetaProp <obj> <metaproperty>
//
// Objects by number (archetypes negative) or by name. Not yet: StimSource and
// Receptron (act/react), CreateArch, and Fingerprint conditions (a file with one
// is left out, as it may be for another version of the mission).
//
// A saved game holds the mission's objects as they were when it was saved --
// with the DML's changes -- but loads the gamesys anew: so after a saved game
// only the gamesys DMLs, and of the mission's, what they do to archetypes (and
// the file variables, the same each time).

#include <drkmiss.h>
#include <filevar.h>
#include <contentctx.h>
#include <dbfile.h>
#include <appagg.h>
#include <sdestool.h>
#include <sdesbase.h>
#include <property.h>
#include <propman.h>
#include <propbase.h>
#include <propstor.h>
#include <propface.h>
#include <dataops.h>
#include <iobjsys.h>
#include <traitman.h>
#include <linkman.h>
#include <relation.h>
#include <lnkquery.h>
#include <linkbase.h>
#include <objtype.h>
#include <mprintf.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbmem.h>   // must be last header!

static BOOL gDMLLoadingSave = FALSE;

void DMLNoteSaveLoad(BOOL loading)
{
   gDMLLoadingSave = loading;
}

////////////////////////////////////////////////////////////
// TEXT
//

struct sDMLText
{
   char *data;
   char **lines;
   int count;
};

static void dml_free(sDMLText *t)
{
   free(t->data);
   free(t->lines);
   t->data = NULL;
   t->lines = NULL;
   t->count = 0;
}

// the file's lines, with // comments (outside quotes) cut off
static BOOL dml_read(const char *path, sDMLText *t)
{
   FILE *f = fopen(path, "rb");
   long size;
   int cap = 256;
   char *p;
   memset(t, 0, sizeof(*t));
   if (!f)
      return FALSE;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size <= 0 || size > 16 * 1024 * 1024)
   {
      fclose(f);
      return FALSE;
   }
   t->data = (char *)malloc(size + 1);
   t->lines = (char **)malloc(sizeof(char *) * cap);
   if (!t->data || !t->lines || fread(t->data, 1, size, f) != (size_t)size)
   {
      fclose(f);
      dml_free(t);
      return FALSE;
   }
   fclose(f);
   t->data[size] = 0;
   for (p = t->data; *p;)
   {
      char *line = p, *c;
      BOOL quoted = FALSE;
      while (*p && *p != '\n' && *p != '\r')
         ++p;
      if (*p)
      {
         *p++ = 0;
         while (*p == '\n' || *p == '\r')
            ++p;
      }
      for (c = line; *c; ++c)
      {
         if (*c == '"')
            quoted = !quoted;
         else if (!quoted && c[0] == '/' && c[1] == '/')
         {
            *c = 0;
            break;
         }
      }
      if (t->count == cap)
      {
         cap *= 2;
         t->lines = (char **)realloc(t->lines, sizeof(char *) * cap);
         if (!t->lines)
         {
            dml_free(t);
            return FALSE;
         }
      }
      t->lines[t->count++] = line;
   }
   return TRUE;
}

static const char *dml_skip(const char *s)
{
   while (*s && isspace((unsigned char)*s))
      ++s;
   return s;
}

// the next token: a quoted string, a brace, '=', or a word; FALSE at the end
static BOOL dml_token(const char **ps, char *out, int size, BOOL *quoted)
{
   const char *s = dml_skip(*ps);
   int n = 0;
   if (quoted)
      *quoted = FALSE;
   if (!*s)
   {
      *ps = s;
      return FALSE;
   }
   if (*s == '"')
   {
      ++s;
      while (*s && *s != '"')
      {
         if (n < size - 1)
            out[n++] = *s;
         ++s;
      }
      if (*s == '"')
         ++s;
      if (quoted)
         *quoted = TRUE;
   }
   else if (*s == '{' || *s == '}' || *s == '=')
      out[n++] = *s++;
   else
      while (*s && !isspace((unsigned char)*s) && *s != '{' && *s != '}' && *s != '"' && *s != '=')
      {
         if (n < size - 1)
            out[n++] = *s;
         ++s;
      }
   out[n] = 0;
   *ps = s;
   return TRUE;
}

static void dml_trim_end(char *s)
{
   char *e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1]))
      *--e = 0;
}

// The block that follows a command's line (a '{' at its end or on the next
// line), as the lines between its braces: *first, *last (exclusive); the
// line after the block in *next. FALSE if there is none.
static BOOL dml_block(sDMLText *t, int line, const char *rest, int *first, int *last, int *next)
{
   int i = line, depth = 0;
   const char *s = dml_skip(rest);
   if (*s != '{')
   {
      // on the next non-empty line?
      for (i = line + 1; i < t->count && !*dml_skip(t->lines[i]); ++i)
         ;
      if (i >= t->count || *dml_skip(t->lines[i]) != '{')
         return FALSE;
   }
   *first = i + 1;
   for (; i < t->count; ++i)
   {
      BOOL quoted = FALSE;
      for (const char *c = t->lines[i]; *c; ++c)
      {
         if (*c == '"')
            quoted = !quoted;
         else if (!quoted && *c == '{')
            ++depth;
         else if (!quoted && *c == '}' && --depth == 0)
         {
            *last = i;
            *next = i + 1;
            return TRUE;
         }
      }
   }
   *last = *next = t->count;
   return TRUE;
}

////////////////////////////////////////////////////////////
// APPLYING
//

struct sDMLRun
{
   const char *path;
   BOOL tagBlocksOnly;     // a saved game's mission DML: file variables...
   BOOL abstractOnly;      // ...and archetypes only
   int done, skipped, failed, unsupported;
};

static ObjID dml_object(sDMLRun *run, const char *token, BOOL quoted)
{
   AutoAppIPtr_(ObjectSystem, pObjSys);
   char *end;
   long id;
   if (!quoted)
   {
      id = strtol(token, &end, 10);
      if (end != token && !*end)
         return (ObjID)id;
   }
   id = pObjSys->GetObjectNamed(token);
   if (id == OBJ_NULL)
      mprintf("DML: %s: no object \"%s\"\n", run->path, token);
   return (ObjID)id;
}

// a field's value: the rest of its line, a quoted string's quotes off
static void dml_value(const char *rest, char *out, int size)
{
   const char *s = dml_skip(rest);
   int n = 0;
   if (*s == '"')
   {
      const char *e = strrchr(s + 1, '"');
      // ("a", "b": a list of quoted names, kept as one)
      const char *mid = strchr(s + 1, '"');
      if (e && e == mid)
      {
         n = (int)(e - s - 1);
         if (n > size - 1)
            n = size - 1;
         memcpy(out, s + 1, n);
         out[n] = 0;
         return;
      }
   }
   strncpy(out, s, size - 1);
   out[size - 1] = 0;
   dml_trim_end(out);
   // (quotes inside a list: "X Axis", "Y Axis" -> X Axis, Y Axis)
   for (char *r = out, *w = out;; ++r)
   {
      if (*r != '"')
         *w++ = *r;
      if (!*r)
         break;
   }
}

// one "field" value line into a structure described by sdesc
static BOOL dml_set_field(sDMLRun *run, const sStructDesc *sdesc, void *data, const char *line,
                          const char *what)
{
   AutoAppIPtr_(StructDescTools, pTools);
   const char *s = line;
   char name[96], value[512];
   BOOL quoted;
   if (!dml_token(&s, name, sizeof(name), &quoted))
      return TRUE;
   if (!strcmp(name, "{") || !strcmp(name, "}"))
      return TRUE;
   dml_value(s, value, sizeof(value));
   if (!name[0] || !sdesc)
   {
      // a one-value structure ("" 0.40)
      if (sdesc && pTools->ParseSimple(sdesc, value, data))
         return TRUE;
      if (sdesc && sdesc->nfields >= 1)
      {
         pTools->ParseField(&sdesc->fields[0], value, data);
         return TRUE;
      }
      return FALSE;
   }
   const sFieldDesc *field = pTools->GetFieldNamed(sdesc, name);
   if (!field)
   {
      // (names in any case)
      for (int i = 0; i < sdesc->nfields; ++i)
         if (!_stricmp(sdesc->fields[i].name, name))
         {
            field = &sdesc->fields[i];
            break;
         }
   }
   if (!field)
   {
      mprintf("DML: %s: %s has no field \"%s\"\n", run->path, what, name);
      return FALSE;
   }
   // (a script slot "none": empty)
   if (field->type == kFieldTypeString && !_stricmp(value, "none"))
      value[0] = 0;
   pTools->ParseField(field, value, data);
   return TRUE;
}

static IProperty *dml_property(const char *name)
{
   AutoAppIPtr_(PropertyManager, pPropMan);
   IProperty *prop = pPropMan->GetPropertyNamed(name);
   if (prop && prop->GetID() != PROPID_NULL)
      return prop;
   SafeRelease(prop);
   return NULL;
}

static void dml_obj_prop(sDMLRun *run, ObjID obj, const char *propName, sDMLText *t,
                         int first, int last, const char *inlineValue)
{
   AutoAppIPtr_(ObjectSystem, pObjSys);
   AutoAppIPtr_(StructDescTools, pTools);
   IProperty *prop = dml_property(propName);
   if (!prop)
   {
      mprintf("DML: %s: no property \"%s\"\n", run->path, propName);
      ++run->failed;
      return;
   }
   if (!pObjSys->Exists(obj))
   {
      mprintf("DML: %s: object %d is not in this mission\n", run->path, obj);
      ++run->failed;
      SafeRelease(prop);
      return;
   }
   const sStructDesc *sdesc = pTools->Lookup(prop->DescribeType()->type);
   if (!sdesc)
   {
      // a string property (the inventory's limb model, say)
      IStringProperty *strprop;
      if (SUCCEEDED(prop->QueryInterface(IID_IStringProperty, (void **)&strprop)))
      {
         char value[512] = "";
         if (inlineValue)
            dml_value(inlineValue, value, sizeof(value));
         else
            for (int i = first; i < last; ++i)
            {
               const char *s = t->lines[i];
               char name[96];
               if (dml_token(&s, name, sizeof(name), NULL) && strcmp(name, "{") && strcmp(name, "}"))
               {
                  dml_value(s, value, sizeof(value));
                  break;
               }
            }
         strprop->Set(obj, value);
         SafeRelease(strprop);
         ++run->done;
      }
      else
      {
         mprintf("DML: %s: property \"%s\" has no description to read it by\n", run->path, propName);
         ++run->failed;
      }
      SafeRelease(prop);
      return;
   }
   IPropertyStore *store = NULL;
   if (FAILED(prop->QueryInterface(IID_IPropertyStore, (void **)&store)) || !store)
   {
      ++run->failed;
      SafeRelease(prop);
      return;
   }
   IDataOps *ops = store->GetOps();
   sDatum dat;
   BOOL got = store->GetCopy(obj, &dat);
   if (!got)
   {
      prop->Create(obj);
      got = store->GetCopy(obj, &dat);
   }
   if (!got || !ops)
   {
      ++run->failed;
      SafeRelease(ops);
      SafeRelease(store);
      SafeRelease(prop);
      return;
   }
   void *data = (ops->BlockSize(dat) >= 0) ? dat.value : &dat;
   BOOL ok = TRUE;
   if (inlineValue)
   {
      char value[512];
      dml_value(inlineValue, value, sizeof(value));
      if (!pTools->ParseSimple(sdesc, value, data) && sdesc->nfields >= 1)
         pTools->ParseField(&sdesc->fields[0], value, data);
   }
   else
      for (int i = first; i < last; ++i)
         if (*dml_skip(t->lines[i]))
            ok = dml_set_field(run, sdesc, data, t->lines[i], propName) && ok;
   store->Set(obj, dat);
   prop->Touch(obj);
   store->ReleaseCopy(obj, dat);
   if (ok)
      ++run->done;
   else
      ++run->failed;
   SafeRelease(ops);
   SafeRelease(store);
   SafeRelease(prop);
}

static void dml_link(sDMLRun *run, ObjID from, ObjID to, const char *flavor, BOOL add, BOOL another,
                     sDMLText *t, int first, int last)
{
   AutoAppIPtr_(LinkManager, pLinkMan);
   AutoAppIPtr_(StructDescTools, pTools);
   IRelation *rel = pLinkMan->GetRelationNamed(flavor);
   if (!rel || rel->GetID() == RELID_NULL)
   {
      mprintf("DML: %s: no link flavor \"%s\"\n", run->path, flavor);
      ++run->failed;
      SafeRelease(rel);
      return;
   }
   if (!add)
   {
      ILinkQuery *q = rel->Query(from, to);
      int n = 0;
      if (q)
      {
         // (the query's links, gathered first: removing one under it moves it)
         LinkID ids[64];
         for (; !q->Done() && n < 64; q->Next())
            ids[n++] = q->ID();
         SafeRelease(q);
         for (int i = 0; i < n; ++i)
            rel->Remove(ids[i]);
      }
      ++run->done;
      SafeRelease(rel);
      return;
   }
   const sRelationDataDesc *ddesc = rel->DescribeData();
   const sStructDesc *sdesc = (ddesc && ddesc->size > 0) ? pTools->Lookup(ddesc->type) : NULL;
   LinkID id = (!another) ? rel->GetSingleLink(from, to) : LINKID_NULL;
   void *buffer = NULL;
   if (ddesc && ddesc->size > 0)
   {
      buffer = calloc(1, ddesc->size);
      if (id != LINKID_NULL)
      {
         void *old = rel->GetData(id);
         if (old && buffer)
            memcpy(buffer, old, ddesc->size);
      }
      if (buffer && sdesc)
         for (int i = first; i < last; ++i)
            if (*dml_skip(t->lines[i]))
               dml_set_field(run, sdesc, buffer, t->lines[i], flavor);
   }
   if (id == LINKID_NULL)
      id = buffer ? rel->AddFull(from, to, buffer) : rel->Add(from, to);
   else if (buffer)
      rel->SetData(id, buffer);
   free(buffer);
   if (id != LINKID_NULL)
      ++run->done;
   else
      ++run->failed;
   SafeRelease(rel);
}

static void dml_meta(sDMLRun *run, ObjID obj, ObjID meta, BOOL add)
{
   AutoAppIPtr_(TraitManager, pTraitMan);
   if (obj == OBJ_NULL || meta == OBJ_NULL || !pTraitMan->IsMetaProperty(meta))
   {
      ++run->failed;
      return;
   }
   if (add)
      pTraitMan->AddObjMetaProperty(obj, meta);
   else
      pTraitMan->RemoveObjMetaProperty(obj, meta);
   ++run->done;
}

static void dml_tag_block(sDMLRun *run, const char *tag, sDMLText *t, int first, int last)
{
   AutoAppIPtr_(StructDescTools, pTools);
   cFileVarList *list = cFileVarBase::AllFileVars();
   cFileVarBase *var = NULL;
   if (list)
      for (cFileVarList::cIter iter = list->Iter(); !iter.Done(); iter.Next())
      {
         cFileVarBase *fv = iter.Value();
         if (fv->mDesc && !_strnicmp(fv->mDesc->tag.label, tag, sizeof(fv->mDesc->tag.label)))
         {
            var = fv;
            break;
         }
      }
   const sStructDesc *sdesc = (var && var->mDesc->type) ? pTools->Lookup(var->mDesc->type) : NULL;
   if (!var || !sdesc)
   {
      mprintf("DML: %s: no file variable %s here\n", run->path, tag);
      ++run->failed;
      return;
   }
   BOOL ok = TRUE;
   for (int i = first; i < last; ++i)
      if (*dml_skip(t->lines[i]))
         ok = dml_set_field(run, sdesc, var->mBlock, t->lines[i], tag) && ok;
   var->Update();
   if (ok)
      ++run->done;
   else
      ++run->failed;
}

static void dml_apply_file(const char *path, BOOL tagBlocksOnly, BOOL abstractOnly)
{
   sDMLText t;
   if (!dml_read(path, &t))
      return;
   sDMLRun run = { path, tagBlocksOnly, abstractOnly, 0, 0, 0, 0 };

   for (int i = 0; i < t.count;)
   {
      const char *s = t.lines[i];
      char cmd[64];
      BOOL quoted;
      if (!dml_token(&s, cmd, sizeof(cmd), &quoted) || !_stricmp(cmd, "DML1"))
      {
         ++i;
         continue;
      }
      int first = i + 1, last = i + 1, next = i + 1;
      // (the block comes after the command's own words)
      char tok[4][256];
      BOOL tq[4] = {FALSE, FALSE, FALSE, FALSE};
      int ntok = 0;
      const char *rest = s, *eq = NULL;
      for (;;)
      {
         const char *before = rest;
         char word[256];
         BOOL q;
         if (!dml_token(&rest, word, sizeof(word), &q))
            break;
         if (!q && !strcmp(word, "{"))
         {
            rest = before;
            break;
         }
         if (!q && !strcmp(word, "="))
         {
            eq = rest;
            break;
         }
         if (ntok < 4)
         {
            strcpy(tok[ntok], word);
            tq[ntok] = q;
            ++ntok;
         }
      }
      BOOL hasBlock = !eq && dml_block(&t, i, rest, &first, &last, &next);
      if (!hasBlock)
         first = last = next = i + 1;

      if (!_stricmp(cmd, "Fingerprint"))
      {
         mprintf("DML: %s: has a fingerprint (conditions on the mission), left out\n", path);
         dml_free(&t);
         return;
      }
      else if (!_stricmp(cmd, "TagBlock") && ntok >= 1)
         dml_tag_block(&run, tok[0], &t, first, last);
      else if (tagBlocksOnly && !abstractOnly)
         ++run.skipped;
      else if ((!_stricmp(cmd, "+ObjProp") || !_stricmp(cmd, "-ObjProp")) && ntok >= 2)
      {
         ObjID obj = dml_object(&run, tok[0], tq[0]);
         if (obj == OBJ_NULL)
            ++run.failed;
         else if (abstractOnly && !OBJ_IS_ABSTRACT(obj))
            ++run.skipped;
         else if (cmd[0] == '+')
            dml_obj_prop(&run, obj, tok[1], &t, first, last, eq);
         else
         {
            IProperty *prop = dml_property(tok[1]);
            if (prop)
            {
               prop->Delete(obj);
               ++run.done;
               SafeRelease(prop);
            }
            else
               ++run.failed;
         }
      }
      else if ((!_stricmp(cmd, "+Link") || !_stricmp(cmd, "++Link") || !_stricmp(cmd, "-Link")) && ntok >= 3)
      {
         ObjID from = dml_object(&run, tok[0], tq[0]), to = dml_object(&run, tok[1], tq[1]);
         if (from == OBJ_NULL || to == OBJ_NULL)
            ++run.failed;
         else if (abstractOnly && !(OBJ_IS_ABSTRACT(from) && OBJ_IS_ABSTRACT(to)))
            ++run.skipped;
         else
            dml_link(&run, from, to, tok[2], cmd[0] == '+', cmd[1] == '+', &t, first, last);
      }
      else if ((!_stricmp(cmd, "+MetaProp") || !_stricmp(cmd, "-MetaProp")) && ntok >= 2)
      {
         ObjID obj = dml_object(&run, tok[0], tq[0]), meta = dml_object(&run, tok[1], tq[1]);
         if (abstractOnly && obj != OBJ_NULL && !OBJ_IS_ABSTRACT(obj))
            ++run.skipped;
         else
            dml_meta(&run, obj, meta, cmd[0] == '+');
      }
      else
      {
         if (run.unsupported < 3)
            mprintf("DML: %s: \"%s\" not read (not supported yet)\n", path, cmd);
         ++run.unsupported;
      }
      i = next;
   }
   mprintf("DML: %s: %d applied, %d failed, %d left for the saved game, %d not supported\n",
           path, run.done, run.failed, run.skipped, run.unsupported);
   dml_free(&t);
}

////////////////////////////////////////////////////////////
// WHICH FILES
//

// the mission path's folders, in its order (the first one wins); returns how many
static int dml_folders(char dirs[][260], int max)
{
   const char *list = DarkContentSearchPath(kDarkContentMissions);
   const char *base = DarkContentBasePath();
   int n = 0;
   if (!list)
      return 0;
   while (*list && n < max)
   {
      const char *e = strchr(list, '+');
      int len = e ? (int)(e - list) : (int)strlen(list);
      char entry[260];
      if (len > 0 && len < (int)sizeof(entry))
      {
         memcpy(entry, list, len);
         entry[len] = 0;
         for (char *c = entry; *c; ++c)
            if (*c == '\\')
               *c = '/';
         dml_trim_end(entry);
         if (entry[0] == '/' || !base || !*base)
            snprintf(dirs[n], 260, "%s", entry);
         else
            snprintf(dirs[n], 260, "%s/%s", base, entry);
         // (one folder once)
         BOOL seen = FALSE;
         for (int k = 0; k < n; ++k)
            if (!_stricmp(dirs[k], dirs[n]))
               seen = TRUE;
         if (!seen)
            ++n;
      }
      list += len;
      if (*list == '+')
         ++list;
   }
   return n;
}

void MissionApplyDMLs(void)
{
   char dirs[32][260];
   char gamesys[64], gamName[96], missName[32];
   const int ndirs = dml_folders(dirs, 32);
   const int missnum = GetMissionData()->num;
   const BOOL save = gDMLLoadingSave;
   if (ndirs <= 0)
      return;
   dbCurrentGamesys(gamesys, sizeof(gamesys));
   gamName[0] = 0;
   if (gamesys[0])
      snprintf(gamName, sizeof(gamName), "%s.dml", gamesys);
   snprintf(missName, sizeof(missName), "miss%d.mis.dml", missnum);

   // least precedence first, so the first folder's changes stand
   for (int d = ndirs - 1; d >= 0; --d)
   {
      char path[400];
      // the gamesys (dark.gam.dml): loaded anew with a saved game too
      if (gamName[0])
      {
         snprintf(path, sizeof(path), "%s/%s", dirs[d], gamName);
         dml_apply_file(path, FALSE, FALSE);
      }
      if (missnum <= 0)
         continue;
      // the mission: all of it on a fresh start; with a saved game, its file
      // variables and archetypes (its objects are in the save as patched)
      snprintf(path, sizeof(path), "%s/miss_all.dml", dirs[d]);
      dml_apply_file(path, save, save);
      snprintf(path, sizeof(path), "%s/%s", dirs[d], missName);
      dml_apply_file(path, save, save);
   }
}
