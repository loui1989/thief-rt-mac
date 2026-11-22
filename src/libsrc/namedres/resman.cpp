#include <storeapi.h>
#include <storeman.h>
#include <resman.h>
#include <hshsttem.h>

#ifndef SHIP
BOOL g_fResPrintAccesses;
BOOL g_fResPrintDrops;
#endif

#include <resthred.h>
#include <filespec.h>
#include <str.h>
#include <mprintf.h>
#include <appagg.h>
#include <binrstyp.h>
#include <zipstfct.h>
#include <pathutil.h>
#include <search.h>
#include <cstdlib>
#include <missiondiag.h>
#include <platform_services.h>
#pragma pack(push, 8)
#include <mutex>
#pragma pack(pop)
#include <string>
#include <vector>
#pragma pack(push, 8)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#pragma pack(pop)
#include <imagefmt.h>
#include <imgrstyp.h>
#include <png.h>
#include <dds.h>
#include <tga.h>
#include <pcx.h>
#include <gif.h>
#include <bitmap.h>
#include <restruecolor.h>

namespace
{
// ---- an HD pack's texture over a fan mission's low-resolution copy ----
//
// A fan mission ships many stock textures again (Black Parade: fam/core,
// fam/city, ... as 64 x 64 PNGs), and its own files come first -- so an HD
// pack's version of the same texture was never used, and the walls stayed
// low resolution (and nearly without bumps). Where a later root (a mod) has
// the texture too and the mission's copy is the original game's picture
// (its layout and colours match the original's), the mod's is taken: the HD
// pack is a remake of that original. A texture the mission's author changed
// keeps the mission's. Decisions are kept in <game>/hd_over_fm.txt.

struct sThumb
{
	int w = 0, h = 0;
	float c[16 * 16 * 3] = {};
	float wt[16 * 16] = {};
};

bool ThumbOf(IStore* pStore, const char* pName, IResMemOverride* pMem, sThumb& t)
{
	IStoreStream* s = pStore->OpenStream(pName, 0);
	if (!s)
		return false;
	grs_bitmap* bm = nullptr;
	uint8* pal = nullptr;
	const char* ext = strrchr(pName, '.');
	switch (DetectImageContent(s))
	{
	case eImageContent::kDDS:
		bm = ResDdsReadImage(s, pMem);
		break;
	case eImageContent::kPNG:
	case eImageContent::kTIFF:
		bm = ResPngReadImage(s, pMem);
		break;
	case eImageContent::kPCX:
		bm = ResPcxReadImage(s, pMem);
		if (bm && bm->type == BMT_FLAT8)
		{
			s->SetPos(0);
			pal = ResPcxReadPalette(s, pMem);
		}
		break;
	case eImageContent::kGIF:
		bm = ResGifReadImage(s, pMem);
		if (bm && bm->type == BMT_FLAT8)
		{
			s->SetPos(0);
			pal = ResGifReadPalette(s, pMem);
		}
		break;
	default:
		if (ext && !stricmp(ext, ".tga"))
			bm = ResTgaReadImage(s, pMem);
		break;
	}
	s->Close();
	s->Release();
	if (!bm)
		return false;
	bool ok = bm->w > 0 && bm->h > 0;
	const uint8* rgba = ResTrueColorFind(bm);
	const ushort fmt = bm->flags & 0x7f00;
	if (!rgba && !(bm->type == BMT_FLAT16) && !(bm->type == BMT_FLAT8 && pal))
		ok = false;
	if (ok)
	{
		t.w = bm->w;
		t.h = bm->h;
		const int step = std::max(1, std::max(t.w, t.h) / 256);
		for (int y = 0; y < t.h; y += step)
			for (int x = 0; x < t.w; x += step)
			{
				float r, g, b, a = 1.0f;
				if (rgba)
				{
					const uint8* p = rgba + (size_t(y) * t.w + x) * 4;
					r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f; a = p[3] / 255.0f;
				}
				else if (bm->type == BMT_FLAT16)
				{
					const ushort p = reinterpret_cast<const ushort*>(bm->bits + size_t(y) * bm->row)[x];
					if (fmt == BMF_RGB_4444)
					{
						a = ((p >> 12) & 15) / 15.0f; r = ((p >> 8) & 15) / 15.0f;
						g = ((p >> 4) & 15) / 15.0f; b = (p & 15) / 15.0f;
					}
					else if (fmt == BMF_RGB_1555 || fmt == BMF_RGB_555)
					{
						a = (fmt == BMF_RGB_555 || (p & 0x8000)) ? 1.0f : 0.0f;
						r = ((p >> 10) & 31) / 31.0f; g = ((p >> 5) & 31) / 31.0f; b = (p & 31) / 31.0f;
					}
					else
					{
						r = ((p >> 11) & 31) / 31.0f; g = ((p >> 5) & 63) / 63.0f; b = (p & 31) / 31.0f;
					}
				}
				else
				{
					const uint8 i = bm->bits[size_t(y) * bm->row + x];
					r = pal[i * 3] / 255.0f; g = pal[i * 3 + 1] / 255.0f; b = pal[i * 3 + 2] / 255.0f;
					if (i == 0 && (bm->flags & BMF_TRANS))
						a = 0.0f;
				}
				const int cell = (y * 16 / t.h) * 16 + (x * 16 / t.w);
				t.c[cell * 3] += r * a;
				t.c[cell * 3 + 1] += g * a;
				t.c[cell * 3 + 2] += b * a;
				t.wt[cell] += a;
			}
		for (int i = 0; i < 256; ++i)
			if (t.wt[i] > 0)
				for (int k = 0; k < 3; ++k)
					t.c[i * 3 + k] /= t.wt[i];
	}
	ResTrueColorForget(bm);
	if (pal)
		pMem->ResFree(pal);
	pMem->ResFree(bm);
	return ok;
}

// how alike two pictures are: their shapes (the correlation of their
// brightness, cell by cell) and their colours on average
bool SamePicture(const sThumb& a, const sThumb& b, float* pLikeness)
{
	double la[256], lb[256], ma = 0, mb = 0, ca[3] = {}, cb[3] = {}, n = 0;
	for (int i = 0; i < 256; ++i)
	{
		la[i] = 0.2126 * a.c[i * 3] + 0.7152 * a.c[i * 3 + 1] + 0.0722 * a.c[i * 3 + 2];
		lb[i] = 0.2126 * b.c[i * 3] + 0.7152 * b.c[i * 3 + 1] + 0.0722 * b.c[i * 3 + 2];
		ma += la[i];
		mb += lb[i];
		for (int k = 0; k < 3; ++k)
		{
			ca[k] += a.c[i * 3 + k];
			cb[k] += b.c[i * 3 + k];
		}
		n += 1;
	}
	ma /= n;
	mb /= n;
	double sab = 0, saa = 0, sbb = 0;
	for (int i = 0; i < 256; ++i)
	{
		sab += (la[i] - ma) * (lb[i] - mb);
		saa += (la[i] - ma) * (la[i] - ma);
		sbb += (lb[i] - mb) * (lb[i] - mb);
	}
	// a flat picture (one colour): its colour is all there is to compare
	const double corr = (saa > 1e-4 && sbb > 1e-4) ? sab / std::sqrt(saa * sbb)
	                                                : (std::fabs(ma - mb) < 0.08 ? 1.0 : 0.0);
	double diff = 0;
	for (int k = 0; k < 3; ++k)
		diff += std::fabs(ca[k] - cb[k]) / n;
	diff /= 3.0;
	*pLikeness = float(corr);
	return corr >= 0.55 && diff <= 0.2;
}

std::string NormalPath(const char* p)
{
	std::string s(p ? p : "");
	for (char& ch : s)
		ch = ch == '\\' ? '/' : char(tolower((unsigned char)ch));
	return s;
}

bool UnderPath(IStore* pStore, const std::string& root)
{
	if (!pStore || root.empty())
		return false;
	const std::string s = NormalPath(pStore->GetFullPathName());
	return s.compare(0, root.size(), root) == 0;
}

std::mutex g_hdMutex;
std::unordered_map<std::string, int> g_hdChoices;   // key -> 1 the mod's, 0 the mission's
bool g_hdLoaded = false;
int g_hdLogged = 0;

std::string HdChoicesFile()
{
	const char* game = getenv("DARK_GAME_PATH");
	return std::string(game && game[0] ? game : ".") + "/hd_over_fm.txt";
}
} // namespace

