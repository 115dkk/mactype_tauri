#include "override.h"
#include "ft.h"
#include <windows.h>
#include <tchar.h>
#include "fteng.h"

#ifdef _DLL
#pragma comment(linker, "/nod:msvcprt.lib /nod:msvcprtd.lib")
#endif

#if 0
#define FREETYPE_REQCOUNTMAX	10
#define GC_TRACE				TRACE
#define FREETYPE_GC_COUNTER		128
#else
#define FREETYPE_REQCOUNTMAX	2048//默认4096,每缓存该数量的文字将刷新缓存降低内存占用
#define GC_TRACE				NOP_FUNCTION
#define FREETYPE_GC_COUNTER		1024//刷新缓存后保留文字数量
#endif

FreeTypeFontEngine* g_pFTEngine;
FT_Library     freetype_library;
FTC_Manager    cache_man;
FTC_CMapCache  cmap_cache;
FTC_ImageCache image_cache;
CTLSDCArray TLSDCArray;

namespace {

struct FreeTypeFontEngineRuntime
{
	std::unique_ptr<FreeTypeFontEngine> engine;
};

FreeTypeFontEngineRuntime* GetFreeTypeFontEngineRuntime() noexcept
{
	// Explicit unload resets the engine. At process exit the tiny state is left
	// to the OS so renderer teardown does not run under the loader lock.
	static FreeTypeFontEngineRuntime* runtime = []() noexcept {
		try {
			return new FreeTypeFontEngineRuntime;
		} catch (const std::bad_alloc&) {
			return static_cast<FreeTypeFontEngineRuntime*>(nullptr);
		}
	}();
	return runtime;
}

} // namespace

bool CreateFreeTypeFontEngine() noexcept
{
	FreeTypeFontEngineRuntime* runtime = GetFreeTypeFontEngineRuntime();
	if (runtime == nullptr) {
		return false;
	}
	std::unique_ptr<FreeTypeFontEngine> engine;
	try {
		engine = std::make_unique<FreeTypeFontEngine>();
	} catch (const std::bad_alloc&) {
		return false;
	}
	runtime->engine = std::move(engine);
	g_pFTEngine = runtime->engine.get();
	return true;
}

void DestroyFreeTypeFontEngine() noexcept
{
	FreeTypeFontEngineRuntime* runtime = GetFreeTypeFontEngineRuntime();
	if (runtime != nullptr) {
		runtime->engine.reset();
	}
	g_pFTEngine = nullptr;
}

int CALLBACK EnumFontCallBack(const LOGFONT *lplf, const TEXTMETRIC *lptm, DWORD /*FontType*/, LPARAM lParam)
{
	LOGFONT* lf = reinterpret_cast<LOGFONT*>(lParam);
	StringCchCopy(lf->lfFaceName, LF_FACESIZE, lplf->lfFaceName);
	lf->lfQuality=0x2d;	//magic number
	return 0;
}



bool GetFontLocalName(TCHAR* pszFontName, __out TCHAR* pszNameOut)	//获得字体的本地化名称,返回值为字体是否存在
{
	LOGFONT lf = {0};
	TCHAR* ret;
	lf.lfQuality = 0x2d;
	if (!(ret = FontNameCache.Find(const_cast<TCHAR*>(pszFontName))))
	{
		StringCchCopy(lf.lfFaceName, LF_FACESIZE, pszFontName);
		lf.lfCharSet=DEFAULT_CHARSET;
		auto dc = renderer_raii::AdoptWindowDeviceContext(nullptr, GetDC(nullptr));
		lf.lfQuality=0;
		EnumFontFamiliesEx(dc.get(), &lf, &EnumFontCallBack, reinterpret_cast<LPARAM>(&lf), 0);
		if (lf.lfQuality==0x2d)
			FontNameCache.Add(const_cast<TCHAR*>(pszFontName), lf.lfFaceName);
		ret=lf.lfFaceName;
	}
	StringCchCopy(pszNameOut, LF_FACESIZE, ret);
	return lf.lfQuality == 0x2d;
}

LOGFONTW* GetFontNameFromFile(LPCWSTR Filename)	//获得一个字体文件内包含的所有字体名称s
{
	DWORD bufsize=0;
	if (GetFontResourceInfo(Filename, &bufsize, nullptr, 2))
	{
		renderer_raii::UniqueMallocMemory<LOGFONTW> logfonts(
			static_cast<LOGFONTW*>(malloc(bufsize + 1)));
		if (logfonts && GetFontResourceInfo(Filename, &bufsize, logfonts.get(), 2))
		{
			reinterpret_cast<char*>(logfonts.get())[bufsize]=0;
			return logfonts.release();
		}
		return nullptr;
	}
	else
	{
		AddFontResourceW(Filename);
		if (GetFontResourceInfo(Filename, &bufsize, nullptr, 2))
		{
			renderer_raii::UniqueMallocMemory<LOGFONTW> logfonts(
				static_cast<LOGFONTW*>(malloc(bufsize + 1)));
			if (logfonts && GetFontResourceInfo(Filename, &bufsize, logfonts.get(), 2))
			{
				reinterpret_cast<char*>(logfonts.get())[bufsize]=0;
				ORIG_RemoveFontResourceExW(Filename,0,nullptr);
				return logfonts.release();
			}
			ORIG_RemoveFontResourceExW(Filename,0,nullptr);
			return nullptr;
		}
		ORIG_RemoveFontResourceExW(Filename,0,nullptr);
		return nullptr;
	}
}

