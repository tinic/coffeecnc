// The native check of a block while a touch probe is in the spindle (inspect_probe_code in
// backend/src/app/src/session.cpp), so the simulator refuses the same MDI and program text.
// Only an accepted subset of RS274 passes: O-words, parameters, active comments and unlisted
// M-codes can hide a spindle start.

export const probeLoadedReason = 'A touch probe is loaded'
export const probeCodeUnknown =
  'A touch probe is loaded. CoffeeCNC could not verify that the code leaves the spindle off.'

export interface ProbeCode {
  refusal: string
  changesTool: boolean
  relabelsTool: boolean
}

const spindleCycles = [330, 331, 740, 760, 840, 842, 843, 860, 880]
const ordinaryMCodes = [0, 1, 2, 5, 6, 7, 8, 9, 30, 48, 49, 50, 51, 52, 53, 60, 62, 63, 64, 65, 66, 67, 68, 70, 71]

/** The number after a G or M letter at `at`, as std::from_chars reads it in fixed format. */
function numberAt(code: string, at: number): number | null {
  const match = /^\+?(-?(?:\d+\.?\d*|\.\d+))/.exec(code.slice(at))
  const value = match?.[1] === undefined ? NaN : Number(match[1])
  return Number.isFinite(value) ? value : null
}

export function inspectProbeCode(line: string): ProbeCode {
  const unknown: ProbeCode = { refusal: probeCodeUnknown, changesTool: false, relabelsTool: false }
  let code = ''
  let comment = ''
  let inside = false
  let semicolon = false
  const activeComment = (): boolean =>
    comment.startsWith('py,') || comment.startsWith('pyrun,') || comment.startsWith('pyreload')
  for (const raw of line) {
    if (raw === '\0' || raw === '\r' || raw === '\n') return unknown
    const c = raw >= 'A' && raw <= 'Z' ? raw.toLowerCase() : raw
    if (c === ' ' || c === '\t' || c === '\v' || c === '\f') continue
    if (semicolon) {
      comment += c
    } else if (inside) {
      if (c === '(') return unknown
      if (c !== ')') {
        comment += c
        continue
      }
      if (activeComment()) return unknown
      comment = ''
      inside = false
    } else if (c === ';') {
      // LinuxCNC can interpret the final semicolon comment as an active comment too.
      semicolon = true
    } else if (c === '(') {
      inside = true
    } else {
      code += c
    }
  }
  if (inside || activeComment()) return unknown
  const result: ProbeCode = { refusal: '', changesTool: false, relabelsTool: false }
  if (/[o#[\]<>]/.test(code)) result.refusal = probeCodeUnknown
  for (let at = code.indexOf('g'); at >= 0; at = code.indexOf('g', at + 1)) {
    const value = numberAt(code, at + 1)
    if (value === null || value < 0 || value > 999) {
      result.refusal = probeCodeUnknown
      continue
    }
    // Spindle-synchronized, tapping and boring motion requires a cutting tool.
    if (spindleCycles.includes(Math.round(value * 10)) && result.refusal === '') result.refusal = probeLoadedReason
  }
  for (let at = code.indexOf('m'); at >= 0; at = code.indexOf('m', at + 1)) {
    const value = numberAt(code, at + 1)
    if (value === null || value < 0 || value > 199) {
      result.refusal = probeCodeUnknown
      continue
    }
    const number = Math.round(value)
    if (value === number && number === 61) {
      result.relabelsTool = true
      continue
    }
    if (value === number && number === 6) result.changesTool = true
    if (number === 3 || number === 4 || number === 19) {
      if (result.refusal === '') result.refusal = probeLoadedReason
    } else if (value !== number || !ordinaryMCodes.includes(number)) {
      result.refusal = probeCodeUnknown
    }
  }
  return result
}

/** Why the block must not run with the touch probe loaded, or '' when it may. */
export function probeCodeBlock(line: string): string {
  const block = inspectProbeCode(line)
  return block.relabelsTool ? probeLoadedReason : block.refusal
}

/**
 * Why native refuses `text` as an MDI block (Session::probe_block), or '': an M61 mixed with a
 * tool change or a spindle command always, other code while `probeLoaded`.
 */
export function mdiProbeBlock(text: string, probeLoaded: boolean): string {
  const block = inspectProbeCode(text)
  if (block.relabelsTool && (block.changesTool || block.refusal !== ''))
    return 'Separate M61 from tool changes and spindle commands.'
  return probeLoaded ? probeCodeBlock(text) : ''
}
