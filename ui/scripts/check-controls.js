// Sweep every control of the panel in the browser simulator, in all four themes, and fail on a
// control that has an outline at rest or no fill of its own.
//
//   npm run check:controls [-- --report report.json] [-- --crops dir] [-- --replay report.json]
//
// The sweep starts at each entry state below and clicks every control it finds (buttons, menu
// toggles, tabs, choices) up to three clicks deep, so that it reaches tabs, popovers, dialogs and
// the dialogs they open. Each control is clicked once. In every state it measures each visible
// control with getComputedStyle in each theme:
//   - border: a border side with a width, a style and a visible colour;
//   - outline: an outline at rest (the sweep removes focus first);
//   - edge: any box-shadow on a control;
//   - unfilled: no background of its own, or a background less than 1.1:1 from the surface under
//     it, disabled or not (a soft red, amber or green fill too, on a bar or a band as on a card);
//   - label: less than 4.5:1 on its fill; a disabled label outside 2.8:1 to 4.6:1 (dimmed, not
//     hidden).
// ALLOWED lists the content areas, which are not checked, and the list rows: a row has no fill at
// rest, but it must have no outline, and hovering it must give it a tone of 1.1:1 or more.
//
// --report writes the findings as JSON. --crops writes a crop of each finding in each theme.
// --replay takes the click paths from an earlier report instead of sweeping, to crop the same
// controls after a change. Chrome comes from $CHROME, else the usual install paths.

