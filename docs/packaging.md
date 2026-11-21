# Packaging reference

Installation, the Debian package, and running a machine's own configuration in simulation. Sources are under `backend/`. Nothing here has been installed on, or run against, a machine.

## Installed layout

| Path under the prefix | Content | Source |
|---|---|---|
| `bin/coffeecnc` | The DISPLAY program | `src/coffeecnc` |
| `bin/coffeecnc-preview` | The preview helper | `src/linuxcnc/preview` |
| `share/coffeecnc/ui/` | The built panel; an installation without it does not start | `ui/dist` |
| `share/coffeecnc/hal/coffeecnc_postgui.hal` | The post-GUI fragment: watchdog, jog stop and jog inhibit, manual tool change | `share/hal/coffeecnc_postgui.hal`, the file the tests run |
| `share/coffeecnc/coffeecnc.ini.example` | The `[COFFEECNC]` section | `share/coffeecnc.ini.example` |

## Lookup

| Item | Order | Where |
|---|---|---|
| Panel | `--ui`, else `<directory of the executable>/../share/coffeecnc/ui` | `src/coffeecnc/application.cpp` |
| Helper | `COFFEECNC_PREVIEW_HELPER`, else `coffeecnc-preview` beside the executable | `src/coffeecnc/application.cpp` |
| Development fallbacks | The checkout's `ui/dist` and the build tree's helper, only with `COFFEECNC_DEVELOPMENT_PATHS` (default ON; OFF in the `package` preset) | `CMakeLists.txt` |
| Start-up line | `coffeecnc: panel=<dir> preview_helper=<path or none>` on standard error; a missing panel ends start-up, a missing helper is said and the preview is unavailable | |

## Package

| Item | Value |
|---|---|
| Command | `cpack --preset package`, or `cpack -G DEB` in a build directory |
| File | `coffeecnc_<version>_<arch>.deb`, prefix `/usr` |
| Version | `project(... VERSION ...)` in `CMakeLists.txt`, the only place |
| Dependencies | Libraries by `dpkg-shlibdeps`; `linuxcnc-uspace` from the installed version up to the next minor series |
| Maintainer | `COFFEECNC_PACKAGE_MAINTAINER`, default `tinic <tinic@users.noreply.github.com>` |
| Check | `package_deb`: control fields, contents, `dpkg-deb -x` below the build directory; `strings` of both programs shows no path of the checkout, the build directory or the home directory |

## A machine's configuration in simulation

| Item | Rule | Where |
|---|---|---|
| Command | `run.sh -m <machine INI> -s <simulated hardware HAL>` | `tests/sim/run.sh`, `tests/sim/assemble_machine.sh` |
| INI | May differ from the machine's in `DISPLAY`, `PROGRAM_PREFIX` and one added `HALFILE`; anything else fails | |
| HAL files | Lose only the lines matching the `#drop:` expressions of the hardware file | `share/hal/coffeecnc_g0759_machine_hardware.hal` |
| Post-GUI file, shutdown file, tool table | Byte copies; the post-GUI file must hold the shipped fragment's commands in order | |
| Machine files | Not in this repository: `-DCOFFEECNC_MACHINES_DIRECTORY=<checkout of the machine configurations>` | |

## Limits

| Limit | Detail |
|---|---|
| Package installation | Verified by extraction only; `dpkg -i` and removal were not run |
| Machine test | Hardware layer simulated: Mesa card, VFD and encoder lines are dropped; timings are sampled in simulation |
| One application per LinuxCNC session | A second start fails at `loadrt` of the post-GUI file |