namespace
{
struct sMissingResourceDiagnostic
{
	std::string name;
	std::string type;
	std::string path;
	unsigned count;
};

std::mutex g_missionDiagnosticsMutex;
std::vector<sMissingResourceDiagnostic> g_missingResources;
std::string g_diagnosticMission;
bool g_diagnosticMissionActive = false;
bool g_diagnosticPostLoadComplete = false;
unsigned g_lastSummarizedRequests = 0;

unsigned MissingResourceRequestCount()
{
	unsigned total = 0;
	for (size_t i = 0; i < g_missingResources.size(); ++i)
		total += g_missingResources[i].count;
	return total;
}

bool TraceResourceMisses()
{
	const char* trace = getenv("DARK_RESOURCE_MISS_TRACE");
	if (!trace || !trace[0] || trace[0] == '0')
		trace = getenv("DARK_RESOURCE_TRACE");
	return trace && trace[0] && trace[0] != '0';
}

void RecordMissingResource(const char* name, const char* type,
	const char* path)
{
	std::lock_guard<std::mutex> lock(g_missionDiagnosticsMutex);
	if (!g_diagnosticMissionActive)
		return;

	const char* safeName = name ? name : "";
	const char* safeType = type ? type : "unknown";
	const char* safePath = path ? path : "";
	for (size_t i = 0; i < g_missingResources.size(); ++i)
	{
		sMissingResourceDiagnostic& existing = g_missingResources[i];
		if (!stricmp(existing.name.c_str(), safeName) &&
			!stricmp(existing.type.c_str(), safeType) &&
			!stricmp(existing.path.c_str(), safePath))
		{
			++existing.count;
			return;
		}
	}

	sMissingResourceDiagnostic missing;
	missing.name = safeName;
	missing.type = safeType;
	missing.path = safePath;
	missing.count = 1;
	g_missingResources.push_back(missing);

	if (TraceResourceMisses())
	{
		char message[1400];
		snprintf(message, sizeof(message),
			"Unresolved resource probe '%s' (type=%s%s%s)%s",
			safeName, safeType, safePath[0] ? ", path=" : "",
			safePath, g_diagnosticPostLoadComplete ? " [after post-load]" : "");
		message[sizeof(message) - 1] = '\0';
		PlatformDiagnosticsLog("TRACE", "resource", message);
	}
}

void WriteMissionResourceSummary(const char* checkpoint)
{
	const unsigned requests = MissingResourceRequestCount();
	if (requests == g_lastSummarizedRequests && checkpoint &&
		!strcmp(checkpoint, "shutdown"))
		return;
	g_lastSummarizedRequests = requests;

	char message[1024];
	snprintf(message, sizeof(message),
		"Mission '%s' %s: %u unique unresolved resource probe%s, "
		"%u total (fallback probes are expected; see texture preflight for "
		"actionable missing art)",
		g_diagnosticMission.empty() ? "(unknown)" : g_diagnosticMission.c_str(),
		checkpoint ? checkpoint : "checkpoint",
		(unsigned)g_missingResources.size(),
		g_missingResources.size() == 1 ? "" : "s", requests);
	message[sizeof(message) - 1] = '\0';
	PlatformDiagnosticsLog("INFO", "resource", message);
	if (requests && TraceResourceMisses())
		mprintf("Resource trace: %u unresolved probe%s; see %s\n",
			(unsigned)g_missingResources.size(),
			g_missingResources.size() == 1 ? "" : "s",
			PlatformDiagnosticsPath());
}
}

void MissionDiagnosticsBegin(const char* mission)
{
	std::lock_guard<std::mutex> lock(g_missionDiagnosticsMutex);
	g_missingResources.clear();
	g_diagnosticMission = mission ? mission : "";
	g_diagnosticMissionActive = true;
	g_diagnosticPostLoadComplete = false;
	g_lastSummarizedRequests = 0;
	char message[1024];
	snprintf(message, sizeof(message), "Beginning mission load: %s",
		g_diagnosticMission.empty() ? "(unknown)" : g_diagnosticMission.c_str());
	message[sizeof(message) - 1] = '\0';
	PlatformDiagnosticsLog("INFO", "mission", message);
}

void MissionDiagnosticsFinish(BOOL loaded)
{
	std::lock_guard<std::mutex> lock(g_missionDiagnosticsMutex);
	if (!g_diagnosticMissionActive)
		return;
	g_diagnosticPostLoadComplete = true;
	WriteMissionResourceSummary(loaded ? "post-load" : "load failed");
}

void MissionDiagnosticsFinalSummary(void)
{
	std::lock_guard<std::mutex> lock(g_missionDiagnosticsMutex);
	if (g_diagnosticMissionActive)
		WriteMissionResourceSummary("shutdown");
}

void MissionDiagnosticsIssue(const char* severity, const char* subsystem,
	const char* message)
{
	PlatformDiagnosticsLog(severity, subsystem, message);
}

///////////////////////////////////////////////////////////////////////////////
//
// CLASS: cResMan
//

cResMan::cResMan(IUnknown* pOuter)
	:
	m_pStoreMan{ nullptr },
	m_pDefSearchPath{ nullptr },
	m_pGlobalContext{ nullptr },
	m_pDefVariants{ nullptr },
	m_ResTypeHash{},
	m_ResTypeByNameHash{},
	m_ResTable{},
	m_pSharedCache{ nullptr },
	m_pCache{ nullptr },
	m_bPagingEnabled{ 1 },
	m_ppResFactories{ nullptr },
	m_nResFactories{ 0 },
	m_FreshStamp{ 1 },
	m_pResStats{ nullptr },
	m_fDidInit{ false },
	m_DefResMem{}
{
	MI_INIT_AGGREGATION_4(pOuter, IResMan, IResStats, IResMem, IResManHelper, kPriorityLibrary, nullptr);
}