/* global document, window, getComputedStyle, PointerEvent -- the page functions below run in the browser */
import { spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import { createServer } from 'vite'

const root = fileURLToPath(new URL('..', import.meta.url))
const scratch = fileURLToPath(new URL('../.scratch/check-controls/', import.meta.url))
const themes = ['light', 'dark', 'harbor', 'sandstone']
const maxDepth = 3
const tabs = 4

const entries = [
  '?scene=manual&tab=manual', '?scene=off&tab=manual', '?scene=estop&tab=manual', '?scene=unhomed&tab=manual',
  '?scene=homing&tab=manual', '?scene=shift&tab=manual', '?scene=refmarks&tab=manual', '?scene=rotary&tab=manual',
  '?scene=loaded&tab=program&layout=path', '?scene=loaded&tab=program&layout=text', '?scene=loaded&tab=program&layout=split',
  '?scene=running&tab=program&layout=split', '?scene=paused&tab=program&layout=split', '?scene=toolchange&tab=program',
  '?scene=optstop&tab=program', '?scene=dwell&tab=program', '?scene=error&tab=program', '?scene=limit&tab=manual',
  '?scene=failed-preview&tab=program', '?scene=stale&tab=program', '?scene=manual&tab=tools', '?scene=history&tab=tools',
  '?scene=offsets&tab=tools', '?scene=probe&tab=probe', '?scene=probe&setup=setter', '?scene=probe&setup=measure',
  '?scene=manual&tab=manual&dialog=touchoff', '?scene=manual&tab=manual&dialog=log', '?scene=loaded&tab=program&dialog=open',
  '?scene=loaded&tab=program&dialog=operation', '?scene=loaded&tab=program&dialog=edit', '?scene=manual&tab=tools&dialog=tool',
  '?scene=history&tab=tools&dialog=history', '?scene=manual&tab=manual&dialog=diagnostics', 'view',
]

// Entry states reached by something other than a control: a touched numeric field opens the
// keypad; a tapped line number opens the line bar of the code view.
const specials = [
  ['?scene=manual&tab=manual', '@touch:input[inputmode=none]'],
  ['?scene=manual&tab=tools&dialog=tool', '@touch:[role=dialog] input[inputmode=none]'],
  ['?scene=loaded&tab=program&dialog=operation', '@touch:[role=dialog] input[inputmode=none]'],
  ['?scene=loaded&tab=program&layout=text', '@el:.row .number'],
  ['?scene=loaded&tab=program&layout=split', '@el:.row .number'],
  ['?scene=paused&tab=program&layout=text', '@el:.row .number'],
]

// [selector, reason, kind]. 'skip': not a control, or content; not checked. 'row': a list row,
// checked for outlines and for a hover tone, not for a fill at rest.
const allowed = [
  ['input[type=checkbox], input[type=radio], input[type=range], input[type=file], input[type=color]', 'native input', 'skip'],
  ['.nc, .editor', 'code view content: the lines, not the line bar', 'skip'],
  ['.axis', 'DRO readout: the coordinate is the touch-off target', 'row'],
  ['.list-row', 'list row: a file, a program run, an MDI history line, a disclosure', 'row'],
  ['.tool-number', 'tool table cell: the whole row takes the hover tone', 'skip'],
]
// Controls the sweep does not click: a language choice only repeats every dialog in another language.
const noClick = '.language-option'

const args = process.argv.slice(2)
const option = (name) => { const i = args.indexOf(name); return i < 0 ? null : args[i + 1] }

function chromePath() {
  const candidates = [process.env.CHROME, '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/usr/bin/google-chrome', '/usr/bin/google-chrome-stable', '/usr/bin/chromium', '/usr/bin/chromium-browser']
  const found = candidates.find((path) => path && existsSync(path))
  if (!found) throw new Error('no Chrome found; set CHROME')
  return found
}

// ---- in the page ----------------------------------------------------------------------------------
// These functions run in the page; they must not use anything from this module.

function pageSetup() {
  const style = document.createElement('style')
  style.textContent = '*, *::before, *::after { transition: none !important; animation: none !important; caret-color: transparent !important; }'
  document.head.append(style)
}

function pageClickables(noClick = null) {
  const sig = (el) => {
    const dialog = el.closest('[role=dialog]')?.querySelector('h2, h3')?.textContent?.trim() ?? ''
    const text = (el.getAttribute('aria-label') || el.textContent || '').replace(/\s+/g, ' ').replace(/\d+/g, '#').trim().slice(0, 40)
    const scope = [...el.attributes].map((a) => a.name).find((name) => name.startsWith('data-v-')) ?? ''
    return `${el.tagName.toLowerCase()}.${[...el.classList].sort().join('.')}|${text}|${scope}|${dialog}`
  }
  window.__sig = sig
  return [...document.querySelectorAll('button, summary, [role=button], label.choice, a.button')]
    .filter((el) => el.checkVisibility({ opacityProperty: true, visibilityProperty: true }) && !el.matches(':disabled, [aria-disabled="true"]') && !(noClick && el.matches(noClick)))
    .map(sig)
}

function pageClick(target) {
  const el = [...document.querySelectorAll('button, summary, [role=button], label.choice, a.button')]
    .find((candidate) => candidate.checkVisibility() && !candidate.matches(':disabled') && window.__sig(candidate) === target)
  if (!el) return false
  el.click()
  return true
}

function pageMeasure(themes, allowed) {
  const parse = (value) => {
    let m = /rgba?\(([\d.]+),\s*([\d.]+),\s*([\d.]+)(?:,\s*([\d.]+))?\)/.exec(value)
    if (m) return [Number(m[1]) / 255, Number(m[2]) / 255, Number(m[3]) / 255, m[4] === undefined ? 1 : Number(m[4])]
    m = /color\(srgb ([\d.e-]+) ([\d.e-]+) ([\d.e-]+)(?: \/ ([\d.]+))?\)/.exec(value)
    if (m) return [Number(m[1]), Number(m[2]), Number(m[3]), m[4] === undefined ? 1 : Number(m[4])]
    return null
  }
  const over = (top, under) => [0, 1, 2].map((i) => top[i] * top[3] + under[i] * (1 - top[3])).concat(1)
  const lum = (c) => c.slice(0, 3).map((v) => (v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4))
    .reduce((sum, v, i) => sum + v * [0.2126, 0.7152, 0.0722][i], 0)
  const ratio = (a, b) => { const [x, y] = [lum(a), lum(b)]; return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05) }
  // The colour under an element: its ancestors' backgrounds composited, or null over an image.
  const under = (el) => {
    const layers = []
    for (let node = el.parentElement; node; node = node.parentElement) {
      const cs = getComputedStyle(node)
      if (cs.backgroundImage !== 'none') return null
      const c = parse(cs.backgroundColor)
      if (c && c[3] > 0) { layers.push(c); if (c[3] >= 1) break }
    }
    return layers.reverse().reduce((acc, c) => over(c, acc), [1, 1, 1, 1])
  }
  document.activeElement?.blur?.()
  const rootEl = document.documentElement
  const original = rootEl.dataset.theme
  const kind = (el) => allowed.find(([selector]) => el.matches(selector) || el.closest(selector))?.[2] ?? 'control'
  const controls = [...document.querySelectorAll('button, summary, [role=button], a.button, input, select, textarea, label.choice')]
    .filter((el) => el.checkVisibility({ opacityProperty: true, visibilityProperty: true }) && kind(el) !== 'skip')
  const findings = []
  const rows = []
  for (const theme of themes) {
    rootEl.dataset.theme = theme
    for (const el of controls) {
      const cs = getComputedStyle(el)
      const row = kind(el) === 'row'
      const reasons = []
      for (const side of ['Top', 'Right', 'Bottom', 'Left']) {
        const colour = parse(cs[`border${side}Color`])
        if (parseFloat(cs[`border${side}Width`]) > 0 && cs[`border${side}Style`] !== 'none' && colour && colour[3] > 0) {
          reasons.push(`border-${side.toLowerCase()} ${cs[`border${side}Width`]}`)
        }
      }
      if (cs.outlineStyle !== 'none' && parseFloat(cs.outlineWidth) > 0) reasons.push(`outline ${cs.outlineWidth}`)
      if (cs.boxShadow !== 'none') reasons.push(`box-shadow ${cs.boxShadow}`)
      const paint = parse(cs.backgroundColor)
      const own = paint && [paint[0], paint[1], paint[2], paint[3] * Number(cs.opacity)]
      const base = under(el)
      const disabled = el.matches(':disabled, [aria-disabled="true"]')
      if (row) {
        if (theme === themes[0] && !el.matches(':disabled, [aria-disabled="true"]')) {
          const box = el.getBoundingClientRect()
          rows.push({ sig: window.__sig(el), at: [box.x + box.width / 2, box.y + box.height / 2] })
        }
      } else if (cs.backgroundImage === 'none' && (!own || own[3] === 0)) reasons.push('unfilled')
      else if (base && own) {
        const fill = over(own, base)
        const r = ratio(fill, base)
        if (r < 1.1) reasons.push(`${disabled ? 'disabled ' : ''}fill ${r.toFixed(2)}:1 from the surface`)
        // A label: 4.5:1 on its fill; a disabled label is dimmed to 3:1 to 4.5:1, never hidden.
        const ink = parse(cs.color)
        if (ink && el.textContent.trim() !== '' && !el.matches('input, select, textarea')) {
          const label = ratio(over([ink[0], ink[1], ink[2], ink[3] * Number(cs.opacity)], fill), fill)
          if (disabled && (label < 2.8 || label > 4.6)) reasons.push(`disabled label ${label.toFixed(2)}:1`)
          if (!disabled && label < 4.5) reasons.push(`label ${label.toFixed(2)}:1`)
        }
      }
      if (reasons.length === 0) continue
      const box = el.getBoundingClientRect()
      findings.push({ sig: window.__sig(el), theme, reasons, box: [box.x, box.y, box.width, box.height] })
    }
  }
  if (original === undefined) delete rootEl.dataset.theme
  else rootEl.dataset.theme = original
  return { findings, rows }
}

