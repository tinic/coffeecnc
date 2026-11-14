# Shell reference

The `coffeecnc` executable: threads, message paths, start-up, and what was qualified on WebKitGTK 2.54.0 / GTK 3.24.49 under Xvfb with LinuxCNC 2.9.4 in simulation. Sources are under `backend/`.

## Threads

| Thread | Period | Owns | Where |
|---|---|---|---|
| GTK main | events | Window, web view, page token, document identity; `begin_page`, `end_page`, `submit`; drains the outbox | `src/shell/src/panel_window.cpp` |
| control | 10 ms | HAL pins, heartbeat, dead-man, jog-stop level; no NML call | `Runtime` |
| status | 5 ms | Its NML status channel; leaves each full status with its time in `NmlStatusMailbox` | `src/linuxcnc/src/nml_status.cpp` |
| worker | on work | NML commands and the error channel; counts accepted program and tool-table loads | `Runtime`, `NmlPort` |
| publisher | 50 ms | `NmlPanelSource`, `PanelComposer`, the tool data memory; no NML call, no HAL pin | `PanelFeed` |

## Message paths

| Path | Mechanism | Effect |
|---|---|---|
| Page to machine | A document-start script sends each message as `<identity>:<text>` to the `coffeecnc` handler | A message of a document other than the one shown is dropped; the rest go to `Runtime::submit` |
| Machine to page | `PanelWindow::post(token, messages)` puts data in a mutex-protected outbox; `g_idle_add` carries no data | On the GTK thread the token is compared with the current page; one `coffeecncReceive(object)` per message, guarded by the document identity |
| Page generation | `load-changed` committed ends the old page and begins the new | Messages for an earlier page are dropped |
| Page loss | `load-failed` (not policy, not cancelled), `web-process-terminated`, window destroy end the page | The first two reload the panel after 500 ms; held jogs stop with the page |
| Panel files | `coffeecnc://panel/`: the handler opens the file, WebKit reads the stream off the GTK thread | Missing file and directory 404, traversal and absolute path 403, other host refused, POST 405 |
| Content security policy | `default-src 'self'; script-src 'self' 'wasm-unsafe-eval' 'unsafe-eval'` | `'unsafe-eval'` is needed by the STEP importer's Emscripten glue (`new Function`) |

## Start-up and shutdown

| Step | Order | Failure |
|---|---|---|
| Start | INI, panel directory, display, HAL component, `halcmd -i <ini> -f` post-GUI file, NML port and status reader, tool data memory, runtime, publisher, window | Prints `coffeecnc: <reason>`, exits 1 |
| Stop | Window closed or SIGINT, SIGTERM, SIGHUP: page ends, publisher joins, runtime safe shutdown | Exits 0 |
| Environment | The executable sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` unless the variable is already set; the shell test also passed with it left on | |

## Status fields

| Field | Source |
|---|---|
| `link` | Age of the mailbox reading, judged after all file and tool reads; stale past 100 ms |
| `tool.prepared` | `pocketPrepped` resolved in the loaded table; null while the table shown is not the accepted one |
| `tool_table` | Tool data memory, shown after two identical passes with task idle between; comments from the file at load time |
| `program.revision` | Goes up with every accepted `open_program` and when task names another file |
| `wait` | Tool change (HAL request) before paused before dwell |
| `preview` | `unavailable` |
| `work_offsets` | Active system live; the others `available: false` |

## Qualified

| Item | Result |
|---|---|
| Handler before first script | Present in the first script of every load |
| Module worker, WebAssembly | Worker starts from `/assets/`; the 7.6 MB module is served `application/wasm`, `compileStreaming` succeeds |
| Status during asset load | Longest time a page went without a status: 184 ms (dev); the panel takes the link for silent at 1 s |
| WebGL2 | Works on Mesa software rendering; tool model drawn (5772 of 91078 pixels) |
| STEP import | Real file chooser path: SM-101 1132 triangles, 120 x 80 x 20 mm; cancel, same file again and an invalid file keep the attachment |
| Fonts | DM Sans, IBM Plex Mono 400/500 |
| Tool-table load | File edit without load shows no change; load by another client is shown; six rounds of reload-then-command show no intermediate table |

## Not qualified

| Item | Reason |
|---|---|
| STEP overlay rendering | The panel hides the part without a toolpath preview |
| Import cancelled mid-flight, named views, orbit, dark theme, lost context, large models | Not tested |
| GUI under ThreadSanitizer | Uninstrumented GLib, GTK, WebKit; see `backend/README.md` |
| Leaks with the accessibility bridge on | Tests run with `NO_AT_BRIDGE=1` |
| A reload that stayed at "Waiting for connection" | Seen once under ThreadSanitizer at 3186b99; not reproduced after panel files were served as streams; cause not established |