///////////////////////////////////////

cResMan::~cResMan()
{
}

///////////////////////////////////////

HRESULT LGAPI ResCacheCallback(const sCacheMsg* pMsg)
{
	if (pMsg->message < 0 || pMsg->message > 1)
		return S_FALSE;

	reinterpret_cast<cResMan*>(pMsg->pClientContext)->FreeData(reinterpret_cast<cResourceTypeData*>(pMsg->itemId), TRUE);
	
	return S_OK;
}

///////////////////////////////////////

HRESULT cResMan::Init()
{
	if (m_fDidInit)
		return S_FALSE;

	m_pSharedCache = AppGetObj(ISharedCache);
	if (!m_pSharedCache)
		CriticalMsg("cResMan: no shared cache is available!");

	sCacheClientDesc cacheDesc{};
	cacheDesc.pID = &IID_IResMan;
	cacheDesc.pContext = this;
	cacheDesc.pfnCallback = ResCacheCallback;
	cacheDesc.nMaxBytes = -1;
	cacheDesc.nMaxItems = -1;
	cacheDesc.flags = 0;

	m_pSharedCache->AddClient(&cacheDesc, &m_pCache);
	if (!m_pCache)
		CriticalMsg("cResMan: couldn't get a resource cache!");

	auto* pDefResType = MakeBinaryResourceType();
	if (!RegisterResType(pDefResType))
		CriticalMsg("cResMan: unable to register binary resource type");

	pDefResType->Release();

	m_pStoreMan = new cStorageManager{};

	auto* pZipStore = MakeZipStorageFactory();
	if (!RegisterStoreFactory(pZipStore))
		CriticalMsg("cResMan: unable to register ZIP storage factory");

	pZipStore->Release();

	m_pResStats = new cResStats{};
	m_fDidInit = true;
	m_ResManARQ.Init(this);

	return S_OK;
}

///////////////////////////////////////

HRESULT cResMan::End()
{
	if (!m_fDidInit)
		return E_FAIL;

	MissionDiagnosticsFinalSummary();
	m_pSharedCache->FlushAll();

	if (m_pCache)
	{
		m_pCache->Release();
		m_pCache = nullptr;
	}

	if (m_pSharedCache)
	{
		m_pSharedCache->Release();
		m_pSharedCache = nullptr;
	}

	m_ResManARQ.Term();
	CleanResources();

	if (m_pResStats)
	{
		delete m_pResStats;
		m_pResStats = nullptr;
	}

	m_ResTable.DestroyAll();
	m_ResTypeHash.DestroyAll();
	m_ResTypeByNameHash.DestroyAll();

	if (m_ppResFactories)
	{
		for (int i = 0; i < m_nResFactories; ++i)
			m_ppResFactories[i]->Release();
		Free(m_ppResFactories);

		m_ppResFactories = nullptr;
	}

	m_nResFactories = 0;

	if (m_pDefSearchPath)
	{
		m_pDefSearchPath->Release();
		m_pDefSearchPath = nullptr;
	}

	if (m_pStoreMan)
	{
		m_pStoreMan->Close();
		m_pStoreMan->Release();
		m_pStoreMan = nullptr;
	}

	m_fDidInit = false;

	return S_OK;
}

///////////////////////////////////////

ISearchPath* cResMan::NewSearchPath(const char* pNewPath)
{
	return m_pStoreMan->NewSearchPath(pNewPath);
}

///////////////////////////////////////

void cResMan::SetDefaultPath(ISearchPath* pPath)
{
	cAutoResThreadLock lock;

	if (m_pDefSearchPath)
		m_pDefSearchPath->Release();

	m_pDefSearchPath = pPath;
	if (pPath)
		pPath->AddRef();
}

///////////////////////////////////////

void cResMan::SetGlobalContext(ISearchPath* pPath)
{
	m_pStoreMan->SetGlobalContext(pPath);
}

///////////////////////////////////////

void cResMan::SetDefaultVariants(ISearchPath* pPath)
{
	m_pStoreMan->SetDefaultVariants(pPath);
}

///////////////////////////////////////

IRes* cResMan::Bind(const char* pRelativePathname, const char* pTypeName, ISearchPath* pPath, const char* pExpRelPath, uint fBindFlags)
{
	char temp[1024];
	strncpy(temp, pRelativePathname, 1024);
	temp[1023] = '\0';

	const char* pRelPath = nullptr;
	char OldSlash[4]{};
	BOOL bCombined;
	const char* pName = nullptr;

	MungePaths(temp, pExpRelPath, const_cast<char**>(&pRelPath), const_cast<char**>(&pName), OldSlash, &bCombined);
	auto* pRes = DoBind(pName, pTypeName, pPath, pRelPath, fBindFlags);
	RestorePath(const_cast<char*>(pRelPath), const_cast<char*>(pName), OldSlash[0], bCombined);

	return pRes;
}

///////////////////////////////////////

void cResMan::BindAll(const char* pPattern, const char* pTypeName, ISearchPath* pPath, void(*callback)(IRes*, IStore*, void*), void* pClientData, const char* pRelPath, uint fBindFlags)
{
	if (!callback)
	{
		Warning(("BindAll called without a callback!\n"));
		return;
	}

	if (!pPath)
	{
		pPath = m_pDefSearchPath;
		if (!pPath)
		{
			Warning(("BindAll called without a search path!\n"));
			return;
		}
	}

	if (!pTypeName || pTypeName[0] == '\0')
	{
		Warning(("Can't Bind Resource without a type!\n"));
		return;
	}

	auto* pType = GetResType(pTypeName);
	if (pType == nullptr)
		return;

	auto* pCookie = pPath->BeginContents(pPattern, 0, pRelPath);
	if (!pCookie)
		return;

	char pResName[MAX_STORENAME];
	cStr resExt{};
	IStore* pStore, * pCanonStore;

	while (pPath->Next(pCookie, &pStore, pResName, &pCanonStore))
	{
		auto fileSpec = cFileSpec{ pResName };
		fileSpec.GetFileExtension(resExt);

		if (pType->IsLegalExt(resExt))
		{
			auto* pRes = GetResource(pResName, pTypeName, pStore);
			if (!pRes)
				pRes = BindSpecific(pResName, pTypeName, pStore, pCanonStore, fBindFlags);

			if (pRes)
			{
				callback(pRes, pStore, pClientData);
				pRes->Release();
			}
		}

		pCanonStore->Release();
		pStore->Release();
	}

	pPath->EndContents(pCookie);
}

///////////////////////////////////////

