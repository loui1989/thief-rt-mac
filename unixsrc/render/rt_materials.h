// Material maps for RTGL1, made from the game's own textures (as Blood RT and
// Duke RT do: their rt_materials.cpp).
//
// RTGL1 finds a texture's extra maps by its name in <rt>/mat_dev: <name>_e.png
// (emission), <name>_orm.png (R occlusion, G roughness, B metallic),
// <name>_n.png (normal, Y up) and <name>_h.png (height, white raised: parallax
// occlusion mapping). Thief has none of these, so they are made, once, from
// each texture's own light and dark texels, by what the texture is made of:
//
//  - the material table, <rt>/materials.txt (written with the defaults below
//    the first time; edit it to taste): classes of roughness, metalness, bump
//    and polish, and rules that put textures in them by the name of their
//    file ("fam/city/marble" is marble);
//  - bumps and heights from luminance at two scales (the fine grain, and a
//    stone's bulge between its dark joints), the cavities (the joints and
//    cracks, darker than the texture's average) in the occlusion channel;
//  - polish: RTGL1's polished surfaces (RG_MESH_PRIMITIVE_POLISHED) mirror
//    the room with traced mirror rays, blended over the texture -- sharp
//    reflections, not glossy ones, whose single noisy sample the denoiser
//    smears into mud (marble floors, glazed tiles, polished metal, glass);
//  - glow: lit windows glow where their panes are bright and warm; a NewDark
//    material's illum_map is its glow map; a flame sprite glows in its own
//    colour, within its soft round shape.
//
// The texture's RTGL1 name carries its file's name, a hash of its pixels and
// of its class, so a changed table or texture makes its maps again, and the
// same picture loaded twice shares them.
//
// The maps are at most kMapMax texels across (kGlowMax for emission, which is
// seen as it is): an HD pack's 2048 textures would otherwise make some 5 MB of
// maps each, 14 GB for the Thief Gold HD Mod. They are made off the main
// thread (rtgl1.cpp), so the functions here keep to their own files.

namespace rtmat {

// part of every material name: a new version makes every map again
constexpr uint32_t kMapsVersion = 4;
constexpr int kMapMax = 512;
constexpr int kGlowMax = 1024;

// ---- PNG, write and read (zlib) ----------------------------------------------

inline uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t c = 0)
{
   return uint32_t(crc32(c, p, uInt(n)));
}

inline void put32(std::vector<uint8_t> &o, uint32_t v)
{
   o.push_back(uint8_t(v >> 24)); o.push_back(uint8_t(v >> 16));
   o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v));
}

inline void chunk(std::vector<uint8_t> &o, const char *type, const uint8_t *data, size_t n)
{
   put32(o, uint32_t(n));
   const size_t at = o.size();
   o.insert(o.end(), type, type + 4);
   if (n)
      o.insert(o.end(), data, data + n);
   put32(o, crc32_of(&o[at], n + 4));
}