template <class T>
struct GCCounterSortFunc : public std::binary_function<const T*, const T*, bool>
{
	bool operator()(const T* arg1, const T* arg2) const
	{
		const int cnt1 = arg1 ? arg1->GetMruCounter() : -1;
		const int cnt2 = arg2 ? arg2->GetMruCounter() : -1;
		return cnt1 > cnt2;
	}
};

struct DeleteCharFunc : public std::unary_function<FreeTypeCharData*&, void>
{
	void operator()(FreeTypeCharData*& arg) const
	{
		if (!arg)
			return;
		delete arg;
		arg = nullptr;
	}
};

template <class T>
void CompactMap(T& pp, int count, int reduce)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTCACHE);
	int reducecount = pp.size() - reduce;
	T::iterator it= pp.begin();
	for (int i=0;i<reducecount;i++) //删除超过FREETYPE_GC_COUNTER之后的缓存
	{
		//it->second->Erase();
		delete it->second;
		pp.erase(it++);
	}
	return;
}

template <class T>
void Compact(T** pp, int count, int reduce)
{
	Assert(count >= 0);
	Assert(reduce > 0);
	if (!pp || !count || count < reduce) {
		return;
	}
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTCACHE);
	TRACE(_T("Compact(0x%p, %d, %d)\n"), pp, count, reduce);
	//GCモドキ
	//T::m_mrucounterの降順に並ぶので
	//reduceを超える部分を削除する

	std::vector<T*> ppTemp(pp, pp + count);

	std::sort(ppTemp.begin(), ppTemp.end(), GCCounterSortFunc<T>());
	int i;
	for (i=0; i<reduce; i++) {
		if (!ppTemp[i])
			break;
		ppTemp[i]->ResetMruCounter();
	}

	//GC_TRACE(_T("GC:"));
	for (i=reduce; i<count; i++) {
		if (!ppTemp[i])
			break;
		//GC_TRACE(_T(" %wc"), ppTemp[i]->GetChar());
		ppTemp[i]->Erase();
	}
	//GC_TRACE(_T("\n"));
}

//FreeTypeCharData
FreeTypeCharData::FreeTypeCharData(FreeTypeCharData** ppCh, FreeTypeCharData** ppGl, WCHAR wch, UINT glyphindex, int width, int mru, int gdiWidth, int AAMode)
	: m_ppSelfGlyph(ppGl), m_glyphindex(glyphindex), m_width(width)
	, m_glyph(nullptr), m_glyphMono(nullptr), m_bmpSize(0), m_bmpMonoSize(0)
	, FreeTypeMruCounter(mru), m_gdiWidth(gdiWidth), m_AAMode(AAMode)
{
	g_pFTEngine->AddMemUsedObj(this);
	AddChar(ppCh);
#ifdef _DEBUG
	m_wch = wch;
#endif
}

FreeTypeCharData::~FreeTypeCharData()
{
	CharDataArray& arr = m_arrSelfChar;
	int n = arr.GetSize();
	while (n--) {
		FreeTypeCharData** pp = arr[n];
		Assert(*pp == this);
		*pp = nullptr;
	}
	if(m_ppSelfGlyph) {
		Assert(*m_ppSelfGlyph == this);
		*m_ppSelfGlyph = nullptr;
	}
	if(m_glyph){
		FT_Done_Ref_Glyph(reinterpret_cast<FT_Referenced_Glyph*>(&m_glyph));
	}
	if(m_glyphMono){
		FT_Done_Ref_Glyph(reinterpret_cast<FT_Referenced_Glyph*>(&m_glyphMono));
	}

	g_pFTEngine->SubMemUsed(m_bmpSize);
	g_pFTEngine->SubMemUsed(m_bmpMonoSize);
	g_pFTEngine->SubMemUsedObj(this);
}

void FreeTypeCharData::SetGlyph(FT_Render_Mode render_mode, FT_Referenced_BitmapGlyph glyph)
{
	const bool bMono = (render_mode == FT_RENDER_MODE_MONO);
	FT_Referenced_BitmapGlyph& gl = bMono ? m_glyphMono : m_glyph;
	if (gl)
		FT_Done_Ref_Glyph(reinterpret_cast<FT_Referenced_Glyph*>(&gl));
	{
		FT_Glyph_Ref_Copy(reinterpret_cast<FT_Referenced_Glyph>(glyph), reinterpret_cast<FT_Referenced_Glyph*>(&gl));
		if (gl) {
			int& size = bMono ? m_bmpMonoSize : m_bmpSize;
			size  = FT_Bitmap_CalcSize(gl->ft_glyph);
			size += sizeof(FT_BitmapGlyphRec);
			g_pFTEngine->AddMemUsed(size);
		}
	}
}


//FreeTypeFontCache
FreeTypeFontCache::FreeTypeFontCache(/*int px, int weight, bool italic, */int mru)
	: /*m_px(px), m_weight(weight), m_italic(italic), */m_active(false)
	, FreeTypeMruCounter(mru)
{
#ifdef _USE_ARRAY
	ZeroMemory(&m_tm, sizeof(TEXTMETRIC));
	ZeroMemory(m_chars, sizeof(m_chars));
	ZeroMemory(m_glyphs, sizeof(m_glyphs));
#else
	m_GlyphCache.clear();
#endif
	g_pFTEngine->AddMemUsedObj(this);
}

