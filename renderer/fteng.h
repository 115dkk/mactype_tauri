#pragma once

#include <ft2build.h>
#include <freetype/freetype.h>	/* FT_FREETYPE_H */
#include <freetype/ftcache.h>	/* FT_CACHE_H */
#include <freetype/tttables.h>
#include <freetype/tttags.h>	// FT_TRUETYPE_TAGS_H
#include <freetype/ftoutln.h>
#include <atomic>
#include <vector>
#include "ftref.h"
#include "freetype_raii.h"
#include "freetype_runtime.h"
#include "profile_runtime.h"
#include <math.h>
#include "undocAPI.h"

class FreeTypeFontEngine;
extern FreeTypeFontEngine* g_pFTEngine;
bool CreateFreeTypeFontEngine() noexcept;
void DestroyFreeTypeFontEngine() noexcept;
extern BOOL g_ccbCache;
extern BOOL g_ccbIndividual;
extern FTC_Manager    cache_man;

typedef set<CBitmapCache*> CTLSDCArray;
extern CTLSDCArray TLSDCArray;

LOGFONTW* GetFontNameFromFile(LPCTSTR Filename);
bool GetFontLocalName(TCHAR* pszFontName, __out TCHAR* pszNameOut);	//获得字体的本地化名称

struct myfont
{
	wstring name;
	int hash;
	bool operator < (const myfont& mf) const {
		return name==mf.name? hash<mf.hash: name<mf.name;
	}
public:
	myfont(LPCWSTR lpFaceName, int nweight, int nitalic):
	name(lpFaceName),hash((nitalic<<31) | nweight)
	{}
};

enum FT_EngineConstants {
	FT_MAX_CHARS	= 65536,
};

class FreeTypeGCCounter
{
private:
	int m_addcount;		//追加用
	int m_mrucount;		//MRU用

public:
	FreeTypeGCCounter()
		: m_addcount(0), m_mrucount(0)
	{
	}
	int AddIncrement() { return ++m_addcount; }
	int DecIncrement() { return --m_addcount; }
	int MruIncrement() { return ++m_mrucount; }

	void ResetGCCounter()
	{
		m_mrucount = 0;
		m_addcount = 0;
	}
};

class FreeTypeMruCounter
{
private:
	int m_mrucounter;	//GC用

public:
	FreeTypeMruCounter(int n)
		: m_mrucounter(n)
	{
	}

	//GC用MRUカウンタ
	int GetMruCounter() const { return m_mrucounter; }
	void ResetMruCounter() { m_mrucounter = 0; }
	void SetMruCounter(FreeTypeGCCounter* p) { m_mrucounter = p->MruIncrement(); }
};

//文字幅、(glyph index)、FT_BitmapGlyph(太字、斜体のみ)をキャッシュする
class FreeTypeCharData : public FreeTypeMruCounter
{
private:
	typedef CValArray<FreeTypeCharData**>	CharDataArray;
	CharDataArray		m_arrSelfChar;	//自分自身の保存元(Char)
	FreeTypeCharData**	m_ppSelfGlyph;	//(Glyph)
	UINT				m_glyphindex;	//グリフ番号
	int					m_width;		//文字幅
	int					m_gdiWidth;		//使用GetCharWidth获得的GDI宽度
	FT_Referenced_BitmapGlyph		m_glyph;		//カラー用
	FT_Referenced_BitmapGlyph		m_glyphMono;	//モノクロ用
	int					m_bmpSize;		//ビットマップサイズ
	int					m_bmpMonoSize;	// 〃
	int					m_AAMode;
//	LONG				m_refcounter;	//参照カウンタ

#ifdef _DEBUG
	WCHAR				m_wch;			//UCS2文字
#endif
	NOCOPY(FreeTypeCharData);

