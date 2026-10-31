#include <resimage.h>
#include <resbastm.h>
#include <tmgr.h>
#include <mode.h>
#include <mdutil.h>

#include <bmp.h>
#include <cel.h>
#include <dds.h>
#include <gif.h>
#include <imagefmt.h>
#include <png.h>
#include <pcx.h>
#include <psd.h>
#include <tga.h>
#include <bitmap.h>
#include <restruecolor.h>
#include <resimginfo.h>
#include <ctype.h>
#include <stdio.h>

ulong gResImageFlat16Format = 0;
ushort gResImageChromaKey = 0;

void ImageHackRemap16Bit(grs_bitmap* pBm, uint8 mip);
void ImageHackAutoSetTransparency(grs_bitmap* pBm, uint8 mip);
void FlagsXorMip(grs_bitmap* pBm, int16 flag);
void FlagsOrMip(grs_bitmap* pBm, int16 flag);

cImageResource::cImageResource(IStore* pStore, const char* pName, IResType* pType, eImgKind kind)
	: cResourceBase{ pStore, pName, pType }, m_Kind{ kind } {}


// ---- the image's name and material file (resimginfo.h) ---------------------

// "…/TheBlackParade_1.0.1/fam/mech/" + "dal01" -> "fam/mech/dal01": the path
// from the resource root (the first of fam/obj/mesh/... from the end), lower
// case, a resource archive's ".crf" dropped
static void ImageResourcePath(const char* storePath, const char* name, char* out, size_t size)
{
	char full[512];
	snprintf(full, sizeof(full), "%s%s%s", storePath ? storePath : "",
		(storePath && *storePath && storePath[strlen(storePath) - 1] != '/' &&
		 storePath[strlen(storePath) - 1] != '\\') ? "/" : "", name ? name : "");
	for (char* c = full; *c; ++c)
		*c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);
	// fam.crf/mech/x -> fam/mech/x
	for (char* c; (c = strstr(full, ".crf/")) != nullptr; )
		memmove(c, c + 4, strlen(c + 4) + 1);
	static const char* roots[] = { "/fam/", "/obj/", "/mesh/", "/bitmap/", "/intrface/", "/book/", "/books/", "/pal/" };
	const char* best = nullptr;
	for (const char* r : roots)
		for (const char* at = strstr(full, r); at; at = strstr(at + 1, r))
			if (!best || at > best)
				best = at;
	const char* from = best ? best + 1 : full;
	if (!best)
	{
		// the last two components
		const char* last = strrchr(full, '/');
		if (last)
		{
			const char* prev = last;
			while (prev > full && prev[-1] != '/')
				--prev;
			from = prev > full ? prev : full;
			if (prev > full)
			{
				const char* p2 = prev - 1;
				while (p2 > full && p2[-1] != '/')
					--p2;
				from = p2;
			}
		}
	}
	snprintf(out, size, "%s", from);
	// no extension
	char* dot = strrchr(out, '.');
	char* slash = strrchr(out, '/');
	if (dot && (!slash || dot > slash))
		*dot = 0;
}

static char* ReadWholeStream(IStore* pStore, const char* fileName, long* pSize)
{
	if (!pStore)
		return nullptr;
	IStoreStream* pStream = pStore->OpenStream(fileName, 0);
	if (!pStream)
		return nullptr;
	const long size = pStream->GetSize();
	char* text = nullptr;
	if (size > 0 && size < 1 << 20)
	{
		text = (char*)malloc((size_t)size + 1);
		if (text)
		{
			const long got = pStream->Read(size, text);
			text[got > 0 ? got : 0] = 0;
			*pSize = got;
		}
	}
	pStream->Close();
	pStream->Release();
	return text;
}

