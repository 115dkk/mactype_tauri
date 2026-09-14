# Renderer stack frame budget

## Why the renderer has a frame budget at all

Renderer hooks do not run on threads the renderer created. They run on
whichever application, driver, or engine thread calls the hooked API, and that
thread's stack was sized by its owner. Field evidence collected on 2026-09-11
(Rebel Inc. Escalation, Unity 2022.3.62f3) shows such threads owning 64 KB
stacks. A single stack frame above the budget is therefore not a performance
question. It is a process crash inside someone else's application, reported as
`0xc00000fd` at an address inside `__chkstk`.

Two limits follow from that. First-party translation units hold a 16 KB frame
budget. The pinned FreeType fork's interpreter, hinter, and rasterizer frames
are a documented dependency boundary and are held under a separate, higher
ceiling of 32 KB, because they are upstream code the fork does not restructure.

`docs/directwrite-small-stack-validation.md` records the incident that produced
these limits and the two 64 KB buffers that were removed from application
thread paths as a result.

## How the gate measures a Release image

`scripts/ci/Test-RendererStackFrames.ps1` reads the shipped `MacType.dll` and
`MacType64.dll` together with their `MacType.Core.pdb` and
`MacType64.Core.pdb`, and needs neither a debugger nor a symbol server.

MSVC routes every function whose frame is larger than a page through
`__chkstk`, with the frame size in `EAX`. The prologue that loads that size is
stable across the supported MSVC CRTs, so scanning the image's code sections
for that byte pattern recovers the complete inventory of large frames from a
Release build. The scan tolerates the small displacement the x86 CRT has
changed between versions by treating one pattern element as a wildcard byte.

Attributing a frame to its owner uses the PDB rather than the image. The MSF
7.0 container holds a stream directory addressed through a block map; the DBI
stream holds a 64 byte header, the module table, and the section contribution
table that maps every code range back to the object file that produced it.
Matching a recovered frame address against that table names the translation
unit, which is what decides whether the 16 KB budget or the 32 KB dependency
ceiling applies. Section contribution entries come in two layouts: `Ver60`
(`0xF12EBA2D`) entries are 28 bytes and `V2` (`0xF13151E4`) adds a 4 byte COFF
section index.

A healthy first-party build owns no probed frame of its own. The gate therefore
validates its ownership map against the PDB module table rather than against
the frames the map happens to own, so an empty result cannot pass by accident.

## Where the evidence lands

The gate writes its frame inventory under `artifacts/stack-frames`. A build
whose first-party frames all sit under the budget still publishes the
inventory, so a later change that adds a large frame can be compared against
the run that preceded it.