	//FT_Bitmap::bufferのサイズを返す
	static inline int FT_Bitmap_CalcSize(FT_BitmapGlyph gl)
	{
		return renderer::freetype::BitmapByteSize(
			gl->bitmap.pitch, gl->bitmap.rows);
	}

public:
	FreeTypeCharData(FreeTypeCharData** ppCh, FreeTypeCharData** ppGl, WCHAR wch, UINT glyphindex, int width, int mru, int gdiWidth, int AAMode);
	~FreeTypeCharData();

#ifdef _DEBUG
	WCHAR GetChar() const { return m_wch; }
#else
	WCHAR GetChar() const { return L'?'; }
#endif
	UINT GetGlyphIndex() const { return m_glyphindex; }
	int GetWidth() const { return m_width; }
	void SetWidth(int width) { m_width = width; }
	void SetGDIWidth(int width) { m_gdiWidth = width; }
	int GetGDIWidth() const {return m_gdiWidth; }
	int GetAAMode() const {return m_AAMode; }
	void AddChar(FreeTypeCharData** ppCh)
	{
		if (ppCh)
			m_arrSelfChar.Add(ppCh);
	}
	FT_Referenced_BitmapGlyph GetGlyph(FT_Render_Mode render_mode) const
	{
		return (render_mode == FT_RENDER_MODE_MONO) ? m_glyphMono : m_glyph;
	}
	void SetGlyph(FT_Render_Mode render_mode, FT_Referenced_BitmapGlyph glyph);

	void Erase()
	{
	}
};
class FreeTypeFontCache : public FreeTypeMruCounter, public FreeTypeGCCounter
{
	typedef map<int, FreeTypeCharData*> GlyphCache;

private:
	bool m_active;
	TEXTMETRIC m_tm;

	//4×65536×2＝512KBぐらいたかが知れてるので固定配列で問題無し
#ifdef _USE_ARRAY
	FreeTypeCharData*	m_chars[FT_MAX_CHARS];
	FreeTypeCharData*	m_glyphs[FT_MAX_CHARS];
#else
	GlyphCache m_GlyphCache;
	static FreeTypeCharData** MissingGlyph()
	{
		static FreeTypeCharData* missingGlyph = nullptr;
		return &missingGlyph;
	}
#endif
	NOCOPY(FreeTypeFontCache);
	void Compact();

	FreeTypeCharData** _GetChar(WCHAR wch)
	{
#ifdef _USE_ARRAY
		return m_chars + wch;
#else
		GlyphCache::iterator it=m_GlyphCache.find(wch);
		return it == m_GlyphCache.end() ? MissingGlyph() : &(it->second);
#endif
	}
	FreeTypeCharData** _GetGlyph(UINT glyph)
	{
#ifdef _USE_ARRAY
		return m_glyphs + glyph;
#else
		GlyphCache::iterator it=m_GlyphCache.find(-static_cast<int>(glyph));
		return it == m_GlyphCache.end() ? MissingGlyph() : &(it->second);
#endif
	}

public:
	FreeTypeFontCache(/*int px, int weight, bool italic, */int mru);
	~FreeTypeFontCache();

	const TEXTMETRIC& GetTextMetric(HDC hdc)
	{
		if (m_tm.tmHeight == 0) {
			::GetTextMetrics(hdc, &m_tm);
		}
		return m_tm;
	}

	FreeTypeCharData* FindChar(WCHAR wch)
	{
		FreeTypeCharData* p = *_GetChar(wch);
		if(p) {
			p->SetMruCounter(this);
		}
		return p;
	}

	FreeTypeCharData* FindGlyphIndex(UINT glyph)
	{
		FreeTypeCharData* p = (glyph & 0xffff0000) ? nullptr : *_GetGlyph(glyph);
		if(p) {
			p->SetMruCounter(this);
		}
		return p;
	}

	bool Activate()
	{
		if (!m_active) {
			m_active = true;
			return true;
		}
		return false;
	}