FreeTypeFontCache::~FreeTypeFontCache()
{
	Erase();
	g_pFTEngine->SubMemUsedObj(this);
}

void FreeTypeFontCache::Compact()
{
	//TRACE(_T("FreeTypeFontCache::Compact: %d > %d\n"), countof(m_chars), FREETYPE_GC_COUNTER);
	ResetGCCounter();
#ifdef _USE_ARRAY
	::Compact(m_glyphs, countof(m_glyphs), FREETYPE_GC_COUNTER);
#else
	::CompactMap(m_GlyphCache, m_GlyphCache.size(), FREETYPE_GC_COUNTER);
#endif
	//GlyphCache::const_iterator it=m_GlyphCache.begin();
}

void FreeTypeFontCache::Erase()
{
	m_active = false;
#ifdef _USE_ARRAY
	std::for_each(m_chars,  m_chars  + FT_MAX_CHARS, DeleteCharFunc());
	std::for_each(m_glyphs, m_glyphs + FT_MAX_CHARS, DeleteCharFunc());
#else
	GlyphCache::iterator it=m_GlyphCache.begin();
	for (;it!=m_GlyphCache.end();++it)
		delete it->second;
	m_GlyphCache.clear();
#endif
}

void FreeTypeFontCache::AddCharData(WCHAR wch, UINT glyphindex, int width, int gdiWidth, FT_Referenced_BitmapGlyph glyph, FT_Render_Mode render_mode, int AAMode)
{
	if (glyphindex & 0xffff0000 /*|| !g_ccbCache*/) {
		return;
	}
	if (AddIncrement() >= FREETYPE_REQCOUNTMAX) {	//先压缩，避免压缩后丢失的问题
		Compact();
	}

#ifdef _USE_ARRAY
	FreeTypeCharData** ppChar  = _GetChar(wch);
	if (*ppChar) {
		(*ppChar)->SetGlyph(render_mode, glyph);
		(*ppChar)->SetMruCounter(this);
		return;
	}
#else
	GlyphCache::iterator it=m_GlyphCache.find(wch);
	if (it!=m_GlyphCache.end())	//找到了旧数据
	{
		FreeTypeCharData* ppChar  = it->second;
		if (ppChar) {
			ppChar->SetGlyph(render_mode, glyph);
			ppChar->SetMruCounter(this);
			ppChar->SetWidth(width);
			ppChar->SetGDIWidth(gdiWidth);
			return;
		}
	}
#endif
	std::unique_ptr<FreeTypeCharData> charData;
	try {
		charData = std::make_unique<FreeTypeCharData>(
			/*ppChar*/nullptr, nullptr, wch, glyphindex, width, MruIncrement(), gdiWidth, AAMode);
	} catch (const std::bad_alloc&) {
		return;
	}
	charData->SetGlyph(render_mode, glyph);

#ifdef _USE_ARRAY
	*ppChar = charData.release();
#else
	m_GlyphCache[wch] = charData.release();
#endif

}

void FreeTypeFontCache::AddGlyphData(UINT glyphindex, int width, int gdiWidth, FT_Referenced_BitmapGlyph glyph, FT_Render_Mode render_mode, int AAMode)
{
	if (glyphindex & 0xffff0000 /*|| !g_ccbCache*/) {
		return;
	}
	//GC
	if (AddIncrement() >= FREETYPE_REQCOUNTMAX) {
		//TRACE(_T("Compact(0x%p)\n"), this);
		Compact();
	}

#ifdef _USE_ARRAY
	FreeTypeCharData** ppGlyph  = _GetGlyph(glyphindex);
	if (*ppGlyph) {
		(*ppGlyph)->SetGlyph(render_mode, glyph);
		(*ppGlyph)->SetMruCounter(this);
		return;
	}
#else
	GlyphCache::iterator it=m_GlyphCache.find(-static_cast<int>(glyphindex));
	if (it!=m_GlyphCache.end())	//找到了旧数据
	{
		FreeTypeCharData* ppChar  = it->second;
		if (ppChar) {
			(ppChar)->SetGlyph(render_mode, glyph);
			(ppChar)->SetMruCounter(this);
			ppChar->SetWidth(width);
			ppChar->SetGDIWidth(gdiWidth);
			return;
		}
	}
#endif

	//追加(glyphのみ)
	std::unique_ptr<FreeTypeCharData> charData;
	try {
		charData = std::make_unique<FreeTypeCharData>(
			nullptr, /*ppGlyph*/nullptr, 0, glyphindex, width, MruIncrement(), gdiWidth, AAMode);
	} catch (const std::bad_alloc&) {
		return;
	}
	charData->SetGlyph(render_mode, glyph);

#ifdef _USE_ARRAY
	*ppGlyph = charData.release();
#else
	m_GlyphCache[-static_cast<int>(glyphindex)] = charData.release();
#endif
}


//FreeTypeFontInfo
void FreeTypeFontInfo::Compact()
{
	//TRACE(_T("FreeTypeFontInfo::Compact: %d > %d\n"), m_cache.GetSize(), m_nMaxSizes);
	ResetGCCounter();
	::CompactMap(m_cache, m_cache.size(), m_nMaxSizes);
	CacheArray::const_iterator it=m_cache.begin();
	for (;it!=m_cache.end();++it)
		it->second->Deactive();
}