IRes* cResMan::BindSpecific(const char* pName, const char* pTypeName, IStore* pStore, IStore* pCanonStore, uint fBindFlags)
{
	cAutoResThreadLock lock;

	if (!pTypeName || !*pTypeName || !pName || !*pName || !pStore)
		CriticalMsg("Missing arguments for BindSpecific!");

	if (!VerifyStorage(pStore))
		return nullptr;

	auto* pResource = GetResource(pName, pTypeName, pStore);
	if (pResource)
		return pResource;

	if (!pStore->StreamExists(pName))
		return nullptr;

	cAnsiStr resExt{};
	cFileSpec fileSpec{ pName };
	fileSpec.GetFileExtension(resExt);

	pResource = CreateResource(pStore, pName, resExt, pTypeName, fBindFlags);
	if (pResource)
	{
		IResControl* pResControl = nullptr;
		if (FAILED(pResource->QueryInterface(IID_IResControl, reinterpret_cast<void**>(&pResControl))))
		{
			CriticalMsg("BindSpecific: resource lacks IResControl!");
			pResource->Release();
			return nullptr;
		}

		pResControl->AllowStorageReset(!(fBindFlags & 1));
		pResControl->SetCanonStore(pCanonStore);
		pResControl->Release();
	}

	m_pResStats->LogStatRes(pResource, eResourceStats::ResourceBindings);

	return pResource;
}

///////////////////////////////////////

IRes* cResMan::Retype(IRes* pOldRes, const char* pTypeName, uint fBindFlags)
{
	if (!pOldRes || !pTypeName)
		return nullptr;

	auto* pStore = pOldRes->GetStore();
	if (!pStore)
		CriticalMsg("Resource without a storage!");

	char* pStreamName;
	pOldRes->GetStreamName(0, &pStreamName);
	if (!pStreamName)
		return nullptr;

	auto* pCanonStore = pOldRes->GetCanonStore();
	auto* pNewRes = BindSpecific(pStreamName, pTypeName, pStore, pCanonStore, fBindFlags);

	Free(pStreamName);
	pCanonStore->Release();
	pStore->Release();

	return pNewRes;
}

///////////////////////////////////////

IRes* cResMan::Lookup(const char* pRelativePathname, const char* pTypeName, const char* pCanonPath)
{
	char temp[1024];
	strncpy(temp, pRelativePathname, 1023);
	temp[1023] = '\0';

	char* pRelPath, * pName, OldSlash[4];
	BOOL bComb;

	MungePaths(temp, pCanonPath, &pRelPath, &pName, OldSlash, &bComb);
	auto* pRes = DoLookup(pName, pTypeName, pRelPath);
	RestorePath(pRelPath, pName, OldSlash[0], bComb);

	return pRes;
}

///////////////////////////////////////

IStore* cResMan::GetStore(const char* pPathName)
{
	cAutoResThreadLock lock{};

	if (pPathName)
		return m_pStoreMan->GetStore(pPathName, 0);

	return nullptr;
}

///////////////////////////////////////

void ResTypeEnumerator(const char* pExt, IResType* pType, void* pClientData)
{
	reinterpret_cast<cResMan*>(pClientData)->InstallResourceType(pExt, pType);
}

///////////////////////////////////////

BOOL cResMan::RegisterResType(IResType* pType)
{
	cAutoResThreadLock lock{};

	if (!pType)
		return FALSE;

	pType->EnumerateExts(ResTypeEnumerator, this);
	if (m_ppResFactories)
	{
		++m_nResFactories;
		m_ppResFactories = reinterpret_cast<IResType**>(Realloc(m_ppResFactories, sizeof(IResType*) * m_nResFactories));
		m_ppResFactories[m_nResFactories - 1] = pType;
	}
	else
	{
		m_ppResFactories = reinterpret_cast<IResType**>(Malloc(sizeof(IResType*)));
		*m_ppResFactories = pType;
		m_nResFactories = 1;
	}

	pType->AddRef();

	auto* pTypeEntry = m_ResTypeByNameHash.Search(pType->GetName());
	if (pTypeEntry)
	{
		m_ResTypeByNameHash.Remove(pTypeEntry);
		delete pTypeEntry;
	}

	m_ResTypeByNameHash.Insert(new cNamedResType{ pType });

	return TRUE;
}

///////////////////////////////////////

BOOL cResMan::RegisterStoreFactory(IStoreFactory* pStoreFactory)
{
	cAutoResThreadLock lock{};

	if (pStoreFactory && m_pStoreMan)
	{
		m_pStoreMan->RegisterFactory(pStoreFactory);

		return TRUE;
	}

	return FALSE;
}

///////////////////////////////////////

void ResTypeRemover(const char* pExt, IResType* pType, void* pClientData)
{
	reinterpret_cast<cResMan*>(pClientData)->RemoveResourceType(pExt, pType);
}

///////////////////////////////////////

void cResMan::UnregisterResType(IResType* pType)
{
	cAutoResThreadLock lock{};

	if (!pType)
		return;

	pType->EnumerateExts(ResTypeRemover, this);

	if (m_ppResFactories)
	{
		int i;
		for (i = 0; i < m_nResFactories && m_ppResFactories[i] != pType; ++i)
			;

		if (i < m_nResFactories)
		{
			m_ppResFactories[i]->Release();
			if (i < m_nResFactories - 1)
				memmove(&m_ppResFactories[i], &m_ppResFactories[i + 1], sizeof(IResType*) * (m_nResFactories - (i + 1)));

			if (!--m_nResFactories)
			{
				Free(m_ppResFactories);
				m_ppResFactories = nullptr;
			}
		}
	}

	auto* pTypeEntry = m_ResTypeByNameHash.Search(pType->GetName());
	if (pTypeEntry && pTypeEntry->m_pType == pType)
	{
		m_ResTypeByNameHash.Remove(pTypeEntry);
		delete pTypeEntry;
	}
}

///////////////////////////////////////

void cResMan::MarkForRefresh(IRes* pRes)
{
	cAutoResThreadLock lock{};

	if (pRes)
	{
		auto* pData = GetResourceTypeData(pRes);
		if (pData)
			pData->m_Freshed = FALSE;
	}
}

///////////////////////////////////////

void cResMan::GlobalRefresh()
{
	++m_FreshStamp;
}

///////////////////////////////////////

void cResMan::SetMode(eResStatMode mode, BOOL bTurnOn)
{
	m_pResStats->SetMode(mode, bTurnOn);
}

///////////////////////////////////////

void cResMan::Dump(const char* pFile)
{
	m_pResStats->Dump(pFile);
}

///////////////////////////////////////

void cResMan::DumpSnapshot(const char* pFile)
{
	FILE* fp = nullptr;

	if (pFile)
		fp = fopen(pFile, "a+");

	DoDumpSnapshot(fp);

	if (fp)
		fclose(fp);
}

///////////////////////////////////////

void cResMan::EnablePaging(BOOL bEnable)
{
	m_bPagingEnabled = bEnable;
}

///////////////////////////////////////