	void Erase();
	void Deactive() { m_active = false; };
	bool IsActive() const { return m_active; }
	void AddCharData(WCHAR wch, UINT glyphindex, int width, int gdiWidth, FT_Referenced_BitmapGlyph glyph, FT_Render_Mode render_mode, int AAMode);
	void AddGlyphData(UINT glyphindex, int width, int gdiWidth, FT_Referenced_BitmapGlyph glyph, FT_Render_Mode render_mode, int AAMode);
};


// フォント名とFaceID(intを使うことにする)
extern CHashedStringList FontNameCache;
class FreeTypeFontInfo : public FreeTypeMruCounter, public FreeTypeGCCounter
{
private:
	INT_PTR  m_id;
	int  m_weight;
	bool m_italic;
	char m_hashinting;
	int  m_ftWeight;
	int  m_os2Weight;
	int  m_nMaxSizes;
	int	 m_nFontFamily;
	renderer_raii::UniqueFont m_ggoFont;
	std::unique_ptr<TT_OS2> m_OS2Table;
	char m_ebmps[256];
	LONG volatile count;
	CFontSettings m_set;
	StringHashFont m_hash;
	wstring	m_fullname, m_familyname, m_stylename;
	typedef map<renderer::freetype::RasterCacheKey,
		std::unique_ptr<FreeTypeFontCache>> CacheArray;
	CacheArray m_cache;
	//快速链接
	FTC_FaceID face_id_link[CFontLinkInfo::FONTMAX * 2 + 1];
	HFONT ggo_link[CFontLinkInfo::FONTMAX * 2 + 1];
	bool m_linkinited;
	int m_linknum;
	FTC_FaceID m_SimSunID;
	NOCOPY(FreeTypeFontInfo);
	void Compact();
	void Createlink();

public:
	void AddRef() {InterlockedIncrement(&count);};
	void Release() {
		if (InterlockedDecrement(&count)==0)
			delete this;
	}
	TT_OS2* GetOS2Table()
	{
		if (!m_OS2Table)
		{
			TT_OS2 * os2_table = nullptr;
			FT_Face freetype_face;
			CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
			if (FTC_Manager_LookupFace(cache_man, reinterpret_cast<FTC_FaceID>(m_id), &freetype_face))
				return nullptr;
			os2_table = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(freetype_face, ft_sfnt_os2));
			if (!os2_table) return nullptr;
			m_OS2Table = std::make_unique<TT_OS2>();
			memcpy(m_OS2Table.get(), os2_table, sizeof(TT_OS2));
		}
		return m_OS2Table.get();
	}
	BOOL FontHasHinting()
	{
		if (m_hashinting==3)
		{
			FT_Face freetype_face;
			CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
			if (FTC_Manager_LookupFace(cache_man, reinterpret_cast<FTC_FaceID>(m_id), &freetype_face))	//查询ft face
			{
				m_hashinting = false;
				return FALSE;
			}
			FT_ULong length = 0;
			FT_Error err = FT_Load_Sfnt_Table(freetype_face, TTAG_fpgm, 0, nullptr, &length);	//获取fpgm表长度
			if (!err && length>50)		//成功读取表，并且长度较长
				m_hashinting = true;		//字体存在hinting
			else
				m_hashinting = false;
		}
		return m_hashinting;
	}
	wstring GetFullName() {return m_fullname;};
	bool m_isSimSun;
	bool IsPixel;
	FreeTypeFontInfo(int n, LPCTSTR name, int weight, bool italic, int mru, wstring fullname, wstring familyname)
		: m_id(n), m_weight(weight), m_italic(italic), m_OS2Table(), IsPixel(false)
		, FreeTypeMruCounter(mru), m_isSimSun(false), m_ggoFont(), m_linkinited(false), m_linknum(0), m_os2Weight(0)
		, m_SimSunID(0), count(1), m_fullname(fullname), m_familyname(familyname), m_hashinting(3), m_nFontFamily(0)
	{
		//m_set = set;
		memset(m_ebmps, 0xff, sizeof(m_ebmps));

		enum { FTC_MAX_SIZES_DEFAULT = 4 };
		const CGdippSettings* pSettings = CGdippSettings::GetInstance();
		m_nMaxSizes = pSettings->CacheMaxSizes();
		if (!m_nMaxSizes)
			m_nMaxSizes = FTC_MAX_SIZES_DEFAULT;
		//extern BOOL g_EngineCreateFont;
			if (pSettings->FontSubstitutes() < SETTING_FONTSUBSTITUTE_ALL)
				m_ggoFont.reset(CreateFont(10,0,0,0,weight,italic,0,0,DEFAULT_CHARSET,0,FONT_MAGIC_NUMBER,0,0,name));
					//use magic number to create unsubstitud font
			else
				m_ggoFont.reset(CreateFont(10,0,0,0,weight,italic,0,0,DEFAULT_CHARSET,0,0,0,0,name));
			renderer_raii::UniqueDeviceContext hdc(CreateCompatibleDC(nullptr));
			auto selectedFont = renderer_raii::SelectObject(hdc.get(), m_ggoFont.get());
			//获得字体的全称

			int nSize = hdc && selectedFont ? GetOutlineTextMetrics(hdc.get(), 0, nullptr) : 0;
			if (nSize==0)
				m_fullname = L"";
			else
			{
				std::vector<BYTE> metricBuffer(nSize, 0);
				LPOUTLINETEXTMETRIC otm = reinterpret_cast<LPOUTLINETEXTMETRIC>(metricBuffer.data());
				otm->otmSize = nSize;
				if (GetOutlineTextMetrics(hdc.get(), nSize, otm) == 0) {
					m_fullname = L"";
				} else {
					auto metricText = [otm](const void* offset) {
						return reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(otm) + reinterpret_cast<ULONG_PTR>(offset));
					};
					m_fullname = wstring(metricText(otm->otmpFullName));
					TCHAR* localname = metricText(otm->otmpFamilyName);
					m_stylename = wstring(metricText(otm->otmpStyleName));
					m_fullname = MakeUniqueFontName(m_fullname, localname, m_stylename);

					TCHAR buff[LF_FACESIZE+1];
					GetFontLocalName(localname, buff);
					m_nFontFamily = otm->otmTextMetrics.tmPitchAndFamily & 0xF0;	//获取字体家族，家族对应使用什么默认链接字体
					m_familyname = wstring(buff);
					m_set = pSettings->FindIndividual(m_familyname.c_str());
					m_ftWeight = CalcBoldWeight(/*weight*/700);
					m_hash = StringHashFont(name);
					if (m_familyname.size()>0 && m_familyname.c_str()[0]==L'@')	//附加一个@
						m_fullname = L'@'+m_fullname;
				}
			}
			//完成
		face_id_link[0] = nullptr;
		ggo_link[0] = nullptr;
	}
	~FreeTypeFontInfo()
	{
		Erase();
	}

	HFONT GetGGOFont(){return m_ggoFont.get();};
	int CalcNormalWeight() const
	{
		return m_set.GetNormalWeight();
	}
	int CalcBoldWeight(int weight) const
	{
//		return weight - FW_NORMAL) / 8;
//		return ((weight - FW_NORMAL) / 12) + (m_set.GetBoldWeight() << 2);
		weight = weight < FW_BOLD ? 0: /*(weight > FW_BOLD ?*/ 612;
		if (weight <= FW_NORMAL) {
			return 0;
		}
		return ((weight - FW_NORMAL) / 8) + (m_set.GetBoldWeight() << 2);
	}
	int CalcBoldWeight(const LOGFONT& lf) const
	{
		return CalcBoldWeight(lf.lfWeight);
	}
	void CalcItalicSlant(FT_Matrix& matrix) const
	{
		matrix.xx = 1 << 16;
//		matrix.xy = 0x5800;
		matrix.xy = (5 + m_set.GetItalicSlant()) << 12;
		matrix.yx = 0;
		matrix.yy = 1 << 16;
	}

	bool Equals(const StringHashFont& hash, int weight, bool italic) const
	{
		weight = CalcBoldWeight(weight);
		return (m_ftWeight == weight && m_italic == italic && m_hash == hash);
	}
	void UpdateFontSetting()
	{
		m_ftWeight = CalcBoldWeight(700/*m_weight*/);
		//清除字体链接
		face_id_link[0]=nullptr;
		ggo_link[0]=nullptr;
		m_linknum = 0;
		m_linkinited = false;
		m_SimSunID = 0;
	}
	int GetFTLink(FTC_FaceID** llplink)
	{
		CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTLINK);
		if (!*face_id_link)
			Createlink();
		*llplink = face_id_link;
		return m_linknum;
	}
	int GetGGOLink(HFONT** llplink)
	{
		CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTLINK);
		if (!*ggo_link)
			Createlink();
		*llplink = ggo_link;
		return m_linknum;
	}
	FTC_FaceID GetSimSunID() {return m_SimSunID;}

	INT_PTR GetId() const { return m_id; }
	LPCTSTR GetName() const { return m_hash.c_str(); }
	wstring GetStyleName() const { return m_stylename; }
	int GetFontWeight() const { return m_weight; }
	int GetExactBoldWeight() const {return m_set.GetBoldWeight(); }
	int GetFTWeight() const { return m_ftWeight; }
	bool IsItalic() const { return m_italic; }
	const StringHashFont& GetHash() const { return m_hash; }

	const CFontSettings& GetFontSettings() const { return m_set; }
	void SetFontSettings(const CFontSettings& set) { m_set = set;};
	bool operator ==(const FreeTypeFontInfo& x) const { return (m_hash == x.m_hash); }

	FreeTypeFontCache* GetCache(FTC_ScalerRec& scaler, const LOGFONT& lf);
	bool EmbeddedBmpExist(int px);
	void Erase()
	{
		CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTCACHE);
		m_cache.clear();
	}
};