void FreeTypeFontInfo::Createlink()
{
	CFontFaceNamesEnumerator fn(m_hash.c_str(), m_nFontFamily);
	std::set<int> linkset;	//链接字体集合，防止重复链接，降低效率
	linkset.insert(m_id);
	face_id_link[m_linknum] = reinterpret_cast<FTC_FaceID>(m_id);
	ggo_link[m_linknum++] = m_ggoFont.get();	//第一个链接一定是自己，不需要获取
	LOGFONT lf;
	BOOL IsSimSun = false;
	memset(&lf, 0, sizeof(LOGFONT));
	lf.lfCharSet=DEFAULT_CHARSET;
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	for (fn.next() ; !fn.atend(); fn.next()) {	//跳过第一个链接
		//FreeTypeFontInfo* pfitemp = g_pFTEngine->FindFont(fn, m_weight, m_italic);
		//if (pfitemp && pfitemp->m_isSimSun)
		//	IsSimSun = true;
		if (!m_SimSunID)
			IsSimSun = (_wcsicmp(fn,L"宋体")==0 || _wcsicmp(fn,L"SimSun")==0);
		StringCchCopy(lf.lfFaceName, LF_FACESIZE, fn);
		if (!_wcsicmp(fn, m_familyname.c_str()))	// allow font link to itself
			pSettings->CopyForceFont(lf,lf);
		FreeTypeFontInfo* pfitemp = g_pFTEngine->FindFont(lf.lfFaceName, /*m_weight*/0, /*m_italic*/false);
		if (pfitemp && linkset.find(pfitemp->GetId())==linkset.end()) {
			linkset.insert(pfitemp->GetId());
			face_id_link[m_linknum] = reinterpret_cast<FTC_FaceID>(pfitemp->GetId());
			ggo_link[m_linknum++] = pfitemp->GetGGOFont();
		if (!m_SimSunID && IsSimSun)
			m_SimSunID = reinterpret_cast<FTC_FaceID>(pfitemp->GetId());
		}
	}
}

bool FreeTypeFontInfo::EmbeddedBmpExist(int px)
{
	if (px>=256 || px<0)
		return false;
	if (m_ebmps[px]!=-1)
		return !!m_ebmps[px];
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
	FTC_ImageTypeRec imgtype={reinterpret_cast<FTC_FaceID>(m_id), static_cast<FT_UInt>(px), static_cast<FT_UInt>(px), FT_LOAD_DEFAULT};	//构造一个当前大小的imagetype
	FT_Glyph temp_glyph=nullptr;
	FT_UInt gindex = FTC_CMapCache_Lookup(cmap_cache, reinterpret_cast<FTC_FaceID>(m_id), -1, static_cast<FT_UInt32>(L'0'));	//获得0的索引值
	FTC_ImageCache_Lookup(image_cache, &imgtype, gindex, &temp_glyph, nullptr);
	if (temp_glyph && temp_glyph->format==FT_GLYPH_FORMAT_BITMAP)	//如果可以读到0的点阵
		m_ebmps[px]=1;	//则该字号存在点阵
	else
	{
		gindex = FTC_CMapCache_Lookup(cmap_cache, reinterpret_cast<FTC_FaceID>(m_id), -1, static_cast<FT_UInt32>(L'的'));	//获得"的"的索引值
		if (gindex)
			FTC_ImageCache_Lookup(image_cache, &imgtype, gindex, &temp_glyph, nullptr);	//读取“的”的点阵
		if (temp_glyph && temp_glyph->format==FT_GLYPH_FORMAT_BITMAP)	//如果可以读到0的点阵
			m_ebmps[px]=1;	//则该字号存在点阵
		else
			m_ebmps[px]=0;
	}
	return !!m_ebmps[px];
}

FreeTypeFontCache* FreeTypeFontInfo::GetCache(FTC_ScalerRec& scaler, const LOGFONT& lf)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTCACHE);

	if (AddIncrement() > m_nMaxSizes) {	//先压缩
		Compact();
	}
	int weight = lf.lfWeight;
	weight = weight < FW_BOLD ? 0: 1/*FW_BOLD*/;
	const bool italic = !!lf.lfItalic;
	if (scaler.height>0xfff || scaler.width>0xfff/* || scaler.height<0 || scaler.width<0*/)	//超大字体不渲染
		return nullptr;
	FreeTypeFontCache* p = nullptr;
	UINT hash=getCacheHash(scaler.height, weight, italic, lf.lfWidth ? scaler.width : 0);	//计算hash
	CacheArray::iterator it=m_cache.find(hash); //寻找cache
	if (it!=m_cache.end())//cache存在
	{
		p = it->second;
	}
	else
	{
		std::unique_ptr<FreeTypeFontCache> cache;
		try {
			cache = std::make_unique<FreeTypeFontCache>(
				/*scaler.height, weight, italic,*/ MruIncrement());
		} catch (const std::bad_alloc&) {
			return nullptr;
		}
		m_cache[hash] = cache.release();
		p = m_cache[hash];
	}

	Assert(p != nullptr);
	if (p && p->Activate()) {
		DecIncrement();	//重复使用则减计数值
	}
	return p;
}


//FreeTypeFontEngine
void FreeTypeFontEngine::Compact()
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);

	TRACE(_T("FreeTypeFontEngine::Compact: %d > %d\n"), m_mfontMap.size(), m_nMaxFaces);
	ResetGCCounter();
	//memset(m_arrFace, 0, sizeof(FT_Face)*m_nFaceCount);	//超过最大face数了，老的face会被ft释放掉，所以需要全部重新获取
	//FontListArray& arr = m_arrFontList;
	//::Compact(arr.GetData(), arr.GetSize(), m_nMaxFaces);
}