// A list row under the pointer: no outline, and a hover tone of 1.1:1 or more from the surface.
function pageHover(themes, sig) {
  const parse = (value) => {
    let m = /rgba?\(([\d.]+),\s*([\d.]+),\s*([\d.]+)(?:,\s*([\d.]+))?\)/.exec(value)
    if (m) return [Number(m[1]) / 255, Number(m[2]) / 255, Number(m[3]) / 255, m[4] === undefined ? 1 : Number(m[4])]
    m = /color\(srgb ([\d.e-]+) ([\d.e-]+) ([\d.e-]+)(?: \/ ([\d.]+))?\)/.exec(value)
    if (m) return [Number(m[1]), Number(m[2]), Number(m[3]), m[4] === undefined ? 1 : Number(m[4])]
    return null
  }
  const over = (top, under) => [0, 1, 2].map((i) => top[i] * top[3] + under[i] * (1 - top[3])).concat(1)
  const lum = (c) => c.slice(0, 3).map((v) => (v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4))
    .reduce((sum, v, i) => sum + v * [0.2126, 0.7152, 0.0722][i], 0)
  const ratio = (a, b) => { const [x, y] = [lum(a), lum(b)]; return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05) }
  const el = [...document.querySelectorAll(':hover')].reverse().find((c) => window.__sig(c) === sig)
  if (!el) return []
  const under = () => {
    const stack = []
    for (let node = el.parentElement; node; node = node.parentElement) {
      const cs = getComputedStyle(node)
      if (cs.backgroundImage !== 'none') return null
      const c = parse(cs.backgroundColor)
      if (c && c[3] > 0) { stack.push(c); if (c[3] >= 1) break }
    }
    return stack.reverse().reduce((acc, c) => over(c, acc), [1, 1, 1, 1])
  }
  const rootEl = document.documentElement
  const original = rootEl.dataset.theme
  const findings = []
  for (const theme of themes) {
    rootEl.dataset.theme = theme
    const surface = under()
    if (!surface) continue
    const cs = getComputedStyle(el)
    const reasons = []
    for (const side of ['Top', 'Right', 'Bottom', 'Left']) {
      const colour = parse(cs[`border${side}Color`])
      if (parseFloat(cs[`border${side}Width`]) > 0 && cs[`border${side}Style`] !== 'none' && colour && colour[3] > 0) reasons.push(`hover border-${side.toLowerCase()}`)
    }
    const own = parse(cs.backgroundColor)
    const r = own ? ratio(over(own, surface), surface) : 1
    if (r < 1.1) reasons.push(`hover tone ${r.toFixed(2)}:1`)
    if (reasons.length > 0) findings.push({ sig, theme, reasons })
  }
  if (original === undefined) delete rootEl.dataset.theme
  else rootEl.dataset.theme = original
  return findings
}