// rgb: w*h*channels bytes (3 or 1 channels)
inline bool write_png(const std::string &path, const uint8_t *pix, int w, int h, int channels)
{
   std::vector<uint8_t> raw;
   raw.reserve(size_t(h) * (size_t(w) * channels + 1));
   for (int y = 0; y < h; ++y) {
      raw.push_back(0);   // filter: none
      raw.insert(raw.end(), pix + size_t(y) * w * channels, pix + size_t(y + 1) * w * channels);
   }
   uLongf zsize = compressBound(uLong(raw.size()));
   std::vector<uint8_t> z(zsize);
   if (compress2(z.data(), &zsize, raw.data(), uLong(raw.size()), 6) != Z_OK)
      return false;
   std::vector<uint8_t> o = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
   uint8_t ihdr[13];
   const uint32_t W = uint32_t(w), H = uint32_t(h);
   ihdr[0] = uint8_t(W >> 24); ihdr[1] = uint8_t(W >> 16); ihdr[2] = uint8_t(W >> 8); ihdr[3] = uint8_t(W);
   ihdr[4] = uint8_t(H >> 24); ihdr[5] = uint8_t(H >> 16); ihdr[6] = uint8_t(H >> 8); ihdr[7] = uint8_t(H);
   ihdr[8] = 8; ihdr[9] = channels == 1 ? 0 : 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
   chunk(o, "IHDR", ihdr, 13);
   chunk(o, "IDAT", z.data(), zsize);
   chunk(o, "IEND", nullptr, 0);
   // (a file of its own for each writer: two threads may make the same maps)
   static std::atomic<unsigned> s_tmp{0};
   const std::string tmp = path + ".tmp" + std::to_string(s_tmp.fetch_add(1));
   FILE *f = fopen(tmp.c_str(), "wb");
   if (!f)
      return false;
   const bool ok = fwrite(o.data(), 1, o.size(), f) == o.size();
   fclose(f);
   return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

// 8-bit greyscale, grey+alpha, RGB, RGBA or palette PNGs, not interlaced: RGBA
inline bool read_png(const std::string &path, std::vector<uint8_t> &rgba, int &w, int &h)
{
   FILE *f = fopen(path.c_str(), "rb");
   if (!f)
      return false;
   std::vector<uint8_t> d;
   uint8_t buf[65536];
   for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
      d.insert(d.end(), buf, buf + n);
   fclose(f);
   if (d.size() < 33 || memcmp(d.data(), "\x89PNG", 4))
      return false;
   auto be = [&](size_t at) { return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3]; };
   int depth = 0, type = 0, interlace = 0;
   std::vector<uint8_t> z, pal, trns;
   for (size_t at = 8; at + 8 <= d.size();) {
      const uint32_t n = be(at);
      const char *t = reinterpret_cast<const char *>(&d[at + 4]);
      if (at + 12 + n > d.size())
         break;
      const uint8_t *c = &d[at + 8];
      if (!memcmp(t, "IHDR", 4)) {
         w = int(be(at + 8)); h = int(be(at + 12));
         depth = c[8]; type = c[9]; interlace = c[12];
      } else if (!memcmp(t, "PLTE", 4)) pal.assign(c, c + n);
      else if (!memcmp(t, "tRNS", 4)) trns.assign(c, c + n);
      else if (!memcmp(t, "IDAT", 4)) z.insert(z.end(), c, c + n);
      else if (!memcmp(t, "IEND", 4)) break;
      at += 12 + n;
   }
   if (depth != 8 || interlace || w <= 0 || h <= 0 || w > 8192 || h > 8192)
      return false;
   const int ch = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
   if (!ch)
      return false;
   const size_t stride = size_t(w) * ch;
   std::vector<uint8_t> raw((stride + 1) * h);
   uLongf rawSize = uLongf(raw.size());
   if (uncompress(raw.data(), &rawSize, z.data(), uLong(z.size())) != Z_OK || rawSize != raw.size())
      return false;
   std::vector<uint8_t> cur(stride), prev(stride, 0);
   rgba.assign(size_t(w) * h * 4, 255);
   for (int y = 0; y < h; ++y) {
      const uint8_t filter = raw[y * (stride + 1)];
      const uint8_t *src = &raw[y * (stride + 1) + 1];
      for (size_t x = 0; x < stride; ++x) {
         const int a = x >= size_t(ch) ? cur[x - ch] : 0, b = prev[x], c = x >= size_t(ch) ? prev[x - ch] : 0;
         int v = src[x];
         switch (filter) {
         case 1: v += a; break;
         case 2: v += b; break;
         case 3: v += (a + b) / 2; break;
         case 4: {
            const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
            break;
         }
         default: break;
         }
         cur[x] = uint8_t(v);
      }
      for (int x = 0; x < w; ++x) {
         uint8_t *o = &rgba[(size_t(y) * w + x) * 4];
         const uint8_t *s = &cur[size_t(x) * ch];
         if (type == 3) {
            const size_t i = s[0];
            if (i * 3 + 2 < pal.size()) { o[0] = pal[i * 3]; o[1] = pal[i * 3 + 1]; o[2] = pal[i * 3 + 2]; }
            o[3] = i < trns.size() ? trns[i] : 255;
         } else if (ch <= 2) {
            o[0] = o[1] = o[2] = s[0];
            o[3] = ch == 2 ? s[1] : 255;
         } else {
            o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
            o[3] = ch == 4 ? s[3] : 255;
         }
      }
      std::swap(cur, prev);
   }
   return true;
}

// ---- the material table ----------------------------------------------------

struct Class {
   std::string name;
   float rough = 0.85f, metal = 0.0f, bump = 0.0f, polish = 0.0f, glow = 0.0f;
   bool none = false;      // "none": no maps at all (sky, water, lava, UI)
   bool window = false;    // a class named window...: its bright, warm texels glow
   bool stained = false;   // a class named stained...: stained glass, its coloured panes glow
   uint32_t hash = 0;
};
struct Rule { std::string pattern; int cls; };
struct Table {
   std::vector<Class> classes;
   std::vector<Rule> rules;
   float autobump = 0.6f;
   int fallback = -1;      // the class of a texture no rule names (autobump)
   bool loaded = false;
   uint32_t hash = 2166136261u;
};
inline Table g_table;

// The defaults, written to <rt>/materials.txt the first time. Thief's texture
// files are named for what they are (marble, brick, iron...), so the rules
// are patterns on the file's path from its resource root.
inline const char *kDefaultTable = R"(# Thief RT material table: what each texture is made of.
#
#   class <name> <roughness 0..1> <metallic 0..1> <bump 0..2> <polish 0..1.5> [glow]
#   <pattern> <class>        (the texture's path, as fam/city/marble: * and ? wildcards;
#                             the first rule that matches wins)
#   autobump <bump>          (textures no rule names: bumps from their own texels)
#   (patterns match in any case)
#
# polish: the surface mirrors the room (sharp, traced reflections over its texture).
# A rule for such a class with no / in it looks at the texture's own file name, not
# its family's (a family named marbletex is not all marble).
# A class named window...: its bright, warm texels glow (lit windows), this strongly.
# A class named stained...: stained glass -- its coloured panes glow with the light
# through them, this strongly, and throw their colour into the room; the lead stays dark.
# A class named none: no maps (sky, water, lava, the interface).
# Delete this file to get the defaults back; after editing it, the textures'
# maps are made again by themselves.

autobump 0.9

