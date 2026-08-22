# usage:
#  nmake            release build
#  nmake debug=1    debug build
#  nmake ddk=1      using DDK compiler
#  nmake sse=1      build for SSE capable CPUs (uses with ddk=1)
#  nmake clean      clean all file(s)
#  nmake cleanobj   clean object file(s)

GDI_PREFIX = ..\gdi++
GDI_PREFIX_DDK = ..\gdi++
GDI_PREFIX_SSE = ..\gdi++_sse

TARGET_EXE = $(GDI_PREFIX).exe
TARGET_DLL = $(GDI_PREFIX).dll

EXE_OBJS = run.obj gdiexe.res
DLL_OBJS = hook.obj override.obj settings.obj cache.obj misc.obj expfunc.obj ft.obj fteng.obj ft2vert.obj gdidll.res
LINK_LIBS  = memcpy_amd.lib

EXE_PREFIX = ..\gdiexe
DLL_PREFIX = ..\gdidll

.SUFFIXES: .c .cpp .obj .rc .res

CPPFLAGS   = /nologo /D "WIN32" /Irenderer
LINKFLAGS  = /nologo
LINK_EXE   = /map:$(EXE_PREFIX).map
LINK_DLL   = /map:$(DLL_PREFIX).map

CPPOPT     = /G6 /Gy /QI0f- /QIfdiv-
LINKOPT    = /opt:nowin98 /opt:icf /opt:ref

!ifdef ddk
TARGET_DLL = $(GDI_PREFIX_DDK).dll
DLL_PREFIX = ..\gdidll

!ifdef sse
TARGET_EXE = $(GDI_PREFIX_SSE).exe
TARGET_DLL = $(GDI_PREFIX_SSE).dll
CPPOPT     = $(CPPOPT) /GL /arch:SSE
!else
CPPOPT     = /G7 /Gy /QI0f- /QIfdiv- /GL /arch:SSE2
!endif

LINKOPT    = $(LINKOPT) /ltcg /ignore:4070,4078 bufferoverflowU.lib
LINK_LIBS  = detoured_.lib detours_.lib $(LINK_LIBS)

!else
LINK_LIBS  = detoured.lib detours.lib $(LINK_LIBS)
!endif

!ifdef ftstatic
CPPFLAGS  = $(CPPFLAGS) /D "FREETYPE_STATIC"
FTLIB     = freetypeMT
!else
FTLIB     = freetype
!endif

!ifdef debug
CPPFLAGS  = $(CPPFLAGS) /Od /MDd /FD /GZ /Zi /D "DEBUG" /D "_DEBUG"
LINKFLAGS = $(LINKFLAGS) /incremental:no /debug /machine:I386 /opt:ref /opt:noicf
LINK_EXE  = $(LINK_EXE) /pdb:$(EXE_PREFIX).pdb
#LINK_DLL  = $(LINK_DLL) $(FTLIB)D.lib /pdb:$(DLL_PREFIX).pdb
LINK_DLL  = $(LINK_DLL) $(FTLIB).lib /pdb:$(DLL_PREFIX).pdb
!else
CPPFLAGS  = $(CPPFLAGS) $(CPPOPT) /O2 /MD
LINK_DLL  = $(LINK_DLL) $(LINKOPT) $(FTLIB).lib
LINK_EXE  = $(LINK_EXE) $(LINKOPT)
!endif

all: $(TARGET_EXE) $(TARGET_DLL)

$(TARGET_EXE): $(EXE_OBJS)
	link $(LINKFLAGS) $(LINK_EXE) $(LINK_LIBS) /out:$@ $(EXE_OBJS)

$(TARGET_DLL): $(DLL_OBJS) renderer\expfunc.def
	link /dll $(LINKFLAGS) $(LINK_DLL) $(LINK_LIBS) /def:renderer\expfunc.def /out:$@ $(DLL_OBJS)

.c.obj:
	cl $(CPPFLAGS) /GF /GA /W3 /Fo$@ /c $<

.cpp.obj:
	cl $(CPPFLAGS) /GF /GA /W3 /Fo$@ /c $<

.rc.res:
	rc /l 0x411 $<

{renderer}.c{}.obj:
	cl $(CPPFLAGS) /GF /GA /W3 /Fo$@ /c $<

{renderer}.cpp{}.obj:
	cl $(CPPFLAGS) /GF /GA /W3 /Fo$@ /c $<

{renderer}.rc{}.res:
	rc /l 0x411 /fo$@ $<

clean: cleanobj
	@-erase "$(TARGET_EXE)"
	@-erase "$(TARGET_DLL)"

cleanobj:
	@-erase $(EXE_OBJS)
	@-erase $(DLL_OBJS)
	@-erase ..\gdi???.map
	@-erase vc??.pdb
	@-erase vc??.idb
	@-erase "$(EXE_PREFIX).pdb"
	@-erase "$(DLL_PREFIX).pdb"
	@-erase "$(GDI_PREFIX).exp"
	@-erase "$(GDI_PREFIX).lib"
	@-erase "$(GDI_PREFIX_DDK).exp"
	@-erase "$(GDI_PREFIX_DDK).lib"
	@-erase "$(GDI_PREFIX_SSE).exp"
	@-erase "$(GDI_PREFIX_SSE).lib"

hook.obj:     renderer\hook.cpp renderer\ft.h renderer\hooklist.h renderer\override.h renderer\common.h renderer\array.h renderer\cache.h renderer\settings.h renderer\tlsdata.h renderer\fteng.h
override.obj: renderer\override.cpp renderer\ft.h renderer\hooklist.h renderer\override.h renderer\common.h renderer\array.h renderer\cache.h renderer\settings.h renderer\tlsdata.h renderer\fteng.h renderer\supinfo.h
cache.obj:    renderer\cache.cpp renderer\hooklist.h renderer\override.h renderer\common.h renderer\array.h renderer\cache.h
misc.obj:     renderer\misc.cpp renderer\common.h renderer\array.h
settings.obj: renderer\settings.cpp renderer\common.h renderer\array.h renderer\cache.h renderer\settings.h renderer\strtoken.h renderer\supinfo.h renderer\fteng.h
expfunc.obj:  renderer\expfunc.cpp renderer\common.h renderer\array.h renderer\cache.h renderer\settings.h
ft.obj:       renderer\ft.cpp renderer\ft.h renderer\override.h renderer\common.h renderer\array.h renderer\cache.h renderer\settings.h renderer\fteng.h renderer\ft2vert.h
fteng.obj:    renderer\fteng.cpp renderer\ft.h renderer\override.h renderer\common.h renderer\array.h renderer\cache.h renderer\settings.h renderer\fteng.h
ft2vert.obj:  renderer\ft2vert.c renderer\ft2vert.h
run.obj:      run.cpp renderer\expfunc.cpp renderer\supinfo.h gdiexe.rc
gdiexe.res:   gdiexe.rc renderer\gdidll.rc