void cResMan::Compact()
{
	m_pSharedCache->Flush(&IID_IResMan);
}

///////////////////////////////////////

void* cResMan::LockResource(IRes* pResource)
{
	cAutoResThreadLock lock{};

	m_pResStats->LogStatRes(pResource, eResourceStats::LockRequests);
	if (!pResource)
		return nullptr;

	auto* pData = GetResourceTypeData(pResource);
	if (!pData)
	{
		CriticalMsg("Resource Lock failed. No private data");
		return nullptr;
	}

	if (!DoLockResource(pResource, pData))
		CriticalMsg("Resource Lock failed -- unable to lock data.");

	return pData->m_pData;
}

///////////////////////////////////////

void* cResMan::ExtractResource(IRes* pRes, void* pBuf)
{
	cAutoResThreadLock lock{};

	if (!pRes || !pBuf)
		return nullptr;

	IResControl* pResControl = nullptr;
	if (FAILED(pRes->QueryInterface(IID_IResControl, reinterpret_cast<void**>(&pResControl))))
	{
		CriticalMsg("Resource without an IResControl!");
		return nullptr;
	}

	auto* pStream = pResControl->OpenStream();
	if (!pStream)
		CriticalMsg1("Unable to open stream: %s", pRes->GetName());

	pResControl->Release();

	if (!pStream)
		return nullptr;

	pStream->Read(pStream->GetSize(), static_cast<char*>(pBuf));
	pStream->Close();
	pStream->Release();

	return pBuf;
}

///////////////////////////////////////

void* cResMan::FindResource(IRes* pResource, long* pSize)
{
	cAutoResThreadLock lock{};

	if (pSize)
		*pSize = 0;

	auto* pData = GetResourceTypeData(pResource);
	if (!pData)
		CriticalMsg("FindResource -- no private resource data!");

	if ((pData->m_nUserLockCount || pData->m_nInternalLockCount)
		&& pData->m_Freshed >= m_FreshStamp)
	{
		if (pSize)
			*pSize = pData->m_nSize;

		if (!pData->m_nUserLockCount && pData->m_nInternalLockCount)
		{
			if (pData->m_pResMem)
				pData->m_pResMem->GetSize(pData->m_pData);
			else
				MSize(pData->m_pData);

			//m_ResTable->???(pData); TODO
		}

		return pData->m_pData;
	}

	return nullptr;
}

///////////////////////////////////////

void cResMan::UnlockResource(IRes* pResource)
{
	cAutoResThreadLock lock{};

	auto* pData = GetResourceTypeData(pResource);
	if (pData)
		DoUnlockResource(pData);
	else
		CriticalMsg("Unable to unlock resource");
}

///////////////////////////////////////

uint cResMan::GetResourceLockCount(IRes* pResource)
{
	cAutoResThreadLock lock{};

	auto* pData = GetResourceTypeData(pResource);
	if (pData)
		return pData->m_nUserLockCount;

	CriticalMsg("Unable to get lock count");
	return 0;
}

///////////////////////////////////////

int cResMan::DropResource(IRes* pResource)
{
	cAutoResThreadLock lock{};

	auto* pData = GetResourceTypeData(pResource);
	return DropResourceData(pData);
}

///////////////////////////////////////

long cResMan::GetResourceSize(IRes* pRes)
{
	cAutoResThreadLock lock{};

	if (!pRes)
		return 0;

	auto* pData = GetResourceTypeData(pRes);
	if (pData->m_nSize)
		return pData->m_nSize;

	if (pData->m_nUserLockCount || pData->m_nInternalLockCount)
		return 0;

	IResControl* pResControl = nullptr;
	if (FAILED(pRes->QueryInterface(IID_IResControl, reinterpret_cast<void**>(&pResControl))))
	{
		CriticalMsg("Resource without an IResControl!");
		return 0;
	}

	auto* pStream = pResControl->OpenStream();
	if (!pStream)
		CriticalMsg1("Unable to open stream: %s", pRes->GetName());

	pResControl->Release();

	if (!pStream)
		return 0;

	auto nSize = pStream->GetSize();
	pStream->Close();
	pStream->Release();

	return nSize;
}

///////////////////////////////////////

void cResMan::UnregisterResource(IRes* pResource, DWORD_PTR ManData)
{
	cAutoResThreadLock lock{};

	if (!pResource || !ManData || ManData == static_cast<DWORD_PTR>(-1))
	{
		Warning(("UnregisterResource called for a bogus resource!\n"));
		return;
	}

	auto* pTypeData = reinterpret_cast<cResourceTypeData*>(ManData);
	pTypeData->m_pResourceData->m_ResourceTypeHash.Remove(pTypeData);
	delete pTypeData;
}

///////////////////////////////////////

int cResMan::AsyncLock(IRes* pResource, int nPriority)
{
	return m_ResManARQ.Lock(pResource, nPriority);
}

///////////////////////////////////////

int cResMan::AsyncExtract(IRes* pResource, int nPriority, void* pBuf, long bufSize)
{
	return m_ResManARQ.Extract(pResource, nPriority, pBuf, bufSize);
}

///////////////////////////////////////

int cResMan::AsyncPreload(IRes* pResource)
{
	return m_ResManARQ.Preload(pResource);
}

///////////////////////////////////////

int cResMan::IsAsyncFulfilled(IRes* pResource)
{
	return m_ResManARQ.IsFulfilled(pResource);
}

///////////////////////////////////////

HRESULT cResMan::AsyncKill(IRes* pResource)
{
	return m_ResManARQ.Kill(pResource);
}

///////////////////////////////////////

HRESULT cResMan::GetAsyncResult(IRes* pResource, void** ppResult)
{
	return m_ResManARQ.GetResult(pResource, ppResult);
}

///////////////////////////////////////

void cResMan::CleanResources()
{
	tHashSetHandle NameHandle, TypeHandle;

	for (auto* pNameData = m_ResTable.GetFirst(NameHandle); pNameData != nullptr; pNameData = m_ResTable.GetNext(NameHandle))
	{
		if (pNameData->m_fFlags & 1)
			continue;

		for (auto* pData = pNameData->m_pFirstStream; pData != nullptr; pData = pData->m_pNext)
		{
			for (auto* pTypeData = pData->m_ResourceTypeHash.GetFirst(TypeHandle);
				pTypeData != nullptr; pTypeData = pData->m_ResourceTypeHash.GetNext(TypeHandle))
			{
				auto refs = -1;
				if (pTypeData->m_pRes)
				{
					refs = pTypeData->m_pRes->AddRef() - 1;
					pTypeData->m_pRes->Release();
				}

				Warning(("Leftover resource %s found at cleanup with lock count of %d, %d references.\n", pTypeData->GetName(), pTypeData->m_nUserLockCount, refs));
			}
		}
	}
}

///////////////////////////////////////

struct sFoundTypedStream
{
	enum { kMaxCandidates = 16 };

