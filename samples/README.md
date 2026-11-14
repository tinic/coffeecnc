# Milling research corpus

Collected from GitHub on 2026-11-14: **22 NC programs, 3 STEP models, 2 native FreeCAD CAM documents, 2 CAMotics projects, a drawing and a setup sheet**, plus supporting data and upstream licenses. 42 upstream files, approximately 5.2 MB.

These are unmodified research fixtures. None has been executed in LinuxCNC, geometrically matched to its STEP file, collision checked, or qualified for a particular machine. Some programs target Mach3/Mach4 or Haas; others are simulator examples. They are useful precisely because real inputs vary in completeness and consistency.

## Start here

| Collection | Files to explore | What it contributes | Qualification |
|---|---|---|---|
| [V3lectronics/Trebuchet](https://github.com/V3lectronics/Trebuchet/tree/0de76c486c1b84ba53aeb93988e54082d0d1331e) | [Left STEP](upstream/trebuchet/CAM/CAM-job-1-left-cut.step), [right STEP](upstream/trebuchet/CAM/CAM-job-2-right-Cut.step), [left CAM](upstream/trebuchet/CAM/CAM-left-job.FCStd), [right CAM](upstream/trebuchet/CAM/CAM-right-job.FCStd), [left NC files](upstream/trebuchet/CAM/gcode-left/), [right NC files](upstream/trebuchet/CAM/gcode-right/), [setup sheet](upstream/trebuchet/CAM/left-job.html) | A fabrication project with related geometry, native operations and 15 separately posted programs | Strong project association; revisions and registration still need verification. Mach3/Mach4 post. |
| [CAMotics](https://github.com/CauldronDevelopmentLLC/CAMotics/tree/e84665f2fa9d1151f03282ac7e01320bc65e015b/examples) | [Heart project](upstream/camotics/examples/heart/heart.camotics), [heart NC](upstream/camotics/examples/heart/heart.ngc), [cutout NC](upstream/camotics/examples/heart/cutout.ngc), [wave project](upstream/camotics/examples/wave/wave.camotics), [wave NC](upstream/camotics/examples/wave/wave.gcode), [wave generator](upstream/camotics/examples/wave/wave.tpl) | Explicit stock and tool descriptions; multiple programs; a surface toolpath without STEP | Simulator examples. Heart project names both NC files; wave project names the generator, not the stored NC. |
| [LinuxCNC](https://github.com/LinuxCNC/linuxcnc/tree/401f78f696769561aca271d8f4f709f601ae5a7d/nc_files) | [Hole circle](upstream/linuxcnc/nc_files/hole-circle.ngc), [two tools with compensation](upstream/linuxcnc/nc_files/gmoccapy_2_tools_with_cutter_radius_compensation.ngc), [grid probing](upstream/linuxcnc/nc_files/gridprobe.ngc) | Actual LinuxCNC dialect: canned cycles, polar increments, tool changes, cutter compensation and probing loops | Language examples requiring machine/tool/probe configuration; no STEP. |
| [Instant Machinist / SM-101](https://github.com/ToukoUrsin/instant-machinist-speedhacks-2026/tree/a85e9de9ac9a92d7ebb8d6f58c1472ff408c54bc) | [STEP](upstream/instant-machinist/instant-machinist/parts/sm-101/part.step), [drawing](upstream/instant-machinist/instant-machinist/parts/sm-101/drawing.pdf), [features](upstream/instant-machinist/instant-machinist/parts/sm-101/features.json), [tools](upstream/instant-machinist/instant-machinist/parts/sm-101/tools.json), [Fusion NC](upstream/instant-machinist/instant-machinist/docs/examples/sm-101-fusion-first-program.nc) | A prismatic plate with pockets/holes and rich associated data | Synthetic AI hackathon example; Haas-targeted output. Not evidence of successful machining. Tool metadata conflicts with this NC revision. |

## Findings from inspecting the files

### Trebuchet: related files are not necessarily a synchronized job package

The upstream README describes a 620 × 300 × 15 mm MDF layout for a competition project. The setup sheet identifies the left CAM document, but its material field says `AusteniticStainlessSteel`. It also reports that the job has not been post-processed. Its May 11 save date precedes the May 15 timestamps in the collected NC headers. Treat the sheet as associated background with stale/conflicting fields, not an authoritative run checklist.

Both native CAM documents select `mach3_mach4`; the NC headers agree. NC uses millimetres, G54, M6 and G43. The left programs call T3; the right programs call T2, despite both tool-controller descriptions referring to a 6 mm end mill. Tool identity belongs to the setup and program revision, not just the visible cutter name.

Static inspection of the FCStd ZIP's `Document.xml` reveals rotated model clones: 90 degrees about Z in both jobs, with additional translation in the right job. The imported source object also has its own placement. A naive STEP overlay at the NC origin would therefore be unjustified. We have not composed and verified all placements or compared resulting solids/toolpaths. “Left” and “right” are upstream job/layout labels; we have not established that these mean a part flip.

The left native document lists 14 operation-group members while the selected `gcode-left` directory has 10 files. There is no established one-file-per-operation correspondence. Some posted paths also contain extremely large arc-center values; retain the original data for preview robustness research rather than normalizing it away.

### CAMotics: a project supplies context, but has its own semantics

The heart project explicitly lists `heart.ngc` and `cutout.ngc`, describes metric stock and defines tools in imperial units. Its NC files contain T words without M6 commands; CoffeeCNC cannot interpret a simulator's tool selection convention as proof that LinuxCNC changed the physical tool.

The wave project references `wave.tpl`. The separately stored `wave.gcode` is useful as a posted-path fixture, but regeneration equivalence was not checked. The current generator's scale is 2 while early posted X steps advance by 4; do not assume the two represent an identical revision. The generator has not been executed.

### LinuxCNC: a program is more than explicit XYZ line segments

The hole-circle example uses G20, G91, G81, polar increments and a repeat count. Grid probing uses named O-word control flow, expressions, G38.2 and probe-result output. The two-tool example uses G41/G43, M6, coolant and an M1 optional stop. Preview and execution state need interpreter semantics and machine context, not a regex that joins coordinate words.

### SM-101: useful geometry, deliberately lower evidence confidence

The source describes an AI demonstration built in October 2026. Its STEP, drawing and feature JSON are associated with a sensor-mount plate. The collected NC has readable operation comments and millimetre/G54 setup, and preselects tools ahead of subsequent changes.

There is a concrete mismatch: `tools.json` describes T4 as a 10 mm spot drill; the NC header describes T4 as a 5 mm flat end mill. The files can exercise revision/conflict handling, but must not be combined into a trusted tool table automatically. Claims of verification and time savings in the upstream README are the author's claims, not findings of this research.

## Provenance and licenses

[manifest.json](manifest.json) records immutable commit IDs, source URLs, local paths, byte counts and SHA-256 hashes for every upstream file. Directory names below `upstream/<collection>/` retain upstream relative paths. Files and line endings are preserved unchanged, including historical author paths in comments.

| Collection | Upstream license material retained |
|---|---|
| Trebuchet | [GPL v3 LICENSE](upstream/trebuchet/LICENSE) |
| CAMotics | [License notice](upstream/camotics/LICENSE), [GPL v2 COPYING](upstream/camotics/COPYING); notice permits later GPL versions |
| LinuxCNC | [GPL v2 COPYING](upstream/linuxcnc/COPYING); retain any per-file notices |
| Instant Machinist | [MIT LICENSE](upstream/instant-machinist/LICENSE) |

These are selected research data, not imported application implementations. Their licenses remain attached independently of CoffeeCNC's eventual project license.

Verify the local copy from the repository root:

```sh
python3 scripts/verify-samples.py
```

The verifier checks bytes, basic container structure and CAMotics file references. It does not interpret G-code or assess machining correctness.

## Remaining collection gaps

We still need a documented metal part actually machined on LinuxCNC, with a consistent STEP/CAM/NC/setup revision and a permissive redistribution statement; a two-sided part with explicit re-fixturing; and examples of tool-change/restart recovery from real operators. The current corpus does not establish those workflows.

Additional leads: [mohanevs/CNC-VMC-Jobs](https://github.com/mohanevs/CNC-VMC-Jobs) has machining programs and STL rather than the requested STEP pairing; no root license was found during inspection, so no files were copied. [OpenATC](https://github.com/sn3ro/OpenATC) has STEP hardware and LinuxCNC routines, but those routines control tool changing and should not be mistaken for machining programs for the STEP parts.