BOOL FreeTypeFontEngine::RemoveFont(FreeTypeFontInfo* fontinfo)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	{
		FontMap::const_iterator iter=m_mfontMap.begin();	//遍历fontmap
		while (iter!=m_mfontMap.end())
		{
			FreeTypeFontInfo* p = iter->second;
			if (p==fontinfo)
				m_mfontMap.erase(iter++);	//删除引用
			else
				++iter;
		}
	}
	{
		FullNameMap::const_iterator iter=m_mfullMap.begin();	//遍历fullmap
		while (iter!=m_mfullMap.end())
		{
			FreeTypeFontInfo* p = iter->second;
			if (p==fontinfo)
				m_mfullMap.erase(iter++);	//删除引用
			else
			{
				iter->second->UpdateFontSetting();
				++iter;
			}
		}
	}
	delete fontinfo;
	return true;
}

BOOL FreeTypeFontEngine::RemoveThisFont(FreeTypeFontInfo* fontinfo, LOGFONT* lg)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	{
		FontMap::const_iterator iter=m_mfontMap.find(myfont(lg->lfFaceName, CalcBoldWeight(lg->lfWeight), lg->lfItalic));	//遍历fontmap
		if (iter!=m_mfontMap.end())
			m_mfontMap.erase(iter);	//删除引用
	}
	{
		FullNameMap::const_iterator iter=m_mfullMap.find(fontinfo->GetFullName());	//遍历fullmap
		if (iter!=m_mfullMap.end())
			m_mfullMap.erase(iter);	//删除引用
	}
	delete fontinfo;
	return true;
}

BOOL FreeTypeFontEngine::RemoveFont(LPCWSTR FontName)
{
	if (!FontName) return false;
	renderer_raii::UniqueMallocMemory<LOGFONTW> fontarrayOwner(GetFontNameFromFile(FontName));
	LOGFONTW* fontarray = fontarrayOwner.get();
	if (!fontarray) return false;
	FTC_FaceID fid = nullptr;
	BOOL bIsFontLoaded, bIsFontFileLoaded = false;
	COwnedCriticalSectionLock __lock2(2, COwnedCriticalSectionLock::OCS_DC);	//获取所有权，现在要处理DC，禁止所有绘图函数访问
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
	while (*reinterpret_cast<char*>(fontarray))
	{
		bIsFontLoaded = false;
		FreeTypeFontInfo* result = FindFont(fontarray->lfFaceName, fontarray->lfWeight, !!fontarray->lfItalic, false, &bIsFontLoaded);
		if (result)
		{
			fid = reinterpret_cast<FTC_FaceID>(result->GetId());
			if (bIsFontLoaded)	//该字体已经被使用过
			{
				RemoveFont(result);	//枚举字体信息全部删除
				bIsFontFileLoaded = true;	//设置字体文件也被使用过
			}
			else
				RemoveThisFont(result, fontarray);
			CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);
			FTC_Manager_RemoveFaceID(cache_man, fid);
			m_mfontList[static_cast<int>(reinterpret_cast<INT_PTR>(fid)) - 1] = nullptr;
		}
		fontarray++;
	}
	if (bIsFontFileLoaded)	//若字体文件被使用过，则需要清楚所有DC
	{
		CTLSDCArray::iterator iter = TLSDCArray.begin();
		while (iter!=TLSDCArray.end())
		{
			(static_cast<CBitmapCache*>(*iter))->~CBitmapCache();	//清除掉所有使用中的DC
			++iter;
		}
	}
	return true;
}

static int FaceIDHolder = 0;
int GetFaceID(void)
{
	return static_cast<int>(InterlockedIncrement(reinterpret_cast<LONG volatile*>(&FaceIDHolder)));
}

void ReleaseFaceID(void)
{
	InterlockedDecrement(reinterpret_cast<LONG volatile*>(&FaceIDHolder));
}

FreeTypeFontInfo* FreeTypeFontEngine::AddFont(void* lpparams)
{
	FREETYPE_PARAMS* params = static_cast<FREETYPE_PARAMS*>(lpparams);
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);
	const LOGFONT& lplf = *params->lplf;
	if(!*lplf.lfFaceName || _tcslen(lplf.lfFaceName) == 0)
		return nullptr;

	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	//const CFontSettings& fs = pSettings->FindIndividual(params->strFamilyName.c_str());
	const int faceId = GetFaceID();
	std::unique_ptr<FreeTypeFontInfo> newFont;
	try {
		newFont = std::make_unique<FreeTypeFontInfo>(
			/*m_mfullMap.size() + 1*/faceId, lplf.lfFaceName, lplf.lfWeight,
			!!lplf.lfItalic, MruIncrement(), params->strFullName, params->strFamilyName);
	} catch (const std::bad_alloc&) {
		ReleaseFaceID();
		return nullptr;
	}
	FreeTypeFontInfo* pfi = newFont.get();

	if (pfi->GetFullName().size()==0)	//点阵字
		{
			ReleaseFaceID();
			return nullptr;
		}