class FreeTypeFontEngine : public FreeTypeGCCounter
{
private:
	typedef map<myfont, FreeTypeFontInfo*> FontMap;
	typedef map<wstring, FreeTypeFontInfo*> FullNameMap;
	typedef vector<FreeTypeFontInfo*> FontList;
	//FontListArray	m_arrFontList;
	int				m_nMaxFaces;
	int				m_nMemUsed;
	bool			m_bAddOnFind;
	std::atomic<std::uint64_t> m_policyGeneration;
	FontMap			m_mfontMap;
	FullNameMap		m_mfullMap;
	FontList		m_mfontList;
	void Compact();

public:
	// Cppcheck 2.20 mistakes the overloaded AddFont/FindFont methods for data members.
	// cppcheck-suppress uninitMemberVar
	FreeTypeFontEngine()
		: m_nMaxFaces(0), m_nMemUsed(0), m_bAddOnFind(false),
		  m_policyGeneration(0)
	{
		enum { FTC_MAX_FACES_DEFAULT = 2 };
		renderer::RendererPolicyRef const policy =
			renderer::CurrentRendererPolicy();
		if (policy)
		{
			m_nMaxFaces = policy->free_type().cacheMaxFaces;
			m_policyGeneration.store(
				policy->generation(), std::memory_order_relaxed);
		}
		if (m_nMaxFaces == 0)
			m_nMaxFaces = FTC_MAX_FACES_DEFAULT;
	}
	~FreeTypeFontEngine()
	{
		TRACE(_T("MaxFaces: %d\n"), m_mfontMap.size());
		TRACE(_T("MemUsed : %d\n"), m_nMemUsed);
		//FontListArray& arr = m_arrFontList;
		FullNameMap::const_iterator iter=m_mfullMap.begin();
		for (;iter!=m_mfullMap.end();++iter)
			iter->second->Release();
	}
	int CalcBoldWeight(int weight) const
	{
		return weight < FW_BOLD ? 0: FW_BOLD;
	}
	FreeTypeFontInfo* AddFont(LPCTSTR lpFaceName, int weight, bool italic, BOOL* bIsFontLoaded = nullptr);
	FreeTypeFontInfo* AddFont(void* lpparams);
	int  GetFontIdByName(LPCTSTR lpFaceName, int weight, bool italic);
//	LPCTSTR GetFontById(int faceid, int& weight, bool& italic);
	FreeTypeFontInfo* FindFont(LPCTSTR lpFaceName, int weight, bool italic, bool AddOnFind = true, BOOL* bIsFontLoaded=nullptr);
	FreeTypeFontInfo* FindFont(int faceid);
	FreeTypeFontInfo* FindFont(void* lpparams);