class none        1.00 0.00 0.00 0.00
class marble      0.35 0.00 0.10 1.20
class tile        0.55 0.00 0.15 0.80
class glass       0.30 0.00 0.00 1.50
class stained     0.40 0.00 0.25 0.00 2.4
class window      0.55 0.00 0.30 1.20 2.0
class metal       0.45 0.80 0.50 0.35
class blade       0.20 0.90 0.15 1.00
class rust        0.85 0.45 0.90 0.00
class wood        0.75 0.00 0.60 0.00
class plaster     0.90 0.00 0.35 0.00
class cloth       1.00 0.00 0.40 0.00
class brick       0.90 0.00 1.00 0.00
class stone       0.85 0.00 0.90 0.00
class rock        0.95 0.00 1.10 0.00
class skin        0.80 0.00 0.15 0.00
class blood       0.25 0.00 0.40 0.70
class windowfire  0.90 0.00 0.60 0.00 1.6
class wallpaper   0.80 0.00 0.20 0.00
class parquet     0.45 0.00 0.30 0.35

# pools of blood: wet and thick, a soft, uneven sheen of the room
*blood*          blood
*/blod*          blood

# no maps
*sky*/*          none
*/sky*           none
*water*          none
*lava*           none
*acid*           none
intrface/*       none
book/*           none
books/*          none
bitmap/*         none
mesh/txt*/*      skin

# Thief II's own families (first, so the general rules below do not take them):
# the mansions' marble floors, polished parquet, damask wallpaper and marble
# columns; the city's windows; the Mechanists' painted iron
fam/mbfloor/*            marble
fam/vicm*/vic*co*        marble
fam/artdout/blackmb      marble
fam/rescor_2/floor*      tile
fam/wdfloor/*            parquet
fam/vicm*/wall*          wallpaper
fam/rescor_2/wall0*      wallpaper
fam/*/decwal*            wallpaper
fam/stainglass/light*    plaster
fam/stainglass/*         stained
fam/newkeep/keepg*       stained
fam/rescor_1/hothou*     glass
fam/vicwood/solwin*      window
fam/civicb*/*win*        window
fam/artdmisc/adwin*      window
fam/artdout/mlight       window
fam/ceilpain/*           plaster
fam/artdeco/dewal*       plaster
fam/artdeco/dceil*       wood
fam/artdeco/mecban       cloth
fam/core_*/crate*        wood
fam/core_*/creat*        wood
fam/mech/mwallw*         wood
fam/mech/mdebri*         rock
fam/mech/*rust*          rust
fam/mech/*               metal
fam/newmech/*            metal

# boarded-up, shuttered and empty windows: no glass, only wood
*/rwind*         wood
*/win325s*      wood
*/winwd64s*     wood

# stained glass (before the windows: WindowStain is stained glass)
fam/*stain[0-9]*    stained
fam/*/stain         stained
fam/*stained*       stained
fam/*windowstain*   stained
fam/*rose*          stained
fam/*/glass[0-9]*   stained
fam/*/glasss        stained
fam/*/sglass*       stained
fam/*/jefglass      stained
fam/*/panglass      stained
fam/*/hamglass*     stained
fam/*/keepglass*    stained
fam/*/tpbglass*     stained
fam/*/tbpglass*     stained
fam/*/spiderglass   stained
# a fireplace with its fire painted in (Assassins): the fire glows and lights the room
# (a "window" class: its bright, warm texels glow)
fam/rescore/firepl02   windowfire

# windows: lit panes glow; other glass is glass
*glass*          glass
*glpries*        glass
*glsmelt*        glass
*/stain[0-9]*    glass
*/rose*          glass
*/win[0-9]*      window
*/win[0-9]*l     window
*/window*        window
*/winwal*        window
*/cfwin*         window
*/winwd*         window
*/winwl*         window

*marb*           marble
*/mar[0-9]*      marble
*marwal*         marble
*greenmar*       marble
*/keeperz/floor007  tile
*tile*           tile
*mosaic*         tile
# the player's sword: a polished steel blade
*/swordbl*       blade
*/bladesp*       blade
*rust*           rust
*iron*           metal
*bronz*          metal
*broz*           metal
*brss*           metal
*brass*          metal
*metal*          metal
*gird*           metal
*grate*          metal
*grat*           metal
*gear*           metal
*pipe*           metal
*plate*          metal
*steel*          metal
*copper*         metal
*gold*           metal
*wood*           wood
*/wd*            wood
*plank*          wood
*plnk*           wood
*parkay*         wood
*wgrain*         wood
*beam*           wood
*bark*           wood
*door*           wood
*book*           wood
*carp*           cloth
*rug*            cloth
*curtain*        cloth
*banner*         cloth
*cloth*          cloth
*stuc*           plaster
*plas*           plaster
*paper*          plaster
*cement*         plaster
*bric*           brick
*brik*           brick
*brick*          brick
*rock*           rock
*cave*           rock
*crag*           rock
*/roc[0-9]*      rock
*stone*          stone
*stn*            stone
*cobb*           stone
*cobl*           stone
*granit*         stone
*granw*          stone
*hewn*           stone
*slab*           stone
*wall*           stone
*ledge*          stone
*floor*          stone