/*
	TCHAR buff[255]={0};
	if (params->strFamilyName.length()==8)
	{
		wsprintf(buff, L"Adding familiyname \"%s\" fullname \"%s\" weight %d\n\result: \"%s\"\n", params->strFamilyName.c_str(), params->strFullName.c_str(),
			params->lplf->lfWeight, pfi->GetFullName().c_str());
		Log(buff);
	}*/

	FullNameMap::const_iterator it = m_mfullMap.find(pfi->GetFullName());
	if (it!=m_mfullMap.end())	//是已经存在的字体了,原因是字体替换使两种名字指向一个字体
	{
		ReleaseFaceID();
		pfi = it->second;//指向原字体
	}
	else
	{
		m_mfullMap[pfi->GetFullName()]=pfi;	//不存在，添加到map表
		m_mfontList.push_back(pfi);
		newFont.release();
	}

	if (pfi->GetFullName()!=params->strFullName)	//如果目标字体的真实名称和需要的名称不一样，说明是字体替换
	{
		pfi->AddRef();	//增加引用计数
		m_mfullMap[params->strFullName] = pfi;	//双重引用，指向同一个字体
	}

	//bool ret = !!arr.Add(pfi);
	//weight = weight < FW_BOLD ? 0: FW_BOLD;
	myfont font(lplf.lfFaceName, lplf.lfWeight, !!params->otm->otmTextMetrics.tmItalic);
	/*
	FontMap::const_iterator it = m_mfontMap.find(font);
		if (it!=m_mfontMap.end())
		{
			it->second->Release();
		}*/

	m_mfontMap[font]=pfi;
	/*
	if (!ret) {
	delete pfi;
	return nullptr;
	}*/


#ifdef _DEBUG
	{
		const CFontSettings& fs = pfi->GetFontSettings();
		TRACE(_T("AddFont: %s, %d, %d, %d, %d, %d, %d\n"), pfi->GetName(),
			fs.GetParam(0), fs.GetParam(1), fs.GetParam(2), fs.GetParam(3), fs.GetParam(4), fs.GetParam(5));
	}
#endif
	return pfi;
}

FreeTypeFontInfo* FreeTypeFontEngine::AddFont(LPCTSTR lpFaceName, int weight, bool italic, BOOL* bIsFontLoaded)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);
	if(lpFaceName == nullptr || _tcslen(lpFaceName) == 0/* || FontExists(lpFaceName, weight, italic)*/)
		return nullptr;

	//FontListArray& arr = m_arrFontList;

	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	//const CFontSettings& fs = pSettings->FindIndividual(lpFaceName);
	wstring dumy;
	//dumy.clear();
	const int faceId = GetFaceID();
	std::unique_ptr<FreeTypeFontInfo> newFont;
	try {
		newFont = std::make_unique<FreeTypeFontInfo>(
			/*m_mfullMap.size() + 1*/faceId, lpFaceName, weight, italic, MruIncrement(), dumy, dumy);
	} catch (const std::bad_alloc&) {
		ReleaseFaceID();
		return nullptr;
	}
	FreeTypeFontInfo* pfi = newFont.get();
	if (pfi->GetFullName().size()==0)	//点阵字
	{
		ReleaseFaceID();
		return nullptr;
	}

	FullNameMap::const_iterator it = m_mfullMap.find(pfi->GetFullName()); //是否在主map表中存在了
	if (it!=m_mfullMap.end())	//已经存在
	{
		ReleaseFaceID();
		pfi = it->second;	//指向已经存在的字体
		if (bIsFontLoaded)
			*bIsFontLoaded = true;
		//pfi->AddRef();
	}
	else
	{
		m_mfullMap[pfi->GetFullName()]=pfi;	//不存在，添加到map表
		m_mfontList.push_back(pfi);
		newFont.release();
		if (bIsFontLoaded)
			*bIsFontLoaded = false;
	}

	//bool ret = !!arr.Add(pfi);
	//weight = weight < FW_BOLD ? 0: FW_BOLD;
	myfont font(lpFaceName, weight, italic);
	m_mfontMap[font]=pfi;		//添加在次要map表
/*
	if (!ret) {
		delete pfi;
		return nullptr;
	}*/


#ifdef _DEBUG
	{
		const CFontSettings& fs = pfi->GetFontSettings();
		TRACE(_T("AddFont: %s, %d, %d, %d, %d, %d, %d\n"), pfi->GetName(),
				fs.GetParam(0), fs.GetParam(1), fs.GetParam(2), fs.GetParam(3), fs.GetParam(4), fs.GetParam(5));
	}
#endif
	return pfi;
}

int FreeTypeFontEngine::GetFontIdByName(LPCTSTR lpFaceName, int weight, bool italic)
{
	const FreeTypeFontInfo* pfi = FindFont(lpFaceName, weight, italic);
	return pfi ? pfi->GetId() : 0;
}

/*
LPCTSTR FreeTypeFontEngine::GetFontById(int faceid, int& weight, bool& italic)
{
	CCriticalSectionLock __lock;

	FreeTypeFontInfo** pp	= m_arrFontList.Begin();
	FreeTypeFontInfo** end	= m_arrFontList.End();
	for(; pp != end; ++pp) {
		FreeTypeFontInfo* p = *pp;
		if (p->GetId() == faceid) {
			p->SetMruCounter(this);
			weight = p->GetWeight();
			italic = p->IsItalic();
			return p->GetName();
		}
	}
	return nullptr;
}
*/
FreeTypeFontInfo* FreeTypeFontEngine::FindFont(void* lpparams)
{
	FREETYPE_PARAMS* params = static_cast<FREETYPE_PARAMS*>(lpparams);
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	FullNameMap::const_iterator iter=m_mfullMap.find(params->strFullName);
	if (iter!=m_mfullMap.end())
	{
		FreeTypeFontInfo* p = iter->second;
		if (p->GetFullName()!=params->strFullName)	//属于替换字体
			return FindFont(params->lplf->lfFaceName, params->lplf->lfWeight, !!params->lplf->lfItalic);
		p->SetMruCounter(this);
		return p;
	}
	//m_bAddOnFind = true;
	return AddFont(params);
}