	const char* pName;
	const char* pRelPath;
	ISearchPath* pPath;
	char pFoundName[MAX_STORENAME];
	char candidateNames[kMaxCandidates][MAX_STORENAME];
	const char* candidatePtrs[kMaxCandidates];
	int candidateCount;
	IStore* pFoundCanonStore;
	IStore* pStore;
};

///////////////////////////////////////

void TryNameWithExt(const char* pExt, IResType*, void* pClientData)
{
	auto* foundTyped = reinterpret_cast<sFoundTypedStream*>(pClientData);

	if (foundTyped->candidateCount >= sFoundTypedStream::kMaxCandidates)
		return;

	auto* pCandidate = foundTyped->candidateNames[foundTyped->candidateCount];
	const int candidateLength = snprintf(pCandidate,
		sizeof(foundTyped->candidateNames[0]), "%s%s", foundTyped->pName, pExt);
	if (candidateLength >= 0 &&
		candidateLength < static_cast<int>(sizeof(foundTyped->candidateNames[0])))
	{
		foundTyped->candidatePtrs[foundTyped->candidateCount] = pCandidate;
		++foundTyped->candidateCount;
	}
}

///////////////////////////////////////

void cResMan::PreferHdTexture(cSearchPath* pPath, const char* const* ppNames, int nNames,
	const char* pRelPath, const char* pName, IStore** ppStore, IStore** ppCanon, int* pIndex)
{
	const char* fm = getenv("DARK_FM_PATH");
	if (!fm || !fm[0] || getenv("DARK_HD_OVER_FM_OFF"))
		return;
	std::string fmRoot = NormalPath(fm);
	while (!fmRoot.empty() && fmRoot.back() == '/')
		fmRoot.pop_back();
	const std::string full = NormalPath(pRelPath ? (std::string(pRelPath) + "/" + pName).c_str() : pName);
	if (full.find("fam/") == std::string::npos && full.find("obj/") == std::string::npos &&
		full.find("mesh/") == std::string::npos)
		return;   // textures only (not the interface, books, ...)
	{
		static bool s_logged = false;
		if (!s_logged)
		{
			s_logged = true;
			fprintf(stderr, "Textures: fan mission at %s; the first texture found in %s\n", fmRoot.c_str(),
				NormalPath((*ppCanon ? *ppCanon : *ppStore)->GetFullPathName()).c_str());
		}
	}
	if (!UnderPath(*ppCanon, fmRoot) && !UnderPath(*ppStore, fmRoot))
		return;   // not the mission's own

	IStore* stores[8] = {};
	IStore* canons[8] = {};
	int idx[8] = {};
	const int n = pPath->FindEach(ppNames, nNames, pRelPath, stores, canons, idx, 8);
	int mine = -1, alt = -1;
	for (int k = 0; k < n && mine < 0; ++k)
		if (UnderPath(canons[k], fmRoot) || UnderPath(stores[k], fmRoot))
			mine = k;
	for (int k = mine + 1; mine >= 0 && k < n && alt < 0; ++k)
		if (!UnderPath(canons[k], fmRoot) && !UnderPath(stores[k], fmRoot))
			alt = k;
	// the original game's own (the last root): the mission's copy is compared
	// with it -- the HD pack is a remake of the original, not of the copy
	const int base = n - 1;
	if (alt >= 0 && mine >= 0 && base > alt)
	{
		const char* fmName = getenv("DARK_FM_NAME");
		const std::string key = std::string(fmName ? fmName : "") + "|" + full;
		int choice = -1;
		{
			std::lock_guard<std::mutex> lock(g_hdMutex);
			if (!g_hdLoaded)
			{
				g_hdLoaded = true;
				if (FILE* f = fopen(HdChoicesFile().c_str(), "r"))
				{
					char line[1024];
					while (fgets(line, sizeof(line), f))
					{
						char* tab = strrchr(line, '\t');
						if (!tab)
							continue;
						*tab = 0;
						g_hdChoices[line] = atoi(tab + 1);
					}
					fclose(f);
				}
			}
			auto it = g_hdChoices.find(key);
			if (it != g_hdChoices.end())
				choice = it->second;
		}
		if (choice < 0)
		{
			sThumb ours, theirs;
			float likeness = 0;
			choice = 0;
			if (ThumbOf(stores[mine], ppNames[idx[mine]], &m_DefResMem, ours) &&
				ThumbOf(stores[base], ppNames[idx[base]], &m_DefResMem, theirs) &&
				std::fabs(float(theirs.w) / theirs.h - float(ours.w) / ours.h) < 0.01f &&
				SamePicture(ours, theirs, &likeness))
				choice = 1;
			{
				std::lock_guard<std::mutex> lock(g_hdMutex);
				g_hdChoices[key] = choice;
				if (FILE* f = fopen(HdChoicesFile().c_str(), "a"))
				{
					fprintf(f, "%s\t%d\n", key.c_str(), choice);
					fclose(f);
				}
				if (g_hdLogged < 60)
				{
					++g_hdLogged;
					fprintf(stderr, "Textures: %s: %s (%dx%d, the original %dx%d, likeness %.2f)\n", full.c_str(),
						choice ? "the HD pack's (the mission's is the original)" : "the mission's own (its own picture)",
						ours.w, ours.h, theirs.w, theirs.h, likeness);
				}
			}
		}
		if (choice == 1)
		{
			(*ppStore)->Release();
			if (*ppCanon)
				(*ppCanon)->Release();
			*ppStore = stores[alt];
			(*ppStore)->AddRef();
			*ppCanon = canons[alt];
			(*ppCanon)->AddRef();
			*pIndex = idx[alt];
		}
	}
	for (int k = 0; k < n; ++k)
	{
		stores[k]->Release();
		canons[k]->Release();
	}
}

