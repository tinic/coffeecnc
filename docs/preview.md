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
| `NO_DOWNCASE_OWORD` or a random tool changer configured; a UVW plane or cutter compensation active at the start; helper missing | `unavailable`, `error.line` 0 |

`line` is a line of the main program, or 0 when none is known. A move inside a subroutine file carries the line of the call in the main program; an error there names the file and its line in the text. A subroutine defined in the program carries its own line.

## Where a preview ends

| Construct | Reason |
|---|---|
| A parameter that task keeps between programs (`#1`–`#5000` not in the parameter file, `#5061`–`#5070`, `#5399`), read before the program sets it | Its value in task is not known. Found by a second computation with these parameters seeded: the preview ends at the first line where the two paths differ |
| A global named parameter (`#<_name>`, also `#<_hal[...]>`, `#<_ini[...]>`) that the program reads and does not define | Task can have it from an earlier program or MDI block. An undefined local one is the program's error, as in task |
| `#<_task>`; a computed O or G word | 1 in task and 0 in the preview; which code runs is not known before it does |
| `M100`–`M199` | A user program can change the machine |
| A code `[RS274NGC] REMAP` names | What the remap does is not known |
| `G38.x`, `M66` | The result depends on the machine |
| `G92.3` | The saved G92 offset is replaced by the live one in the preview's copy |
| A work system, or `G28`/`G30` (also `M6` with `[EMCIO] TOOL_CHANGE_AT_G30`), whose values the parameter file does not hold | Not known |
| NURBS moves (`G5.2`, `G5.3`) | Not computed |
| An inherited motion mode no block can set, at a block that would move by it | |

## Parameters and saved positions

| Rule | Detail |
|---|---|
| Source | The machine's parameter file (`[RS274NGC] PARAMETER_FILE`) is read with the snapshot. The live active work system, its rotation and G92 are written over the copy. The other work systems, the G28 and G30 positions and every other saved parameter are the file's |
| Freshness | Task writes the file whenever it synchronises its interpreter: on a switch to MDI or Auto, before each MDI block, at a queue buster, after a program ends or is aborted, on E-stop and machine off. A change made by the last MDI block (`G10 L2`, `G28.1`) is in the file only at the next of these |
| Run | Run switches task to Auto, which writes the file, and then compares it again: a preview made before the file had the change refuses that Run ("The program or tool data changed. Review the new program check."), and a new preview follows. Verified in `linuxcnc_cam_programs_g0759` (`cam_stale_parameter_file_refuses_run`) |
| Watch | The file is compared by its values, without G92, the selected system and the active system's offsets (the live state's) |
| Tool length at the start | `G43` (the table's length of the tool in the spindle) or `G43.1` (another length) is put in place in the preview's interpreter too, so `G53`, `G28` and `G30` take off the same length as task |
| Parameters | Interpreted. A program that uses `#` is computed a second time with the parameters task keeps between programs seeded with values of their own; the result stands when both are the same, else a third computation ends the preview at the first line where they differ |

## Gates when a preview ends early

| Action | Preview `ready` | Preview `failed` (ends at a line, with a partial check) | Preview `unavailable` |
|---|---|---|---|
| Run | Starts; review only for warnings | Starts after the review of the partial check | Starts after the review |
| Program check | Whole program | The path up to the end, with "CoffeeCNC could only check part of this program." | The reason only |
| Run to line | Any motion line of the path | A motion line of the path before the end; refused after it | Refused |
| Restart from line | When the restart point can be reconstructed | Refused, with the preview's reason | Refused |

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
| task-17 B | G55 active: `ready`, 73 segments. G54 active without a parameter file: ends at line 24, the first move in G55 |
| Camcraft `ops_block_face` | Without a parameter file: ends at line 5 (`G28`) |
| `backend/tests/cam`: Fusion 360, Mastercam, SheetCam and Estlcam style posts, parameters and subroutines, three vises in G54/G55/G56 | `ready` in `preview_engine` and on the simulated G0759 (`linuxcnc_cam_programs_g0759`); a program reading `#3000` before it sets it ends at that line |
| G0759, Fusion-style program with `G28 G91 Z0` | Preview of the G28 at machine Z 0, as the machine went; Run to the line after it; Restart from a line after it ran the rest and ended at machine Z 0 |
| Panel | The paired STEP stock is drawn over the computed path of task-17 A |