FreeTypeFontInfo* FreeTypeFontEngine::FindFont(LPCTSTR lpFaceName, int weight, bool italic, bool AddOnFind, BOOL* bIsFontLoaded)
{
/*
	if (m_bAddOnFind)
	{
		m_bAddOnFind = false;
		return nullptr;
	}*/

	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	weight = CalcBoldWeight(weight);
	myfont font(lpFaceName, weight, italic);
	FontMap::const_iterator iter=m_mfontMap.find(font);
	if (iter!=m_mfontMap.end())
	{
		FreeTypeFontInfo* p = iter->second;
		p->SetMruCounter(this);
/*
		TCHAR buff[255]={0};
		if (wcslen(lpFaceName)==8)
		{
			wsprintf(buff, L"Finding familiyname \"%s\" weight %d\n\tFound: \"%s\"\n", lpFaceName,
				weight, p->GetFullName().c_str());
			Log(buff);
		}*/
		if (bIsFontLoaded)
			*bIsFontLoaded = true;
		return p;
	}
	//m_bAddOnFind = true;
	return AddFont(lpFaceName, weight, italic, bIsFontLoaded);
}

FreeTypeFontInfo* FreeTypeFontEngine::FindFont(int faceid)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	if (faceid>m_mfontList.size())
		return nullptr;
	else
		return m_mfontList[faceid-1];	//存在bug！！！
	/*
	FullNameMap::const_iterator iter=m_mfullMap.begin();
		for(; iter != m_mfullMap.end(); ++iter) {
			FreeTypeFontInfo* p = iter->second;
			if (p->GetId() == faceid) {
				p->SetMruCounter(this);
				return p;
			}
		}
		return nullptr;*/

}


//FreeTypeSysFontData
// http://kikyou.info/diary/?200510#i10 を参考にした
#include <freetype/tttables.h>	// FT_TRUETYPE_TABLES_H
#include <mmsystem.h>	//mmioFOURCC
#define TVP_TT_TABLE_ttcf	mmioFOURCC('t', 't', 'c', 'f')
#define TVP_TT_TABLE_name	mmioFOURCC('n', 'a', 'm', 'e')

// Windowsに登録されているフォントのバイナリデータを名称から取得
FreeTypeSysFontData* FreeTypeSysFontData::CreateInstance(LPCTSTR name, int weight, bool italic)
{
	std::unique_ptr<FreeTypeSysFontData> pData(new FreeTypeSysFontData);
	if (!pData) {
		return nullptr;
	}
	if (!pData->Init(name, weight, italic)) {
		return nullptr;
	}
	return pData.release();
}

