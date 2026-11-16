import { watch } from 'vue'
import { onResult, panel, send } from './machine'
import type { Tool } from './protocol'

export function sameTool(a: Tool | null | undefined, b: Tool): boolean {
  return a?.number === b.number && a.pocket === b.pocket &&
    a.length === b.length && a.diameter === b.diameter && a.comment === b.comment
}

/** Both acknowledgement and authoritative readback must arrive before saving a dependent shape. */
export function saveTool(tool: Tool, signal: AbortSignal): Promise<string | null> {
  const hello = panel.hello
  const command = { type: 'set_tool' as const, tool }
  return new Promise((resolve) => {
    let accepted = false
    let done = false
    const stops: (() => void)[] = []
    const timer = setTimeout(() => { finish('The tool save was not confirmed. Reopen the editor to check the current values.'); }, 10000)
    function finish(reason: string | null): void {
      if (done) return
      done = true; clearTimeout(timer); for (const stop of stops) stop(); resolve(reason)
    }
    function check(): void {
      if (panel.hello !== hello || panel.link !== 'live') finish('The connection changed. Reopen the editor to check whether the tool was saved.')
      else if (accepted && sameTool(panel.toolTable?.tools.find((row) => row.number === tool.number), tool)) finish(null)
    }
    stops.push(onResult((body, result) => {
      if (body !== command) return
      if (result.outcome !== 'accepted') finish(result.reason || 'The tool could not be saved.')
      else { accepted = true; check() }
    }))
    stops.push(watch([(): unknown => panel.toolTable, (): unknown => panel.hello, (): unknown => panel.link], check, { flush: 'sync' }))
    const abort = (): void => { finish('Editor closed. Reopen it to check whether the tool was saved.') }
    signal.addEventListener('abort', abort, { once: true })
    stops.push(() => { signal.removeEventListener('abort', abort) })
    if (signal.aborted) { abort(); return }
    try { send(command) } catch { finish('The tool save could not be sent.') }
  })
}
