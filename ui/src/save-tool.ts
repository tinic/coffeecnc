import { watch } from 'vue'
import { t } from './i18n'
import { translateMessage } from './i18n/messages'
import { onResult, panel, send } from './machine'
import type { Tool } from './protocol'

export function sameTool(a: Tool | null | undefined, b: Tool): boolean {
  return a?.number === b.number && a.pocket === b.pocket &&
    a.length === b.length && a.diameter === b.diameter && a.comment === b.comment
}

/** Both acknowledgement and authoritative readback must arrive before saving a dependent shape. */
export function saveTool(tool: Tool, signal: AbortSignal): Promise<(() => string) | null> {
  const hello = panel.hello
  const command = { type: 'set_tool' as const, tool }
  return new Promise((resolve) => {
    let accepted = false
    let done = false
    const stops: (() => void)[] = []
    const timer = setTimeout(() => { finish(() => t('tools.save.notConfirmed')); }, 10000)
    function finish(reason: (() => string) | null): void {
      if (done) return
      done = true; clearTimeout(timer); for (const stop of stops) stop(); resolve(reason)
    }
    function check(): void {
      if (panel.hello !== hello || panel.link !== 'live') finish(() => t('tools.save.connectionChanged'))
      else if (accepted && sameTool(panel.toolTable?.tools.find((row) => row.number === tool.number), tool)) finish(null)
    }
    stops.push(onResult((body, result) => {
      if (body !== command) return
      if (result.outcome !== 'accepted') finish(() => translateMessage(result.reason, result.refusal) || t('tools.save.couldNotSave'))
      else { accepted = true; check() }
    }))
    stops.push(watch([(): unknown => panel.toolTable, (): unknown => panel.hello, (): unknown => panel.link], check, { flush: 'sync' }))
    const abort = (): void => { finish(() => t('tools.save.editorClosed')) }
    signal.addEventListener('abort', abort, { once: true })
    stops.push(() => { signal.removeEventListener('abort', abort) })
    if (signal.aborted) { abort(); return }
    try { send(command) } catch { finish(() => t('tools.save.couldNotSend')) }
  })
}