// NewDark's material file: the parameters the engine acts on. .ifdef/.ifndef
// take DX6 (false here) or a $config var (taken as not set); render passes
// ("render_pass { ... }") are skipped.
static void ParseMaterialFile(IStore* pStore, const char* fileName, sResImageInfo* info, int depth)
{
	long size = 0;
	char* text = ReadWholeStream(pStore, fileName, &size);
	if (!text)
		return;
	info->hasMaterial = 1;
	int skipDepth = 0, condDepth = 0, braces = 0;
	char* save = nullptr;
	for (char* line = strtok_r(text, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save))
	{
		while (*line == ' ' || *line == '\t' || (unsigned char)*line >= 0x80)   // (and a UTF-8 BOM)
			++line;
		// a render pass closed on the line of the next parameter ("}tile_factor 1 -2")
		while (*line == '}')
		{
			if (braces > 0) --braces;
			++line;
			while (*line == ' ' || *line == '\t')
				++line;
		}
		if (!*line || *line == '#' || (line[0] == '/' && line[1] == '/'))
			continue;
		char key[64] = "", a[200] = "", b[200] = "";
		const int n = sscanf(line, "%63s %199s %199s", key, a, b);
		if (n < 1)
			continue;
		// conditional blocks
		if (!strcmp(key, ".ifdef") || !strcmp(key, ".ifndef"))
		{
			++condDepth;
			const bool truth = strcmp(a, "0") != 0 && a[0] != '$' && _stricmp(a, "DX6") != 0;
			const bool take = !strcmp(key, ".ifdef") ? truth : !truth;
			if (skipDepth || !take)
				++skipDepth;
			continue;
		}
		if (!strcmp(key, ".else"))
		{
			if (skipDepth == 1) skipDepth = 0;
			else if (skipDepth == 0) skipDepth = 1;
			continue;
		}
		if (!strcmp(key, ".endif"))
		{
			if (condDepth > 0) --condDepth;
			if (skipDepth > 0) --skipDepth;
			continue;
		}
		if (skipDepth)
			continue;
		// render passes: their contents are not the base surface's
		const bool inPass = braces > 0;
		for (const char* c = line; *c; ++c)
			braces += *c == '{' ? 1 : *c == '}' ? -1 : 0;
		if (braces < 0) braces = 0;
		if (inPass || braces > 0 || !strcmp(key, "render_pass"))
			continue;
		if (!_stricmp(key, "include") && n >= 2 && depth < 4)
		{
			// in the same relative path
			char inc[200];
			snprintf(inc, sizeof(inc), "%s", a[0] == '"' ? a + 1 : a);
			if (char* q = strchr(inc, '"')) *q = 0;
			ParseMaterialFile(pStore, inc, info, depth + 1);
		}
		else if (!_stricmp(key, "tile_factor") && n >= 2)
		{
			const float u = (float)atof(a), v = n >= 3 ? (float)atof(b) : u;
			// a factor of n: the image holds n tiles of the original; -n: 1/n
			auto scale = [](float f) { return f > 0 ? 1.0f / f : f < 0 ? -f : 1.0f; };
			info->tileU = scale(u);
			info->tileV = scale(v);
		}
		else if (!_stricmp(key, "ani_rate") && n >= 2)
			info->aniRate = atoi(a);
		else if (!_stricmp(key, "ani_mode") && n >= 2)
		{
			int flags = 0;
			char* modes = line + strlen(key);
			if (strstr(modes, "WRAP") || strstr(modes, "wrap")) flags |= 1;
			if (strstr(modes, "REVERSE") || strstr(modes, "reverse")) flags |= 2;
			if (strstr(modes, "PINGPONG") || strstr(modes, "pingpong")) flags |= 4;
			info->aniFlags = flags;
		}
		else if (!_stricmp(key, "force_ani_settings"))
			info->forceAniSettings = n < 2 || atoi(a) != 0;
		else if (!_stricmp(key, "force_opaque"))
			info->forceOpaque = n < 2 || atoi(a) != 0;
		else if (!_stricmp(key, "illum_map") && n >= 3)
		{
			info->illumStrength = (float)atof(a);
			char path[200];
			snprintf(path, sizeof(path), "%s", b[0] == '"' ? b + 1 : b);
			if (char* q = strchr(path, '"')) *q = 0;
			for (char* c = path; *c; ++c)
				*c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);
			snprintf(info->illumMap, sizeof(info->illumMap), "%s", path);
		}
		else if (!_stricmp(key, "terrain_scale") && n >= 2)
		{
			info->terrainScaleW = atoi(a);
			info->terrainScaleH = n >= 3 ? atoi(b) : info->terrainScaleW;
		}
	}
	free(text);
}