IRes* cResMan::DoBind(const char* pName, const char* pTypeName, ISearchPath* pPath, const char* pRelPath, uint fBindFlags)
{
	if (!pName || pName[0] == '\0')
	{
		Warning(("Can't Bind Resource with empty name!\n"));
		return nullptr;
	}

	if (!pTypeName || pTypeName[0] == '\0')
	{
		Warning(("Can't Bind Resource without a type!\n"));
		return nullptr;
	}

	auto* pType = GetResType(pTypeName);
	if (!pType)
	{
		Warning(("Bind: unknown type specified!\n"));
		return nullptr;
	}

	if (!pPath)
	{
		pPath = m_pDefSearchPath;
		if (!pPath)
		{
			Warning(("Bind: no path specified.\n"));
			return nullptr;
		}
	}

	char pFullname[MAX_STORENAME];
	IStore* pStore, * pCanonStore;

	cStr resExt{};
	cFileSpec fileSpec{ pName };
	fileSpec.GetFileExtension(resExt);

	if (resExt.IsEmpty())
	{
		sFoundTypedStream results{};
		results.pName = pName;
		results.pPath = pPath;
		results.pRelPath = pRelPath;
		results.pFoundCanonStore = nullptr;
		results.pStore = nullptr;

		pType->EnumerateExts(TryNameWithExt, &results);

		// Resource roots are ordered by content priority.  Search all legal
		// extensions inside one root before moving to the next root, so a
		// lower-priority PNG/TGA cannot override an FM or mod PCX merely due
		// to the global extension preference.
		int foundIndex = -1;
		if (auto* pConcretePath = dynamic_cast<cSearchPath*>(pPath))
		{
			results.pStore = pConcretePath->FindAny(results.candidatePtrs,
				results.candidateCount, 0, &results.pFoundCanonStore,
				results.pRelPath, &foundIndex);
		}
		else
		{
			for (int i = 0; !results.pStore && i < results.candidateCount; ++i)
			{
				results.pStore = pPath->Find(results.candidatePtrs[i], 0,
					&results.pFoundCanonStore, results.pRelPath);
				if (results.pStore)
					foundIndex = i;
			}
		}

		// a fan mission's low-resolution copy of a texture an HD pack has
		if (results.pStore && foundIndex >= 0 && !stricmp(pTypeName, RESTYPE_IMAGE))
			if (auto* pConcretePath = dynamic_cast<cSearchPath*>(pPath))
				PreferHdTexture(pConcretePath, results.candidatePtrs, results.candidateCount,
					results.pRelPath, pName, &results.pStore, &results.pFoundCanonStore, &foundIndex);

		if (foundIndex >= 0)
			strcpy(results.pFoundName, results.candidatePtrs[foundIndex]);

		pStore = results.pStore;
		if (results.pStore)
		{
			strcpy(pFullname, results.pFoundName);
			pCanonStore = results.pFoundCanonStore;
		}
	}
	else
	{
		if (!pType->IsLegalExt(resExt))
		{
			Warning(("Illegal extension %s given to bind type %s.\n", resExt));
			return nullptr;
		}

		pStore = pPath->Find(pName, 0, &pCanonStore, pRelPath);
		strcpy(pFullname, pName);
	}

	if (!pStore)
	{
		RecordMissingResource(pName, pTypeName, pRelPath);
		return nullptr;
	}

	auto* pRes = BindSpecific(pFullname, pTypeName, pStore, pCanonStore, fBindFlags);
	const char* pStorePath = pStore->GetFullPathName();
	if (!pStorePath)
		pStorePath = "";
	const char* pTrace = std::getenv("DARK_RESOURCE_TRACE");
	if (pTrace && pTrace[0] && pTrace[0] != '0')
		mprintf("resource bind: %s [%s] -> %s%s%s\n", pName, pTypeName,
			pStorePath, pStorePath[0] ? "\\" : "", pFullname);

	if (pCanonStore)
		pCanonStore->Release();

	pStore->Release();

	return pRes;
}

///////////////////////////////////////

IRes* cResMan::DoLookup(const char* pName, const char* pTypeName, const char* pRawCanonPath)
{
	if (!pTypeName || pTypeName[0] == '\0')
	{
		Warning(("Can't Lookup Resource without a type!\n"));
		return nullptr;
	}

	if (!pName || pName[0] == '\0')
	{
		Warning(("Lookup called with empty name.\n"));
		return nullptr;
	}

	auto* pNameData = m_ResTable.Search(pName);
	if (!pNameData)
		return nullptr;

	char* pCanonPath = nullptr;
	if (pRawCanonPath)
		GetNormalizedPath(pRawCanonPath, &pCanonPath);

	IRes* pRes = nullptr;
	if (pNameData->m_fFlags & 1)
		for (int i = 0; i < pNameData->m_ppFullNames->Size(); ++i)
		{
			auto* pFullName = (*pNameData->m_ppFullNames)[i];

			auto* pData = m_ResTable.FindResData(pFullName, nullptr, 0);
			if (pData)
			{
				pRes = WalkNameChain(pData, pName, pTypeName, pCanonPath);
				if (pRes != nullptr)
					break;
			}
			else
				Warning(("Lookup: odd -- no entry found for %s.\n", pFullName));
		}
	else
		pRes = WalkNameChain(pNameData->m_pFirstStream, pName, pTypeName, pCanonPath);

	if (pCanonPath)
		Free(pCanonPath);

	return pRes;
}

///////////////////////////////////////

BOOL cResMan::DropResourceData(cResourceTypeData* pData)
{
	if (pData->m_nUserLockCount)
		return FALSE;

	if (pData->m_nInternalLockCount)
	{
		CacheRemove(pData);
		FreeData(pData, FALSE);
	}

	return TRUE;
}

///////////////////////////////////////

void cResMan::CacheAdd(cResourceTypeData* pData)
{
	if (m_bPagingEnabled)
		// The cache callback receives itemId and passes it to FreeData as the
		// resource metadata object.  Keying this by m_pData instead makes the
		// callback interpret the resource bytes as cResourceTypeData.
		m_pCache->Add(reinterpret_cast<tCacheItemID>(pData), pData->m_pRes, pData->m_nSize);
}

///////////////////////////////////////

BOOL cResMan::CacheRemove(cResourceTypeData* pData)
{
	if (m_bPagingEnabled)
	{
		void* pDummy;
		if (m_pCache->Remove(reinterpret_cast<tCacheItemID>(pData), &pDummy) == S_OK)
			return TRUE;

		Warning(("Tried to remove an item expected but not in the cache (%s)\n", pData->GetName()));
	}

	return FALSE;
}

///////////////////////////////////////