	bool FontExists(LPCTSTR lpFaceName, int weight, bool italic)
	{
		return !!FindFont(lpFaceName, weight, italic);
	}
	BOOL RemoveFont(LPCWSTR FontName);
	BOOL RemoveFont(FreeTypeFontInfo* fontinfo);
	BOOL RemoveThisFont(FreeTypeFontInfo* fontinfo, LOGFONT* lg);
	//メモリ使用量カウンタ
	void AddMemUsed(int x)
	{
		m_nMemUsed += x;
	}
	void SubMemUsed(int x)
	{
		m_nMemUsed -= x;
		if (m_nMemUsed < 0)
			m_nMemUsed = 0;
	}
	template <class T>
	void AddMemUsedObj(T* /*p*/)
	{
		AddMemUsed(sizeof(T));
	}
	template <class T>
	void SubMemUsedObj(T* /*p*/)
	{
		SubMemUsed(sizeof(T));
	}
	void ReloadAll()
	{
		//重新载入全部字体，即清空所有字体缓存
		COwnedCriticalSectionLock __olock(2);
		CCriticalSectionLock __lock;
		renderer::RendererPolicyRef const policy =
			renderer::CurrentRendererPolicy();
		if (!policy)
			return;

		FullNameMap::const_iterator iter=m_mfullMap.begin();
		for (;iter!=m_mfullMap.end();)
		{
			FreeTypeFontInfo* p =iter->second;
			if (p)
			{
				p->Erase();
				CFontSettings settings =
					policy->font_settings_for(p->GetName());
				if (settings.GetAntiAliasMode() > 2 &&
					policy->raster().harmonyLcd)
					settings.SetAntiAliasMode(2);
				p->SetFontSettings(settings);
				p->UpdateFontSetting();
			}
			++iter;
		}
		m_policyGeneration.store(
			policy->generation(), std::memory_order_release);
	}
	std::uint64_t PolicyGeneration() const noexcept
	{
		return m_policyGeneration.load(std::memory_order_acquire);
	}
};