static void NoteImageInfo(const grs_bitmap* bm, IStore* pStore, const char* name)
{
	sResImageInfo* info = (sResImageInfo*)calloc(1, sizeof(sResImageInfo));
	if (!info)
		return;
	info->tileU = info->tileV = 1.0f;
	info->aniFlags = -1;
	const char* storePath = pStore ? pStore->GetFullPathName() : "";
	ImageResourcePath(storePath, name, info->name, sizeof(info->name));
	// the material file: the image's name with .mtl, beside it
	char mtl[200];
	snprintf(mtl, sizeof(mtl), "%s", name ? name : "");
	char* dot = strrchr(mtl, '.');
	if (dot) *dot = 0;
	strncat(mtl, ".mtl", sizeof(mtl) - strlen(mtl) - 1);
	ParseMaterialFile(pStore, mtl, info, 0);
	ResImageInfoRegister(bm, info);
}

// for C callers (anim_txt.c): the material file's animation settings.
// Returns 0 if it gives none. flags: 1 wrap, 2 reverse, 4 pingpong.
extern "C" int ResImageMaterialAnim(const void* bitmap, int* rate, int* flags, int* force)
{
	sResImageInfo info;
	if (!ResImageInfoFind(bitmap, &info) || (info.aniRate <= 0 && info.aniFlags < 0))
		return 0;
	*rate = info.aniRate;
	*flags = info.aniFlags;
	*force = info.forceAniSettings;
	return 1;
}

// the material file's tile_factor as a UV scale (1, 1: none)
extern "C" int ResImageMaterialTile(const void* bitmap, float* su, float* sv)
{
	sResImageInfo info;
	*su = *sv = 1.0f;
	if (!ResImageInfoFind(bitmap, &info))
		return 0;
	*su = info.tileU > 0 ? info.tileU : 1.0f;
	*sv = info.tileV > 0 ? info.tileV : 1.0f;
	return *su != 1.0f || *sv != 1.0f;
}

// A copy of an image made elsewhere (texmem's mip-mapped world textures)
// takes its name, material and full colour with it. The copy's colour is its
// own allocation, freed by ResImageFreeCopyAttachments.
extern "C" void ResImageCopyAttachments(const void* from, const void* to, int w, int h)
{
	if (!from || !to || from == to)
		return;
	sResImageInfo info;
	if (ResImageInfoFind(from, &info))
	{
		sResImageInfo* copy = (sResImageInfo*)malloc(sizeof(sResImageInfo));
		if (copy)
		{
			*copy = info;
			ResImageInfoRegister(to, copy);
		}
	}
	const unsigned char* rgba = ResTrueColorFind(from);
	if (rgba && w > 0 && h > 0)
	{
		unsigned char* copy = (unsigned char*)malloc((size_t)w * h * 4);
		if (copy)
		{
			memcpy(copy, rgba, (size_t)w * h * 4);
			ResTrueColorRegister(to, copy);
		}
	}
}

extern "C" void ResImageFreeCopyAttachments(const void* bitmap)
{
	const unsigned char* rgba = ResTrueColorFind(bitmap);
	ResTrueColorForget(bitmap);
	free((void*)rgba);
	ResImageInfoForget(bitmap);
}