// ---- driver ---------------------------------------------------------------------------------------

async function cdp(port) {
  const target = await (await fetch(`http://127.0.0.1:${String(port)}/json/new?about:blank`, { method: 'PUT' })).json()
  const ws = new WebSocket(target.webSocketDebuggerUrl)
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject })
  let id = 0
  const pending = new Map()
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data)
    // A confirm() or alert() would block the page; accept it.
    if (message.method === 'Page.javascriptDialogOpening') ws.send(JSON.stringify({ id: ++id, method: 'Page.handleJavaScriptDialog', params: { accept: true } }))
    if (message.id && pending.has(message.id)) { pending.get(message.id)(message); pending.delete(message.id) }
  }
  // A page that stops answering fails the step instead of stalling the sweep.
  const call = (method, params = {}) => new Promise((resolve, reject) => {
    const i = ++id
    const timer = setTimeout(() => { pending.delete(i); reject(new Error(`${method} timed out`)) }, 20000)
    pending.set(i, (message) => { clearTimeout(timer); resolve(message) })
    ws.send(JSON.stringify({ id: i, method, params }))
  })
  const run = async (fn, ...values) => {
    const reply = await call('Runtime.evaluate', { expression: `(${fn.toString()})(...${JSON.stringify(values)})`, returnByValue: true, awaitPromise: true })
    if (reply.result?.exceptionDetails) throw new Error(reply.result.exceptionDetails.exception?.description ?? 'page error')
    return reply.result?.result?.value
  }
  await call('Page.enable')
  await call('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false })
  return { call, run, close: () => { call('Page.close').catch(() => undefined); setTimeout(() => { ws.close() }, 500) } }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

async function open(page, base, entry, path) {
  await page.call('Page.navigate', { url: `${base}${entry.startsWith('?') ? entry + '&theme=light&lang=en' : entry}` })
  for (let i = 0; i < 50 && !(await page.run(() => document.querySelector('#app')?.childElementCount > 0)); i++) await sleep(100)
  await sleep(700)
  await page.run(pageSetup)
  await page.run(pageClickables, noClick)
  for (const step of path) {
    if (step.startsWith('@touch:')) {
      // A numeric field opens its keypad on a touch release only: a touch pointer press and release.
      const touched = await page.run((selector) => {
        const el = [...document.querySelectorAll(selector)].find((c) => c.checkVisibility())
        if (!el) return false
        const r = el.getBoundingClientRect()
        const at = { clientX: r.x + r.width / 2, clientY: r.y + r.height / 2, pointerId: 7, pointerType: 'touch', isPrimary: true, bubbles: true, cancelable: true }
        el.dispatchEvent(new PointerEvent('pointerdown', at))
        el.dispatchEvent(new PointerEvent('pointerup', at))
        return true
      }, step.slice(7))
      if (!touched) return false
    } else if (step.startsWith('@el:')) {
      const clicked = await page.run((selector) => {
        const el = [...document.querySelectorAll(selector)].find((c) => c.checkVisibility())
        el?.click()
        return el !== undefined
      }, step.slice(4))
      if (!clicked) return false
    } else if (!(await page.run(pageClick, step))) return false
    await sleep(400)
    await page.run(pageClickables, noClick)
  }
  return true
}

async function crop(page, finding, dir, name) {
  for (const theme of themes) {
    await page.run((t) => { document.documentElement.dataset.theme = t; document.activeElement?.blur?.() }, theme)
    const [x, y, w, h] = finding.box
    const clip = { x: Math.max(0, x - 24), y: Math.max(0, y - 24), width: Math.min(1280, w + 48), height: Math.min(800, h + 48), scale: 1 }
    const shot = await page.call('Page.captureScreenshot', { format: 'png', clip })
    if (shot.result) await writeFile(`${dir}/${name}-${theme}.png`, Buffer.from(shot.result.data, 'base64'))
  }
}

async function main() {
  await mkdir(scratch, { recursive: true })
  const server = await createServer({ root, configFile: `${root}vite.config.ts`, logLevel: 'error', server: { port: 5390, strictPort: false, host: '127.0.0.1' } })
  await server.listen()
  const base = server.resolvedUrls.local[0]
  const profile = `${scratch}profile`
  const chrome = spawn(chromePath(), ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--hide-scrollbars', '--no-first-run', ...(process.platform === 'linux' ? ['--no-sandbox'] : []), 'about:blank'], { stdio: 'ignore' })
  let port = null
  for (let i = 0; i < 100 && port === null; i++) {
    await sleep(100)
    if (existsSync(`${profile}/DevToolsActivePort`)) port = Number((await readFile(`${profile}/DevToolsActivePort`, 'utf8')).split('\n')[0])
  }
  const findings = new Map()
  const errors = []
  const cropsDir = option('--crops')
  if (cropsDir) await mkdir(cropsDir, { recursive: true })
  try {
    if (port === null) throw new Error('Chrome did not start')
    const pages = await Promise.all(Array.from({ length: tabs }, () => cdp(port)))
    const replay = option('--replay')
    if (replay) {
      const earlier = JSON.parse(await readFile(replay, 'utf8'))
      let next = 0
      await Promise.all(pages.map(async (page) => {
        while (next < earlier.length) {
          const item = earlier[next++]
          if (!(await open(page, base, item.entry, item.path))) { console.log(`not reached: ${item.sig}`); continue }
          // An item names its control by signature, or by a selector and a label text.
          const box = await page.run((sig, selector, text) => {
            // A changed class changes the signature; the label and the component still find it.
            const loose = (s) => s.split('|').slice(1).join('|')
            const all = [...document.querySelectorAll(selector ?? 'button, summary, [role=button], a.button, input, select, textarea, label.choice')]
              .filter((c) => c.checkVisibility() && (!text || c.textContent.includes(text)))
            const el = selector ? all[0] : all.find((c) => window.__sig(c) === sig) ?? all.find((c) => loose(window.__sig(c)) === loose(sig))
            if (!el) return null
            const r = el.getBoundingClientRect()
            return [r.x, r.y, r.width, r.height]
          }, item.sig ?? null, item.selector ?? null, item.text ?? null)
          if (box && item.hover) await page.call('Input.dispatchMouseEvent', { type: 'mouseMoved', x: box[0] + box[2] / 2, y: box[1] + box[3] / 2 })
          if (box) await crop(page, { box }, cropsDir, item.name)
          else console.log(`not found: ${item.name} ${item.sig ?? item.selector}`)
        }
      }))
    } else {
      const seen = new Set()
      const hovered = new Set()
      const queue = [...entries.map((entry) => ({ entry, path: [] })), ...specials.map(([entry, step]) => ({ entry, path: [step] }))]
      let active = 0
      let done = 0
      await Promise.all(pages.map(async (first, slot) => {
        let page = first
        for (;;) {
          const job = queue.shift()
          if (!job) { if (active === 0) return; await sleep(100); continue }
          active++
          if (++done % 50 === 0) console.error(`${String(done)} states, ${String(queue.length)} queued`)
          try {
            if (!(await open(page, base, job.entry, job.path))) continue
            const measured = await page.run(pageMeasure, themes, allowed)
            const record = async (finding, box) => {
              const known = findings.get(finding.sig)
              if (known) { if (!known.themes.includes(finding.theme)) known.themes.push(finding.theme); return }
              const name = `control-${String(findings.size + 1).padStart(3, '0')}`
              findings.set(finding.sig, { name, sig: finding.sig, reasons: finding.reasons, themes: [finding.theme], entry: job.entry, path: job.path })
              if (cropsDir && box) await crop(page, { box }, cropsDir, name)
            }
            for (const finding of measured.findings) await record(finding, finding.box)
            for (const row of measured.rows) {
              if (hovered.has(row.sig)) continue
              hovered.add(row.sig)
              await page.call('Input.dispatchMouseEvent', { type: 'mouseMoved', x: row.at[0], y: row.at[1] })
              await sleep(150)
              let hover = await page.run(pageHover, themes, row.sig)
              // The first pointer move after a load can land before the hover style applies; look again.
              if (hover.length > 0) { await sleep(300); hover = await page.run(pageHover, themes, row.sig) }
              for (const finding of hover) await record(finding, null)
              await page.call('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 0, y: 0 })
            }
            if (job.path.length < maxDepth) {
              for (const sig of await page.run(pageClickables, noClick)) {
                if (seen.has(sig)) continue
                seen.add(sig)
                queue.push({ entry: job.entry, path: [...job.path, sig] })
              }
            }
          } catch (error) {
            errors.push(`${job.entry} ${job.path.join(' > ')}: ${String(error)}`)
            // A page that stopped answering is replaced; the sweep goes on and reports the step.
            if (String(error).includes('timed out')) { page.close(); page = await cdp(port); pages[slot] = page }
          } finally {
            active--
          }
        }
      }))
      const dialogs = new Set([...seen].map((sig) => sig.split('|')[3]).filter(Boolean))
      console.log(`controls clicked: ${String(seen.size)}; dialogs reached: ${String(dialogs.size)}`)
      if (args.includes('--verbose')) console.log([...dialogs].sort().join('\n'))
    }
    for (const page of pages) page.close()
  } finally {
    chrome.kill()
    await server.close()
    await sleep(300)
    await rm(profile, { recursive: true, force: true })
  }
  for (const error of errors) console.log(`error: ${error}`)
  const list = [...findings.values()]
  const report = option('--report')
  if (report) await writeFile(report, JSON.stringify(list, null, 2))
  for (const f of list) console.log(`${f.name} ${f.sig} [${f.themes.join(',')}] ${f.reasons.join('; ')}\n    at ${f.entry} ${f.path.join(' > ')}`)
  console.log(`findings=${String(list.length)} errors=${String(errors.length)}`)
  process.exitCode = list.length > 0 || errors.length > 0 ? 1 : 0
}

await main()
