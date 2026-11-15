# Preview reference

The toolpath of the loaded program, computed by LinuxCNC's interpreter in a helper process and sent as the protocol's `Preview`. Sources are under `backend/`. Verified against LinuxCNC 2.9.4 in simulation only.

## Parts

| Part | Where | Effect |
|---|---|---|
| `PreviewInputs`, `PreviewEngine` | `src/core/include/coffeecnc/core/preview.hpp` | Everything a path depends on; the pure engine interface |
| `PreviewContext` | `src/app/include/coffeecnc/app/status_publisher.hpp` | `Status.preview_context`: a counter over the exact inputs |
| `PreviewService`, `PreviewWorker` | `src/app/include/coffeecnc/app/preview.hpp` | The preview thread: newest context wins, a running computation is cancelled, files of the result compared every 1 s |
| `PanelComposer` | `src/app/src/panel_feed.cpp` | Decides what is submitted and which `Preview` is sent |
| `HelperPreviewEngine` | `src/linuxcnc/include/coffeecnc/linuxcnc/preview_engine.hpp` | Snapshot, private directory, start of the helper, limits, result |
| `coffeecnc-preview` | `src/linuxcnc/preview/preview_helper.cpp` | `librs274` through `InterpBase` only; the canonical functions keep the preview's own state (position, frame, tool) and record moves, and never issue a machine command |
| Text checks | `src/linuxcnc/src/program_watch.cpp` | Whole-file scan before any read; block check before each execute |
| Result format | `src/linuxcnc/src/preview_wire.cpp` | Parsed as hostile input; only the valid prefix is kept |

## States

| Situation | `Preview` |
|---|---|
| No program | `unavailable`, no error |
| Computing, or a reused result being compared with its files | `pending` |
| Whole path | `ready`; needs a valid result ending `ok` and the helper's own exit status 0 |
| Interpreter error, or the first block the preview does not go past | `failed`, `error.line` the line of the main program, segments up to it |
| Timeout, a bound, files that keep changing (3 tries), a helper that ended badly | `failed`, `error.line` 0 |
| Program running when first seen, or since the link returned | `unavailable`, `error.line` 0, until seen at rest |
| Tool table not accepted yet | `unavailable`, `error.line` 0 |
| Block delete switched during a run or pause | Context goes up; `unavailable`, `error.line` 0 for the rest of that run, also when switched back |
| `[RS274NGC] REMAP`, `NO_DOWNCASE_OWORD` or a random tool changer configured; a UVW plane or cutter compensation active at the start; helper missing | `unavailable`, `error.line` 0 |

`line` is a line of the main program, or 0 when none is known. A move inside a subroutine file carries the line of the call in the main program; an error there names the file and its line in the text. A subroutine defined in the program carries its own line.

## Where a preview ends

| Construct | Reason |
|---|---|
| Any `#` outside an ordinary comment; a call with arguments; a computed O or G word | Parameter values cannot be attributed with the public interpreter API |
| `M100`–`M199` | A user program can change the machine |
| `G38.x`, `M66` | The result depends on the machine |
| `G28`, `G30`, `G92.3` | Positions saved in the parameter file are not live |
| A work system other than the active one | Only the active system's offsets are live |
| `M6` with `[EMCIO] TOOL_CHANGE_AT_G30` | As `G30` |
| NURBS moves (`G5.2`, `G5.3`) | Not computed |
| An inherited motion mode no block can set, at a block that would move by it | |

Refused as a whole, before the interpreter reads anything: a `;py,` line, `(py,)`, `(pyrun,)`, `(pyreload)`, `(log…)`, `(PROBEOPEN)`, `(PROBECLOSE)` anywhere in the program or a subroutine file; an O-word name outside `[a-z0-9_.-]+`. Within the snapshotted files, the scan covers every line, including branches not taken and subroutine definitions never called.

Not modelled: path blending (`G64`; the comparison with the machine ran under `G61`), spindle speed and overrides. `M98`/`M99` and `WIZARD_ROOT` subroutines end in the interpreter's error.

## Inputs and time

| Rule | Detail |
|---|---|
| Coherent start | Inputs are taken only from a reading at rest, with the tool table accepted and the link live, for that program revision |
| During a run | Start state, work systems, G92 and tool table stay as before the run: the preview is the plan from before the run, not a prediction. When the run has ended, the state then found counts: a new context if an input changed, else the earlier result is reused after its files were compared |
| Files | Program and subroutine files are copied; the copy is compared byte for byte with the originals before a result is published and at a 1 s polling period after, call paths resolved again. A change seen raises the context; the period is not a detection deadline |
| Reuse | A context that has a finished result is not computed again; the result is current once the worker has compared its files |
| Not bound | What task opens when the program is run is the original path, not the copy |

## Bounds

| Bound | Value |
|---|---|
| Wall time of the helper (the snapshot precedes it) | 20 s, then the process group is killed |
| Time for the helper to exit after closing its result | 2 s |
| Points | 400000, checked in helper and parent |
| Arc chord error | 0.01 mm |
| Program and subroutine bytes | 16 MiB. Measured on the test host, not a bound: computed in 4.6 s, one comparison of its files 2.7 ms |
| Subroutine files; different called names, missing ones included | 64; 256 |
| Helper standard output and error kept | 64 KiB, never parsed |
| Helper process | Own process group, nice 10, CPU, file size and core limits; address-space limit except in sanitizer builds |

## Launch

| Item | Detail |
|---|---|
| Helper | `COFFEECNC_PREVIEW_HELPER`, else `coffeecnc-preview` beside the executable, else the build tree's |
| Private files | A directory of the process under the system temporary directory, removed at exit; its path must be plain characters (LinuxCNC expands INI paths with `wordexp`) |
| Threads | Snapshot, comparison and helper I/O run on the preview thread only. The tool data memory is read by the publisher thread only |

## Verified

| Check | Result |
|---|---|
| Path against the simulated machine | Lines, G2/G3 in G17/G18/G19, a helix, a G20 section, G54 offset rotated 30 degrees, G43: every machine sample (5 ms) within 0.03 mm of the previewed path |
| First block of axis words only | Preview equals the machine after a fresh start, an MDI `G1`, an aborted G1 program and a finished program |
| `samples/` task-17 A, G54 active | `ready`, 98 segments |
| task-17 B | G55 active: `ready`, 73 segments. G54 active: ends at line 24, the first move in G55 |
| Camcraft `ops_block_face` | Ends at line 5 (`G28`) |
| Panel | The paired STEP stock is drawn over the computed path of task-17 A |