void* cImageResource::LoadData(ulong* pSize, ulong* pTimestamp, IResMemOverride* pResMem)
{
	cAutoResThreadLock lock{};

	if (!pResMem)
		return nullptr;

	auto* pStream = OpenStream();
	if (!pStream)
	{
		CriticalMsg1("Unable to open stream: %s", GetName());
		return nullptr;
	}

	grs_bitmap* bm = nullptr;
	const eImageContent content = DetectImageContent(pStream);
	switch (content)
	{
	case eImageContent::kDDS:
		bm = ResDdsReadImage(pStream, pResMem);
		break;
	case eImageContent::kGIF:
		bm = ResGifReadImage(pStream, pResMem);
		break;
	case eImageContent::kPCX:
		bm = ResPcxReadImage(pStream, pResMem);
		break;
	case eImageContent::kPNG:
	case eImageContent::kTIFF:
		// WIC selects its decoder from the stream signature, so TIFF assets
		// work even when a NewDark FM gives them a historical .gif name.
		bm = ResPngReadImage(pStream, pResMem);
		break;
	case eImageContent::kPSD:
		bm = ResPsdReadImage(pStream, pResMem);
		break;
	default:
		break;
	}

	// CEL, BMP, and TGA do not have signatures covered above.  Retain the
	// resource type selected from their extension as the fallback.
	if (!bm)
	{
		switch (m_Kind)
		{
	case eImgKind::kImgBMP:
		bm = ResBmpReadImage(pStream, pResMem);
		break;
	case eImgKind::kImgCEL:
		bm = ResCelReadImage(pStream, pResMem);
		break;
	case eImgKind::kImgDDS:
		bm = ResDdsReadImage(pStream, pResMem);
		break;
	case eImgKind::kImgGIF:
		bm = ResGifReadImage(pStream, pResMem);
		break;
	case eImgKind::kImgPNG:
		bm = ResPngReadImage(pStream, pResMem);
		break;
	case eImgKind::kImgPCX:
		bm = ResPcxReadImage(pStream, pResMem);
		break;
	case eImgKind::kImgTGA:
		bm = ResTgaReadImage(pStream, pResMem);
		break;
	default:
		break;
		}
	}

	if (pTimestamp)
		*pTimestamp = pStream->LastModified();

	pStream->Close();
	pStream->Release();
	if (!bm)
		return nullptr;

	if ((content == eImageContent::kDDS || content == eImageContent::kPNG ||
	     content == eImageContent::kTIFF ||
	     content == eImageContent::kPSD) && bm->type == BMT_FLAT16)
	{
		// The true-colour codecs emit explicitly tagged RGB565, ARGB1555, or
		// ARGB4444 pixels.  ImageHackRemap16Bit is a legacy heuristic for
		// untagged 15/16-bit art; applying it to ARGB1555 interprets the alpha
		// bit as colour data and changes the format tag.  BP's Dial/Window
		// animation frames take that path, producing the conspicuous false
		// colours.  The flat16 and hardware blitters understand these explicit
		// formats directly.
		const ulong format = bm->flags & 0x700;
		if (format == BMF_RGB_1555 || format == BMF_RGB_4444)
			bm->flags |= BMF_TRANS;
	}
	else
	{
		ImageHackRemap16Bit(bm, 0);
		ImageHackAutoSetTransparency(bm, 0);
	}
	NoteImageInfo(bm, m_pStore, m_pName);
	if (pSize)
		*pSize = pResMem->GetSize(bm);

	return bm;
}

BOOL cImageResource::FreeData(void* pUntypedData, ulong nSize, IResMemOverride* pResMem)
{
	cAutoResThreadLock lock{};

	if (!pUntypedData)
		CriticalMsg("FreeData: nothing to free!");

	auto* pData = reinterpret_cast<grs_bitmap*>(pUntypedData);

	if (!m_pStream && (pData->flags & 0x40) && g_tmgr)
		g_tmgr->unload_texture(pData);
	ResTrueColorForget(pData);
	ResImageInfoForget(pData);

	pResMem->ResFree(pData);

	return TRUE;
}

int cImageResource::ExtractPartial(const long nStart, const long nEnd, void* pBuf)
{
	cAutoResThreadLock lock{};
	long nNumToRead = 0;
	long nSize = 0;
	auto bLocked = false;

	if (!pBuf)
		return 0;

	auto* pData = m_pResMan->FindResource(this, &nSize);
	if (!pData || !nSize)
	{
		pData = m_pResMan->LockResource(this);
		if (!pData)
			return 0;

		bLocked = true;

		nSize = m_pResMan->GetResourceSize(this);
	}

	if (nEnd < nSize)
	{
		nNumToRead = nEnd - nStart + 1;
		memmove(pBuf, (char*)pData + nStart, nNumToRead);
	}
	else
	{
		nNumToRead = nSize - nStart;
		memmove(pBuf, (char*)pData + nStart, nSize - nStart);
	}

	if (bLocked)
		m_pResMan->UnlockResource(this);

	return nNumToRead;
}