# what is left of a family of rough walls: as its walls (after every rule
# above, so its marble, tiles, wood and metal are still those)
*/cave/*         rock
*/rubble/*       rock
*/mine/*         rock
*/sewer/*        brick
*/sewer2/*       brick
)";

inline bool glob(const char *p, const char *s)
{
   for (; *p; ++p, ++s) {
      if (*p == '*') {
         while (p[1] == '*')
            ++p;
         if (!p[1])
            return true;
         for (; *s; ++s)
            if (glob(p + 1, s))
               return true;
         return false;
      }
      if (*p == '[') {
         // a range: [0-9]
         const char *e = strchr(p, ']');
         if (!e || !*s)
            return false;
         bool in = false;
         const char sc = char(tolower((unsigned char)*s));
         for (const char *c = p + 1; c < e; ++c)
            if (c + 2 < e && c[1] == '-') { in |= sc >= tolower((unsigned char)c[0]) && sc <= tolower((unsigned char)c[2]); c += 2; }
            else in |= sc == tolower((unsigned char)*c);
         if (!in)
            return false;
         p = e;
         continue;
      }
      // (in any case: a fan mission's textures are often named in capitals --
      // HALLEFORD/BrickYellow02 -- and no rule named them)
      if (!*s || (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)))
         return false;
   }
   return !*s;
}

inline uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u)
{
   const uint8_t *b = static_cast<const uint8_t *>(p);
   for (size_t i = 0; i < n; ++i)
      h = (h ^ b[i]) * 16777619u;
   return h;
}

inline void load_table(const std::string &rtDir)
{
   Table &t = g_table;
   if (t.loaded)
      return;
   t.loaded = true;
   const std::string path = rtDir + "/materials.txt";
   std::string text;
   if (FILE *f = fopen(path.c_str(), "rb")) {
      char buf[4096];
      for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
         text.append(buf, n);
      fclose(f);
   } else {
      text = kDefaultTable;
      if (FILE *w = fopen(path.c_str(), "wb")) {
         fputs(kDefaultTable, w);
         fclose(w);
      }
   }
   t.hash = fnv(text.data(), text.size());
   std::unordered_map<std::string, int> byName;
   size_t at = 0;
   while (at < text.size()) {
      size_t end = text.find('\n', at);
      if (end == std::string::npos)
         end = text.size();
      std::string line = text.substr(at, end - at);
      at = end + 1;
      if (size_t c = line.find('#'); c != std::string::npos)
         line.resize(c);
      char a[160], b[160];
      float r, m, bu, po, gl = 0.0f;
      if (float ab; sscanf(line.c_str(), " autobump %f", &ab) == 1) {
         t.autobump = std::max(0.0f, ab);
         continue;
      }
      const int n = sscanf(line.c_str(), " class %159s %f %f %f %f %f", a, &r, &m, &bu, &po, &gl);
      if (n >= 5) {
         Class c;
         c.name = a;
         c.rough = std::clamp(r, 0.0f, 1.0f);
         c.metal = std::clamp(m, 0.0f, 1.0f);
         c.bump = std::clamp(bu, 0.0f, 2.0f);
         c.polish = std::clamp(po, 0.0f, 1.5f);
         c.glow = n >= 6 ? std::max(0.0f, gl) : 0.0f;
         c.none = c.name == "none";
         c.window = c.name.compare(0, 6, "window") == 0;
         c.stained = c.name.compare(0, 7, "stained") == 0;
         c.hash = fnv(line.data(), line.size());
         byName[c.name] = int(t.classes.size());
         t.classes.push_back(c);
         continue;
      }
      if (sscanf(line.c_str(), " %159s %159s", a, b) == 2) {
         auto it = byName.find(b);
         if (it != byName.end())
            t.rules.push_back({a, it->second});
      }
   }
   // the fallback: no polish, bumps by autobump
   Class auto_;
   auto_.name = "auto";
   auto_.rough = 0.85f;
   auto_.bump = t.autobump;
   auto_.hash = fnv(&t.autobump, sizeof(t.autobump), 0x6175746fu);
   t.fallback = int(t.classes.size());
   t.classes.push_back(auto_);
   fprintf(stderr, "RT: material table %s: %zu classes, %zu rules\n", path.c_str(),
           t.classes.size(), t.rules.size());
}

inline const Class *class_of(const std::string &source)
{
   const Table &t = g_table;
   // A rule for a polished class (a mirror: marble, tile, glass) that names
   // no folder looks at the texture's own name, not its family's: The Black
   // Parade's fam/marbletex is its odds and ends -- rough black stone blocks
   // (bigbla), brick, trim -- and all of it mirrored the room as marble.
   const size_t slash = source.rfind('/');
   const char *own = source.c_str() + (slash == std::string::npos ? 0 : slash + 1);
   for (const Rule &r : t.rules) {
      const Class &c = t.classes[size_t(r.cls)];
      const bool byOwnName = c.polish > 0.0f && r.pattern.find('/') == std::string::npos;
      if (glob(r.pattern.c_str(), byOwnName ? own : source.c_str()))
         return &c;
   }
   return t.fallback >= 0 ? &t.classes[size_t(t.fallback)] : nullptr;
}

// ---- the maps ----------------------------------------------------------------

inline bool exists(const std::string &p)
{
   struct stat st;
   return stat(p.c_str(), &st) == 0;
}

// luminance 0..1 at two scales, wrapping (the textures tile)
inline std::vector<float> heights(const uint8_t *rgba, int w, int h, bool fineToo = true)
{
   std::vector<float> l(size_t(w) * h), o(size_t(w) * h);
   for (int i = 0; i < w * h; ++i) {
      const uint8_t *a = &rgba[size_t(i) * 4];
      l[size_t(i)] = (0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2]) / 255.0f;
   }
   // a box blur of radius r, wrapping, as a running sum along each row and
   // column (the same sums as adding up the 2r+1 texels at each one)
   auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
   auto blur = [&](const std::vector<float> &in, int r) {
      std::vector<float> tmp(in.size()), out(in.size());
      const float inv = 1.0f / float(2 * r + 1);
      for (int y = 0; y < h; ++y) {
         const float *row = &in[size_t(y) * w];
         double s = 0;
         for (int d = -r; d <= r; ++d)
            s += row[wrap(d, w)];
         for (int x = 0; x < w; ++x) {
            tmp[size_t(y) * w + x] = float(s) * inv;
            s += row[wrap(x + r + 1, w)] - row[wrap(x - r, w)];
         }
      }
      std::vector<double> col(size_t(w), 0.0);
      for (int d = -r; d <= r; ++d) {
         const float *row = &tmp[size_t(wrap(d, h)) * w];
         for (int x = 0; x < w; ++x)
            col[size_t(x)] += row[x];
      }
      for (int y = 0; y < h; ++y) {
         const float *add = &tmp[size_t(wrap(y + r + 1, h)) * w];
         const float *sub = &tmp[size_t(wrap(y - r, h)) * w];
         float *o = &out[size_t(y) * w];
         for (int x = 0; x < w; ++x) {
            o[x] = float(col[size_t(x)]) * inv;
            col[size_t(x)] += add[x] - sub[x];
         }
      }
      return out;
   };
   // (the fine scale no finer than a few texels of a big texture: grain at
   // the texel made a normal that changed from one pixel to the next, which
   // sparkled and crawled as the path tracer's samples moved over it -- a
   // grey stone wall's sheen shimmered standing still)
   // (a small texture -- an original 64 x 64, a fan mission's -- is not
   // blurred at its texel: there a brick is a few texels, and blurring by a
   // few more took its bumps away -- Black Parade's walls looked flat)
   const int big = std::clamp(std::min(w, h) / 48, 1, 6);
   const int small = std::clamp(std::min(w, h) / 192, 0, 3);
   const auto fine = small > 0 ? blur(blur(l, small), small) : l;
   const auto broad = blur(blur(l, big), big);
   const float fineW = small > 0 ? 0.35f : 0.5f;
   for (size_t i = 0; i < o.size(); ++i)
      o[i] = (fineToo ? fineW * fine[i] : 0.0f) + (1.0f - fineW) * broad[i] * 1.6f;
   return o;
}

// halved (2x2 texels averaged) until it is at most maxSize across
inline const uint8_t *shrink(const uint8_t *rgba, int w, int h, int maxSize,
                             std::vector<uint8_t> &store, int &ow, int &oh)
{
   ow = w;
   oh = h;
   const uint8_t *src = rgba;
   while (std::max(ow, oh) > maxSize && ow >= 2 && oh >= 2) {
      const int nw = ow / 2, nh = oh / 2;
      std::vector<uint8_t> next(size_t(nw) * nh * 4);
      for (int y = 0; y < nh; ++y)
         for (int x = 0; x < nw; ++x)
            for (int k = 0; k < 4; ++k) {
               const size_t a = (size_t(2 * y) * ow + 2 * x) * 4 + k, b = a + size_t(ow) * 4;
               next[(size_t(y) * nw + x) * 4 + k] = uint8_t((src[a] + src[a + 4] + src[b] + src[b + 4] + 2) / 4);
            }
      store.swap(next);
      src = store.data();
      ow = nw;
      oh = nh;
   }
   return src;
}

struct Result {
   bool polished = false;
   bool glows = false;     // has an emission map
   float glow = 0.0f;      // how much it lights its surroundings
   bool none = false;
};

// a stained glass pane: coloured or bright, not the dark lead between the
// panes (nor clear glass's dull grey)
inline bool stained_pane(const uint8_t *a)
{
   const int r = a[0], g = a[1], b = a[2];
   const int lum = (54 * r + 183 * g + 19 * b) >> 8;
   const int chroma = std::max({r, g, b}) - std::min({r, g, b});
   return a[3] >= 128 && lum > 40 && (chroma > 30 || lum > 140);
}

// Scale2x (EPX): twice the size, a pixel's corners taking a neighbour's
// colour where two of them meet along a diagonal -- edges between flat
// colours stay sharp and stair-steps round off, as stained glass's panes and
// lead are. (The texture tiles: its neighbours wrap round.)
inline std::vector<uint8_t> scale2x(const uint8_t *src, int w, int h)
{
   std::vector<uint8_t> out(size_t(w) * h * 16);
   auto px = [&](int x, int y) { return reinterpret_cast<const uint32_t *>(src)[size_t((y + h) % h) * w + (x + w) % w]; };
   uint32_t *o = reinterpret_cast<uint32_t *>(out.data());
   const int W = w * 2;
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
         const uint32_t P = px(x, y), A = px(x, y - 1), B = px(x + 1, y), C = px(x - 1, y), D = px(x, y + 1);
         o[size_t(2 * y) * W + 2 * x] = (C == A && C != D && A != B) ? A : P;
         o[size_t(2 * y) * W + 2 * x + 1] = (A == B && A != C && B != D) ? B : P;
         o[size_t(2 * y + 1) * W + 2 * x] = (D == C && D != B && C != A) ? C : P;
         o[size_t(2 * y + 1) * W + 2 * x + 1] = (B == D && B != A && D != C) ? D : P;
      }
   return out;
}

// a lit window's pane: bright and warm (lamplight behind it), not its frame
inline bool lit_pane(const uint8_t *a)
{
   const int r = a[0], g = a[1], b = a[2];
   const int lum = (54 * r + 183 * g + 19 * b) >> 8;
   return a[3] >= 128 && lum > 120 && r >= b + 25 && g >= b;
}

// The lit panes of a window texture: its lit_pane texels, if they make one.
// A shutter's or a frame's pale, warm wood (A Night in Rockburg's
// fam/wind/win13: a wooden shutter in a "win" family) passes texel by texel
// too -- each of them glowed, speckles of light all over the wood. Lit panes
// stand out (twice as bright as the rest of the texture, or near it) and lie
// together (most of them with lit neighbours); the wood's palest grain is
// barely brighter than the rest, scattered all over. Measured on Thief 2's
// city windows and the FM's: their panes 2.1-4.1x as bright, 57-100% with 3
// or more lit neighbours; the shutter's 1.4x and 39%. The texture tiles:
// neighbours wrap.
inline std::vector<uint8_t> lit_pane_mask(const uint8_t *rgba, int w, int h)
{
   const size_t n = size_t(w) * h;
   std::vector<uint8_t> raw(n);
   double litLum = 0, restLum = 0;
   size_t lit = 0;
   for (size_t i = 0; i < n; ++i) {
      const uint8_t *a = &rgba[i * 4];
      raw[i] = lit_pane(a);
      const double lum = (54 * a[0] + 183 * a[1] + 19 * a[2]) >> 8;
      (raw[i] ? litLum : restLum) += lum;
      lit += raw[i];
   }
   if (lit == 0 || lit == n)
      return raw;
   litLum /= double(lit);
   restLum /= double(n - lit);
   size_t together = 0;
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
         if (!raw[size_t(y) * w + x])
            continue;
         int k = 0;
         for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
               k += (dx || dy) && raw[size_t((y + dy + h) % h) * w + size_t((x + dx + w) % w)];
         together += k >= 3;
      }
   const bool panes = litLum >= 1.8 * std::max(restLum, 1.0) && double(together) >= 0.45 * double(lit);
   if (!panes)
      std::fill(raw.begin(), raw.end(), uint8_t(0));
   return raw;
}

// the mark of a window texture's glow made with lit_pane_mask (made before
// it, its glow is made again)
constexpr const char *kPaneMark = "_e.w2";

// Crevices: the texture's joints and cracks (darker than its average, as
// the cavities in the occlusion channel) darkened in its colour as well. The
// occlusion channel only darkens a surface's ambient glow in RTGL1's shaders,
// which in Build's sector-lit levels is most of their light and in Thief's
// lamp-lit ones next to none: so mortar between bricks reads as recessed
// under the lamps too. strength 0..1; a flat class is left alone.
inline void crevice_albedo(std::vector<uint8_t> &rgba, int w, int h, const Class &c, float strength)
{
   if (strength <= 0 || c.bump <= 0 || c.none || w < 4 || h < 4)
      return;
   const auto raw = heights(rgba.data(), w, h);
   // (over 3x3 texels: a crack a texel wide, darkened alone, was a hard line
   // that shimmered as the view moved and the upscaler jittered across it)
   std::vector<float> hg(raw.size());
   for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
         float sum = 0;
         for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
               sum += raw[size_t((y + dy + h) % h) * w + size_t((x + dx + w) % w)] * (dx == 0 && dy == 0 ? 4.0f : (dx == 0 || dy == 0) ? 2.0f : 1.0f);
         hg[size_t(y) * w + x] = sum / 16.0f;
      }
   double avg = 0;
   for (float v : hg)
      avg += v;
   avg /= double(std::max<size_t>(1, hg.size()));
   const float b = std::clamp(c.bump / 1.2f, 0.0f, 1.0f);
   for (size_t i = 0; i < hg.size(); ++i) {
      const float rel = std::clamp(float(hg[i] / std::max(avg, 0.02)), 0.0f, 2.0f);
      const float d = std::clamp(1.0f - rel, 0.0f, 1.0f);
      const float f = 1.0f - strength * 0.85f * b * d * (2.0f - d);
      for (int k = 0; k < 3; ++k)
         rgba[i * 4 + k] = uint8_t(std::clamp(rgba[i * 4 + k] * f + 0.5f, 0.0f, 255.0f));
   }
}

// Whether a texture's maps are on disk already (prepare would only look).
inline bool maps_made(const std::string &dir, const std::string &name, const Class &c)
{
   if (c.none)
      return true;
   const std::string base = dir + "/" + name;
   if (!exists(base + "_orm.png") || (!exists(base + "_e.png") && !exists(base + "_e.none")))
      return false;
   if (c.window && !c.stained && c.glow > 0 && !exists(base + kPaneMark))
      return false;
   return c.bump <= 0 || (exists(base + "_n.png") && exists(base + "_h.png"));
}

// Makes the maps of a texture (if they are not there yet) and says what it is.
// glowMap: a NewDark illum_map's RGBA (w x h of its own), or null.
inline Result prepare(const std::string &dir, const std::string &name, const Class &c,
                      const uint8_t *rgba, int w, int h,
                      const std::vector<uint8_t> *glowMap, int gw, int gh, float glowStrength,
                      const std::vector<uint8_t> *glass = nullptr, int glw = 0, int glh = 0)
{
   Result res;
   res.none = c.none;
   if (c.none || w < 4 || h < 4)
      return res;
   // the maps' own sizes (kMapMax, kGlowMax)
   std::vector<uint8_t> smallStore, glowStore;
   int ew = w, eh = h;
   const uint8_t *eRgba = shrink(rgba, w, h, kGlowMax, glowStore, ew, eh);
   rgba = shrink(eRgba, ew, eh, kMapMax, smallStore, w, h);
   const size_t EN = size_t(ew) * eh;
   const std::string base = dir + "/" + name;
   const std::string pE = base + "_e.png", pORM = base + "_orm.png", pN = base + "_n.png",
                     pH = base + "_h.png", pNoE = base + "_e.none";
   const size_t N = size_t(w) * h;
   // a window's glow made before its panes had to lie together: made again
   if (c.window && !c.stained && c.glow > 0 && !exists(base + kPaneMark)) {
      std::remove(pE.c_str());
      std::remove(pNoE.c_str());
   }
   std::vector<float> hgt;
   auto H = [&]() -> const std::vector<float> & {
      if (hgt.empty())
         hgt = heights(rgba, w, h);
      return hgt;
   };

   // emission
   if (exists(pE))
      res.glows = true;
   else if (!exists(pNoE)) {
      std::vector<uint8_t> e(EN * 3, 0);
      int lit = 0;
      if (glowMap && gw > 0 && gh > 0) {
         // the material's glow map, scaled to the texture
         const float k = std::clamp(glowStrength * 2.5f, 0.25f, 1.0f);
         for (int y = 0; y < eh; ++y)
            for (int x = 0; x < ew; ++x) {
               const uint8_t *g = &(*glowMap)[(size_t(std::min(gh - 1, y * gh / eh)) * gw +
                                               std::min(gw - 1, x * gw / ew)) * 4];
               uint8_t *o = &e[(size_t(y) * ew + x) * 3];
               for (int k3 = 0; k3 < 3; ++k3)
                  o[k3] = uint8_t(std::clamp(g[k3] * k * g[3] / 255.0f, 0.0f, 255.0f));
               lit += (o[0] | o[1] | o[2]) > 8;
            }
      } else if (c.stained && c.glow > 0) {
         // the light through each pane, in its colour (a little richer:
         // glass lit from behind is more saturated than its painted texture)
         for (size_t i = 0; i < EN; ++i) {
            const uint8_t *a = &eRgba[i * 4];
            if (!stained_pane(a))
               continue;
            const float mean = (a[0] + a[1] + a[2]) / 3.0f;
            for (int k3 = 0; k3 < 3; ++k3)
               e[i * 3 + k3] = uint8_t(std::clamp(mean + (a[k3] - mean) * 1.25f, 0.0f, 255.0f));
            ++lit;
         }
         // mostly lead or dull glass: not stained glass
         if (lit < int(EN / 20))
            lit = 0;
      } else if (c.window && c.glow > 0) {
         const std::vector<uint8_t> pane = lit_pane_mask(eRgba, ew, eh);
         for (size_t i = 0; i < EN; ++i) {
            const uint8_t *a = &eRgba[i * 4];
            if (!pane[i])
               continue;
            for (int k3 = 0; k3 < 3; ++k3)
               e[i * 3 + k3] = uint8_t(std::min(255.0f, a[k3] * 1.15f));
            ++lit;
         }
         // a few lit texels are a highlight, not a lit window (the lit
         // windows of the original's city are 2-25% pane)
         if (lit < int(EN / 100))
            lit = 0;
      }
      if (lit > 0 && write_png(pE, e.data(), ew, eh, 3))
         res.glows = true;
      else if (FILE *f = fopen(pNoE.c_str(), "wb"))
         fclose(f);
      if (c.window && !c.stained && c.glow > 0)
         if (FILE *f = fopen((base + kPaneMark).c_str(), "wb"))
            fclose(f);
   }
   if (res.glows)
      res.glow = glowMap ? std::max(0.3f, glowStrength * 2.0f) : std::max(0.3f, c.glow * 0.25f);

   // roughness, metal, cavities
   if (!exists(pORM)) {
      const auto &hg = H();
      double avg = 0;
      for (float v : hg)
         avg += v;
      avg /= double(std::max<size_t>(1, hg.size()));
      std::vector<uint8_t> orm(N * 3);
      for (size_t i = 0; i < N; ++i) {
         const float rel = std::clamp(float(hg[i] / std::max(avg, 0.02)), 0.0f, 2.0f);
         float r;
         // a window's lit pane keeps its glow (a mirror over it would dim it);
         // its dark panes reflect the street -- only its glass, where it has
         // a glass mask: not the frame or the wall around it
         float polish = (c.window && lit_pane(&rgba[i * 4])) ? std::min(c.polish, 0.15f) : c.polish;
         if (glass) {
            const int x = int(i % size_t(w)), y = int(i / size_t(w));
            if ((*glass)[size_t(std::min(glh - 1, y * glh / h)) * glw + std::min(glw - 1, x * glw / w)] < 128)
               polish = 0.0f;
         }
         float metal = c.metal;
         if (c.stained) {
            // glass: a sheen over each pane (the room's lamps caught in it,
            // not a mirror); the lead between them dull
            const bool pane = stained_pane(&rgba[i * 4]);
            r = pane ? 0.22f : 0.8f;
            metal = pane ? 0.0f : 0.35f;
         } else if (polish > 0)
            r = 0.7f - std::clamp(polish * (0.85f + 0.15f * rel), 0.0f, 3.0f) / 4.5f;
         else
            r = std::max(c.rough + (1.0f - rel) * 0.1f, 0.71f);
         const float depthRel = std::clamp(1.0f - rel, 0.0f, 1.0f);
         const float occ = 1.0f - 0.6f * std::clamp(c.bump / 1.2f, 0.0f, 1.0f) * depthRel * (2.0f - depthRel);
         orm[i * 3] = uint8_t(std::clamp(occ * 255.0f + 0.5f, 0.0f, 255.0f));
         orm[i * 3 + 1] = uint8_t(std::clamp(r * 255.0f + 0.5f, 0.0f, 255.0f));
         orm[i * 3 + 2] = uint8_t(std::clamp(metal * (0.75f + 0.25f * rel) * 255.0f + 0.5f, 0.0f, 255.0f));
      }
      write_png(pORM, orm.data(), w, h, 3);
   }
   res.polished = c.polish > 0;
   if (glass && res.polished) {
      // (a mask with no glass at all: an empty opening, nothing mirrors)
      bool any = false;
      for (uint8_t v : *glass)
         if (v >= 128) { any = true; break; }
      res.polished = any;
   }

   // bumps and heights
   if (c.bump > 0) {
      if (!exists(pN)) {
         // A polished surface (marble, glazed tile): only its broad relief
         // (a joint, a worn dip), gently. Its mirror reflection is bent by
         // every bump: the grain of the stone scattered it into noise -- with
         // bumps off, the reflection came out nearly clear.
         std::vector<float> smooth;
         if (c.polish > 0)
            smooth = heights(rgba, w, h, false);
         else if (c.stained) {
            // stained glass: the lead stands out of the glass (raised where
            // the texture is dark), each pane's own ripple over it, gently
            smooth = H();
            for (size_t i = 0; i < N; ++i)
               smooth[i] = (stained_pane(&rgba[i * 4]) ? 0.0f : 0.5f) + 0.25f * smooth[i];
         }
         const auto &hg = (c.polish > 0 || c.stained) ? smooth : H();
         std::vector<uint8_t> n(N * 3);
         // a bigger texture has more texels to the same stone: smaller steps
         const float k = (c.polish > 0 ? 0.35f : 1.0f) * 17.0f * c.bump * std::sqrt(std::clamp(float(std::min(w, h)) / 64.0f, 0.5f, 4.0f));
         for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
               auto at = [&](int xx, int yy) { return hg[size_t(((yy + h) % h) * w + (xx + w) % w)]; };
               const float dx = (at(x + 1, y) - at(x - 1, y)) * 0.5f;
               const float dy = (at(x, y + 1) - at(x, y - 1)) * 0.5f;
               float nx = -dx * k, ny = dy * k, nz = 1.0f;
               const float il = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
               nx *= il; ny *= il; nz *= il;
               uint8_t *o = &n[(size_t(y) * w + x) * 3];
               o[0] = uint8_t(std::clamp((nx * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
               o[1] = uint8_t(std::clamp((ny * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
               o[2] = uint8_t(std::clamp((nz * 0.5f + 0.5f) * 255.0f + 0.5f, 0.0f, 255.0f));
            }
         write_png(pN, n.data(), w, h, 3);
      }
      if (!exists(pH)) {
         const auto &hg = H();
         float lo = 1e9f, hi = -1e9f;
         for (float v : hg) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
         }
         const float span = std::max(hi - lo, 1e-3f);
         const float depth = std::clamp(c.bump / 1.2f, 0.0f, 1.0f);
         std::vector<uint8_t> g(N);
         for (size_t i = 0; i < N; ++i)
            g[i] = uint8_t(std::clamp((1.0f - depth * (1.0f - (hg[i] - lo) / span)) * 255.0f + 0.5f, 0.0f, 255.0f));
         write_png(pH, g.data(), w, h, 1);
      }
   }
   return res;
}

// A flame sprite's emission: its own colour within its soft shape.
inline bool prepare_flame(const std::string &dir, const std::string &name, const uint8_t *rgba, int w, int h)
{
   const std::string pE = dir + "/" + name + "_e.png";
   if (exists(pE))
      return true;
   // its colour within its soft shape, brought up to full brightness at its
   // brightest texel (up to 4x): an HD pack's dim, thin particle frames
   // otherwise glowed hardly at all
   std::vector<float> f(size_t(w) * h * 3);
   float peak = 0.0f;
   for (size_t i = 0; i < size_t(w) * h; ++i)
      for (int k = 0; k < 3; ++k) {
         f[i * 3 + k] = rgba[i * 4 + k] * (rgba[i * 4 + 3] / 255.0f);
         peak = std::max(peak, f[i * 3 + k]);
      }
   const float gain = peak > 0.0f ? std::clamp(255.0f / peak, 1.0f, 4.0f) : 1.0f;
   std::vector<uint8_t> e(f.size());
   for (size_t i = 0; i < f.size(); ++i)
      e[i] = uint8_t(std::min(255.0f, f[i] * gain + 0.5f));
   return write_png(pE, e.data(), w, h, 3);
}

// A file name of the texture's path: "fam/city/marble" -> "fam_city_marble"
inline std::string file_safe(const std::string &s)
{
   std::string o;
   for (char ch : s)
      // (no dots: RTGL1 would take the rest of the name for an extension)
      o += (isalnum((unsigned char)ch) || ch == '-') ? char(tolower((unsigned char)ch)) : '_';
   if (o.size() > 100)
      o = o.substr(o.size() - 100);
   return o;
}

} // namespace rtmat