//GetFontDataのメモリストリーム
class FreeTypeSysFontData
{
private:
	renderer_raii::UniqueDeviceContext m_hdc;
	renderer_raii::UniqueFont m_font;
	renderer_raii::SelectedFont m_selectedFont;
	bool	m_isTTC;
	bool	m_locked;
	renderer_raii::UniqueMappedView m_pMapping;
	renderer_raii::PageLock m_mappingLock;
	DWORD	m_dwSize;
	renderer_raii::UniqueFreeTypeFace m_ftFace;
	wstring m_name;
	FT_StreamRec m_ftStream;

	// Cppcheck 2.20 mistakes the private Init method for a data member.
	// cppcheck-suppress uninitMemberVarPrivate
	FreeTypeSysFontData()
		: m_isTTC(false)
		, m_locked(false)
		, m_dwSize(0)
	{
		ZeroMemory(&m_ftStream, sizeof(FT_StreamRec));
	}

	static unsigned long IoFunc(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count);
	static void CloseFunc(FT_Stream  stream);
	bool OpenFaceByIndex(int index);
	bool Init(LPCTSTR name, int weight, bool italic);

public:
	static FreeTypeSysFontData* CreateInstance(LPCTSTR name, int weight, bool italic);
	~FreeTypeSysFontData() = default;

	FT_Face GetFace()
	{
		return m_ftFace.release();
	}
};