void cImageResource::ExtractBlocks(void* pBuf, const long nSize, tResBlockCallback callback, void* pCallbackData)
{
	long nRemain = 0, nMove = 0, nIx = 0;
	auto bLocked = false;

	auto* pData = m_pResMan->FindResource(this, &nRemain);

	if (!pData || !nRemain)
	{
		pData = m_pResMan->LockResource(this);
		if (!pData)
			return;

		bLocked = true;

		nRemain = m_pResMan->GetResourceSize(this);
	}

	while (nRemain > 0)
	{
		if (nRemain >= nSize)
		{
			nMove = nSize;
			memmove(pBuf, pData, nSize);
		}
		else
		{
			nMove = nRemain;
			memmove(pBuf, pData, nRemain);
		}

		callback(this, pBuf, nMove, nIx++, pCallbackData);

		pData = static_cast<char*>(pData) + nMove;
		nRemain -= nMove;
	}

	if (bLocked)
		m_pResMan->UnlockResource(this);
}

void ImageHackRemap16Bit(grs_bitmap* pBm, uint8 mip)
{
	if (pBm->type != BMT_FLAT16)
		return;

	auto format16 = gResImageFlat16Format;
	if (!gResImageFlat16Format && grd_mode_info[grd_mode].bitDepth > 8u)
		format16 = grd_mode_info[grd_mode].bitDepth != 15 ? 1024 : 768;
	if (format16 && (pBm->flags & format16 & 0x700) == 0)
	{
		auto* pStart = pBm->bits;
		auto* pBits = (uint16*)&pBm->bits[2 * pBm->h * pBm->w];
		if (mip)
			pBits = (uint16*)&pStart[md_sizeof_mipmap_bits(pBm)];
		if ((pBm->flags & 0x400) != 0)
		{
			while (--pBits >= (uint16*)pStart)
			{
				*pBits = *pBits;
				*pBits = ((uint8)(*pBits >> 8) >> 3 << 10) | *pBits & 0x83FF;
				*pBits = (32 * ((((*pBits >> 5) & 0x3F) >> 1) & 0x1F)) | *pBits & 0xFC1F;
			}
		}
		else
		{
			while (--pBits >= (uint16*)pStart)
			{
				*pBits = *pBits;
				*pBits = (((*pBits >> 10) & 0x1F) << 11) | *pBits & 0x7FF;
				*pBits = (32 * ((63 * ((*pBits >> 5) & 0x1F) / 31) & 0x3F)) | *pBits & 0xF81F;
			}
		}
		if (mip)
			FlagsXorMip(pBm, 1792);
		else
			pBm->flags ^= 0x700u;
	}
}

void ImageHackAutoSetTransparency(grs_bitmap* pBm, uint8 mip)
{
	int16 flag = 0;

	if (pBm->type == BMT_FLAT8)
	{
		auto* pBits = &pBm->bits[pBm->h * pBm->w];
		while (--pBits >= pBm->bits)
		{
			if (!*pBits)
			{
				flag = 1;
				break;
			}
		}
	}
	else if (pBm->type == BMT_FLAT16)
	{
		auto* pBits = (uint16*)&pBm->bits[2 * pBm->h * pBm->w];
		while (--pBits >= (uint16*)pBm->bits)
		{
			if (*pBits == gResImageChromaKey)
			{
				flag = 1;
				break;
			}
		}
	}

	if (flag)
	{
		if (mip)
			FlagsOrMip(pBm, flag);
		else
			pBm->flags |= flag;
	}
}

void FlagsXorMip(grs_bitmap* pBm, int16 flag)
{
	while (true)
	{
		pBm->flags ^= flag;
		if (pBm->w == 1 && pBm->h == 1)
			break;
		++pBm;
	}
}

void FlagsOrMip(grs_bitmap* pBm, int16 flag)
{
	while (true)
	{
		pBm->flags |= flag;
		if (pBm->w == 1 && pBm->h == 1)
			break;
		++pBm;
	}
}