void* cResMan::DoLockResource(IRes* pResource, cResourceTypeData* pData)
{
	m_ResManARQ.ClearPreload(pData);
	++pData->m_nUserLockCount;
	if (pData->m_nUserLockCount == 1)
	{
		if (pData->m_Freshed < m_FreshStamp)
		{
			Warning(("Refresh requested for %s; NYI...\n", pData->GetName()));
			pData->m_Freshed = m_FreshStamp;
		}

		if (pData->m_nInternalLockCount)
		{
			auto bSuccess = CacheRemove(pData);
			if (!bSuccess)
				CriticalMsg1("Res tried to take %s back from cache, it wasnt there?", pData->GetName());

			pData->m_nInternalLockCount = 0;
			if (bSuccess)
			{
				m_pResStats->LogStatRes(pResource, eResourceStats::NewLoadLRU);
			}
			else
			{
				m_pResStats->LogStatRes(pResource, eResourceStats::LoadFailed);
				m_pResStats->LogStatRes(pResource, eResourceStats::LockFailed);
			}
		}
		else
		{
			IResControl* pResControl;
			if (FAILED(pResource->QueryInterface(IID_IResControl, reinterpret_cast<void**>(&pResControl))))
			{
				CriticalMsg1("No IResControl for resource %s!", pResource->GetName());
				return nullptr;
			}

			if (pData->m_pData)
				CriticalMsg1("Unexpectedly overwriting data for %s", pResource->GetName());

			auto nNumTypes = 0;
			auto bProxied = false;
			auto ppProxiedTypes = pResControl->GetTranslatableTypes(&nNumTypes);
			if (nNumTypes)
			{
				int i = 0;

				cResourceTypeData* pFoundData;
				while (!bProxied && i < nNumTypes)
				{
					pFoundData = pData->m_pResourceData->m_ResourceTypeHash.Search(ppProxiedTypes[i]);

					if (pFoundData && pFoundData->m_pData)
						bProxied = true;
					else
						++i;
				}

				if (bProxied)
				{

					IResMemOverride* pResMem;
					if (pData->m_pResMem)
						pResMem = pData->m_pResMem;
					else
						pResMem = &m_DefResMem;

					if (g_fResPrintAccesses)
						mprintf("cResMan::DoLockResource(): Loading resource %s (proxied)\n", pData->GetName());

					ulong nNewSize = 0;
					BOOL bDidAllocate = FALSE;
					auto pNewData = pResControl->LoadTranslation(pFoundData->m_pData, pFoundData->m_nSize, ppProxiedTypes[i], &bDidAllocate, &nNewSize, pResMem);

					if (pNewData)
					{
						pData->m_pData = pNewData;
						pData->m_nSize = nNewSize;
						if (!bDidAllocate)
						{
							pData->m_pProxiedRes = pFoundData;
							DoLockResource(pResource, pFoundData);
						}

						m_pResStats->LogStatRes(pResource, eResourceStats::ProxiedLoad);
					}
					else
					{
						Warning(("Proxy load failed. Attempting fresh load.\n"));
						bProxied = false;
					}
				}
			}

			if (!bProxied)
			{
				if (g_fResPrintAccesses)
					mprintf("cResMan::DoLockResource(): Loading resource %s\n", pData->GetName());

				if (pData->m_pResMem)
					pData->m_pData = pResControl->LoadData(&pData->m_nSize, 0, pData->m_pResMem);
				else
					pData->m_pData = pResControl->LoadData(&pData->m_nSize, 0, &m_DefResMem);

				if (!pData->m_pData)
					CriticalMsg1("Failed to load resource data %s", pData->GetName());

				m_pResStats->LogStatRes(pResource, eResourceStats::NewLoad);
				m_pResStats->LogStatRes(pResource, eResourceStats::MemAlloced);
				pResource->AddRef();
			}
			pResControl->Release();
		}
	}
	else
	{
		m_pResStats->LogStatRes(pResource, eResourceStats::AlreadyLoaded);
	}

	return pData->m_pData;
}

///////////////////////////////////////

void cResMan::DoUnlockResource(cResourceTypeData* pData)
{
	if (!pData->m_nUserLockCount)
		CriticalMsg1("Lock count 0 during Unlock of %s", pData->GetName());

	if (pData->m_nUserLockCount)
	{
		--pData->m_nUserLockCount;
		if (!pData->m_nUserLockCount)
		{
			if (pData->m_pProxiedRes)
			{
				DoUnlockResource(pData->m_pProxiedRes);
				pData->m_pData = nullptr;
				pData->m_nSize = 0;
				pData->m_pProxiedRes = nullptr;
			}
			else
			{
				if (pData->m_nInternalLockCount)
					CriticalMsg1("Internal Lock Count non-zero during Unlock of %s", pData->GetName());

				if (!pData->m_nInternalLockCount)
				{
					pData->m_nInternalLockCount = 1;
					CacheAdd(pData);
				}
			}
		}
	}
}

///////////////////////////////////////

IRes* cResMan::WalkNameChain(cResourceData* pData, const char*, const char* pTypeName, const char* pCanonPath)
{
	if (!pTypeName)
		CriticalMsg("No type given to WalkNameChain!");

	for (; pData != nullptr; pData = pData->m_pNext)
	{
		auto* pTypeData = pData->m_ResourceTypeHash.Search(pTypeName);
		if (!pTypeData)
			continue;

		if (!pCanonPath)
		{
			pTypeData->m_pRes->AddRef();
			return pTypeData->m_pRes;
		}

		char* pResCanonPath = nullptr;
		pTypeData->m_pRes->GetCanonPath(&pResCanonPath);
		if (strcmpi(pCanonPath, pResCanonPath) == 0)
		{
			pTypeData->m_pRes->AddRef();
			Free(pResCanonPath);
			return pTypeData->m_pRes;
		}

		Free(pResCanonPath);
	}

	return nullptr;
}

///////////////////////////////////////

void cResMan::DoDumpSnapshot(FILE* fp)
{
	if (fp)
	{
		fprintf(fp, "Current Resources in Use:\n");
		fprintf(fp, "Canonical Path                 Type\tSize\tExtLock\tIntLock\tRefCnt\n");
	}
	else
	{
		mprintf("Current Resources in Use:\n");
		mprintf("Canonical Path                 Type\tSize\tExtLock\tIntLock\tRefCnt\n");
	}

	tHashSetHandle nameHandle;
	for (auto* pNameEntry = m_ResTable.GetFirst(nameHandle); pNameEntry != nullptr; pNameEntry = m_ResTable.GetNext(nameHandle))
	{
		if (pNameEntry->m_fFlags & 1)
			continue;

		for (auto* pResData = pNameEntry->m_pFirstStream; pResData != nullptr; pResData = pResData->m_pNext)
		{
			tHashSetHandle h;
			for (auto* pResEntry = pResData->m_ResourceTypeHash.GetFirst(h);
				pResEntry != nullptr; pResEntry = pResData->m_ResourceTypeHash.GetNext(h))
			{
				char* pCanonPathName = nullptr;
				pResEntry->m_pRes->GetCanonPathName(&pCanonPathName);

				char padded_name[33];
				strncpy(padded_name, pCanonPathName, 0x1Du);
				padded_name[29] = '\0';

				int i;
				for (i = static_cast<int>(strlen(padded_name)); i < 30; ++i)
					padded_name[i] = ' ';
				padded_name[i] = '\0';

				auto refcnt = pResEntry->m_pRes->AddRef() - 1;
				pResEntry->m_pRes->Release();

				if (fp)
					fprintf(fp, "%s%s\t%u\t%d\t%d\t%d\n", padded_name, pResEntry->m_pType->GetName(), pResEntry->m_nSize, pResEntry->m_nUserLockCount, pResEntry->m_nInternalLockCount, refcnt);
				else
					mprintf("%s%s\t%u\t%d\t%d\t%d\n", padded_name, pResEntry->m_pType->GetName(), pResEntry->m_nSize, pResEntry->m_nUserLockCount, pResEntry->m_nInternalLockCount, refcnt);

				Free(pCanonPathName);
			}
		}
	}
}

///////////////////////////////////////

void cResMan::UnregisterResourceData(cResourceTypeData*, int)
{
}

///////////////////////////////////////////////////////////////////////////////

tResult LGAPI _Res2Create(REFIID, IResMan** ppResMan, IUnknown* pOuter)
{
	auto* pResMan = new cResMan{ pOuter };

	if (ppResMan)
		*ppResMan = pResMan;

	return pResMan != nullptr ? S_OK : E_FAIL;
}