bool FreeTypeSysFontData::Init(LPCTSTR name, int weight, bool italic)
{
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	renderer_raii::UniqueMallocMemory<unsigned char> pNameFromGDI;
	renderer_raii::UniqueMallocMemory<unsigned char> pNameFromFreeType;
	DWORD cbNameTable;
	DWORD cbFontData;
	int index;
	DWORD buf;
	FT_StreamRec& fsr = m_ftStream;
	m_name.assign(name);
	m_hdc.reset(CreateCompatibleDC(nullptr));
	if(!m_hdc) {
		return false;
	}
	// 名前以外適当
	if (pSettings->FontSubstitutes() < SETTING_FONTSUBSTITUTE_ALL)
	{
		m_font.reset(CreateFont(
					12, 0, 0, 0, weight,
					italic, FALSE, FALSE,
					DEFAULT_CHARSET,
					OUT_DEFAULT_PRECIS,
					FONT_MAGIC_NUMBER,
					DEFAULT_QUALITY,
					DEFAULT_PITCH | FF_DONTCARE,
					name));
	}
	else
		m_font.reset(CreateFont(
					12, 0, 0, 0, weight,
					italic, FALSE, FALSE,
					DEFAULT_CHARSET,
					OUT_DEFAULT_PRECIS,
					CLIP_DEFAULT_PRECIS,
					DEFAULT_QUALITY,
					DEFAULT_PITCH | FF_DONTCARE,
					name));

	if(!m_font){
		return false;
	}

	m_selectedFont = renderer_raii::SelectObject(m_hdc.get(), m_font.get());
	if (!m_selectedFont) {
		return false;
	}
	// フォントデータが得られそうかチェック
	cbNameTable = ORIG_GetFontData(m_hdc.get(), TVP_TT_TABLE_name, 0, nullptr, 0);
	if(cbNameTable == GDI_ERROR){
		goto ERROR_Init;
	}

	pNameFromGDI.reset(static_cast<unsigned char*>(malloc(cbNameTable)));
	if (!pNameFromGDI) {
		goto ERROR_Init;
	}
	pNameFromFreeType.reset(static_cast<unsigned char*>(malloc(cbNameTable)));
	if (!pNameFromFreeType) {
		goto ERROR_Init;
	}

	//- name タグの内容をメモリに読み込む
	if(ORIG_GetFontData(m_hdc.get(), TVP_TT_TABLE_name, 0, pNameFromGDI.get(), cbNameTable) == GDI_ERROR){
		goto ERROR_Init;
	}

	// フォントサイズ取得処理
	cbFontData = ORIG_GetFontData(m_hdc.get(), TVP_TT_TABLE_ttcf, 0, &buf, 1);
	if(cbFontData == 1){
		// TTC ファイルだと思われる
		cbFontData = ORIG_GetFontData(m_hdc.get(), TVP_TT_TABLE_ttcf, 0, nullptr, 0);
		m_isTTC = true;
	}
	else{
		cbFontData = ORIG_GetFontData(m_hdc.get(), 0, 0, nullptr, 0);
	}
	if(cbFontData == GDI_ERROR){
		// エラー; GetFontData では扱えなかった
		goto ERROR_Init;
	}

	if (pSettings->UseMapping()) {
		auto hmap = renderer_raii::AdoptHandle(CreateFileMapping(
			INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT | SEC_NOCACHE,
			0, cbFontData, nullptr));
		if (!hmap) {
			goto ERROR_Init;
		}
		m_pMapping.reset(MapViewOfFile(hmap.get(), FILE_MAP_ALL_ACCESS, 0, 0, cbFontData));

		if (m_pMapping) {
			if (ORIG_GetFontData(m_hdc.get(), m_isTTC ? TVP_TT_TABLE_ttcf : 0,
				0, m_pMapping.get(), cbFontData) != GDI_ERROR) {
				m_mappingLock = renderer_raii::PageLock::TryLock(m_pMapping.get(), cbFontData);
				if (m_mappingLock) {
					m_dwSize = cbFontData;
				} else {
					m_pMapping.reset();
				}
			} else {
				m_pMapping.reset();
			}
		}
	}

	// FT_StreamRec の各フィールドを埋める
	fsr.base				= 0;
	fsr.size				= cbFontData;
	fsr.pos					= 0;
	fsr.descriptor.pointer	= this;
	fsr.pathname.pointer	= nullptr;
	fsr.read				= IoFunc;
	fsr.close				= CloseFunc;

	index = 0;
	m_locked = true;
	if(!OpenFaceByIndex(index)){
		goto ERROR_Init;
	}

	for(;;) {
		// FreeType から、name タグのサイズを取得する
		FT_ULong length = 0;
		FT_Error err = FT_Load_Sfnt_Table(m_ftFace.get(), TTAG_name, 0, nullptr, &length);
		if(err){
			goto ERROR_Init;
		}

		// FreeType から得た name タグの長さを Windows から得た長さと比較
		if(length == cbNameTable){
			// FreeType から name タグを取得
			err = FT_Load_Sfnt_Table(m_ftFace.get(), TTAG_name, 0, pNameFromFreeType.get(), &length);
			if(err){
				goto ERROR_Init;
			}
			// FreeType から読み込んだ name タグの内容と、Windows から読み込んだ
			// name タグの内容を比較する。
			// 一致していればその index のフォントを使う。
			if(!memcmp(pNameFromGDI.get(), pNameFromFreeType.get(), cbNameTable)){
				// 一致した
				// face は開いたまま
				break; // ループを抜ける
			}
		}

		// 一致しなかった
		// インデックスを一つ増やし、その face を開く
		index ++;

		if(!OpenFaceByIndex(index)){
			// 一致する face がないまま インデックスが範囲を超えたと見られる
			// index を 0 に設定してその index を開き、ループを抜ける
			index = 0;
			if(!OpenFaceByIndex(index)){
				goto ERROR_Init;
			}
			break;
		}
	}

	m_locked = false;
	return true;

ERROR_Init:
	m_ftFace.reset();
	m_selectedFont.reset();
	m_font.reset();
	m_locked = false;
	return false;
}

unsigned long FreeTypeSysFontData::IoFunc(
			FT_Stream		stream,
			unsigned long	offset,
			unsigned char*	buffer,
			unsigned long	count )
{
	if(count == 0){
		return 0;
	}

	FreeTypeSysFontData * pThis = reinterpret_cast<FreeTypeSysFontData*>(stream->descriptor.pointer);
	Assert(pThis != nullptr);

	DWORD result = 0;
	if (pThis->m_pMapping) {
		result = Min(pThis->m_dwSize - offset, count);
		memcpy(buffer, static_cast<const BYTE*>(pThis->m_pMapping.get()) + offset, result);
	} else {
		result = ::GetFontData(pThis->m_hdc.get(), pThis->m_isTTC ? TVP_TT_TABLE_ttcf : 0, offset, buffer, count);
		if(result == GDI_ERROR) {
			// エラー
			return 0;
		}
	}
	return result;
}

void FreeTypeSysFontData::CloseFunc(FT_Stream stream)
{
	FreeTypeSysFontData * pThis = reinterpret_cast<FreeTypeSysFontData*>(stream->descriptor.pointer);
	Assert(pThis != nullptr);

	if(!pThis->m_locked)
		delete pThis;
}

bool FreeTypeSysFontData::OpenFaceByIndex(int index)
{
	m_ftFace.reset();

	FT_Open_Args args = { 0 };
	args.flags		= FT_OPEN_STREAM;
	args.stream		= &m_ftStream;

	// FreeType で扱えるか？
	FT_Face face = nullptr;
	FT_Error ftErrCode = FT_Open_Face(freetype_library, &args, index, &face);
	m_ftFace.reset(face);
#ifdef DEBUG
	if (ftErrCode!=0)
		TRACE(L"Open face failed, error = %d\n", ftErrCode);
#endif
	return (ftErrCode == 0);
}
