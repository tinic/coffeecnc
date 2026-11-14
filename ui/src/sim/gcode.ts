// The simulator's G-code interpreter. One implementation serves both the preview (a dry run over
// a copy of the modal state) and execution (line by line, as the simulated task reaches each line).
//
// Supported subset:
//   G0 G1 G2 G3 (I J or R, XY plane, helical Z allowed), G4 P, G17, G20 G21, G53,
//   G54 G55 G56 G57 G58 G59 G59.1 G59.2 G59.3, G10 L2 / L20 / L10, G43 [H] G49, G90 G91, G92, G94,
//   F S T, M0 M1 M2 M30 M3 M4 M5 M6 M7 M8 M9, N line numbers, ( ) and ; comments, (MSG, text),
//   block delete with a leading /.
// Everything else is an error that names the line and the word. Nothing is skipped silently.

import type { LengthUnit, Preview, PreviewSegment, SpindleDirection, Tool, Vec3 } from '../protocol'

export type AxisIndex = 0 | 1 | 2
export const axisIndices: readonly AxisIndex[] = [0, 1, 2]
const axisWords: readonly string[] = ['X', 'Y', 'Z']
const mmPerInch = 25.4

/** Everything the interpreter remembers between blocks. Lengths in mm, feed in mm/min. */
export interface Modal {
  /** Where the interpreter is, machine coordinates. */
  pos: Vec3
  /** Active work coordinate system, 1 (G54) to 9 (G59.3). */
  wcs: number
  offsets: Vec3[]
  rotations: number[]
  g92: Vec3
  toolOffset: Vec3
  units: LengthUnit
  absolute: boolean
  motion: 0 | 1 | 2 | 3
  feed: number
  speed: number
  spindle: SpindleDirection
  flood: boolean
  mist: boolean
  /** Tool the interpreter considers loaded. */
  tool: number
  prepared: number | null
  tools: Tool[]
}

export type Op =
  | {
      kind: 'move'
      line: number
      motion: 'rapid' | 'feed'
      shape: 'linear' | 'arc'
      tool: number
      feed: number
      /** Polyline in machine coordinates, starting at the present position. */
      points: Vec3[]
      /** Tool offset in effect during the move. */
      toolOffset: Vec3
    }
  | { kind: 'dwell'; seconds: number }
  | { kind: 'pause'; optional: boolean }
  | { kind: 'tool_change'; tool: number }
  | { kind: 'message'; text: string }
  | { kind: 'end' }

export interface LineResult {
  ops: Op[]
  /** Nothing but white space, comments or a deleted block. */
  blank: boolean
}

export class InterpError extends Error {
  constructor(
    readonly line: number,
    readonly detail: string,
  ) {
    super(`line ${String(line)}: ${detail}`)
  }
}

export const wcsNames: readonly string[] = ['G54', 'G55', 'G56', 'G57', 'G58', 'G59', 'G59.1', 'G59.2', 'G59.3']

export function newModal(tools: Tool[]): Modal {
  return {
    pos: [0, 0, 0],
    wcs: 1,
    offsets: Array.from({ length: 9 }, (): Vec3 => [0, 0, 0]),
    rotations: Array.from({ length: 9 }, () => 0),
    g92: [0, 0, 0],
    toolOffset: [0, 0, 0],
    units: 'mm',
    absolute: true,
    motion: 1,
    feed: 0,
    speed: 0,
    spindle: 'stopped',
    flood: false,
    mist: false,
    tool: 0,
    prepared: null,
    tools,
  }
}

export function cloneModal(m: Modal): Modal {
  return {
    ...m,
    pos: [...m.pos],
    offsets: m.offsets.map((o): Vec3 => [...o]),
    rotations: [...m.rotations],
    g92: [...m.g92],
    toolOffset: [...m.toolOffset],
    tools: m.tools.map((t) => ({ ...t })),
  }
}

export function workOffset(m: Modal, wcs: number = m.wcs): Vec3 {
  return m.offsets[wcs - 1] ?? [0, 0, 0]
}

function rotation(m: Modal): number {
  return ((m.rotations[m.wcs - 1] ?? 0) * Math.PI) / 180
}

/**
 * Program (work) coordinates to machine coordinates, in LinuxCNC's order (emccanon.cc,
 * rotate_and_offset_pos): add G92, rotate XY, add G5x, then add the tool offset. The tool offset
 * is outside the rotation.
 */
export function toMachine(m: Modal, p: Vec3): Vec3 {
  const g5x = workOffset(m)
  const a = rotation(m)
  const x = p[0] + m.g92[0]
  const y = p[1] + m.g92[1]
  return [
    x * Math.cos(a) - y * Math.sin(a) + g5x[0] + m.toolOffset[0],
    x * Math.sin(a) + y * Math.cos(a) + g5x[1] + m.toolOffset[1],
    p[2] + m.g92[2] + g5x[2] + m.toolOffset[2],
  ]
}

/** Machine coordinates to program (work) coordinates: the inverse of `toMachine`. */
export function toWork(m: Modal, p: Vec3): Vec3 {
  const g5x = workOffset(m)
  const a = rotation(m)
  const x = p[0] - m.toolOffset[0] - g5x[0]
  const y = p[1] - m.toolOffset[1] - g5x[1]
  return [
    x * Math.cos(a) + y * Math.sin(a) - m.g92[0],
    -x * Math.sin(a) + y * Math.cos(a) - m.g92[1],
    p[2] - m.toolOffset[2] - g5x[2] - m.g92[2],
  ]
}

function gName(code: number): string {
  return code % 10 === 0 ? `G${String(code / 10)}` : `G${(code / 10).toFixed(1)}`
}

export function activeCodes(m: Modal): string[] {
  const codes = [
    `G${String(m.motion)}`,
    'G17',
    m.units === 'inch' ? 'G20' : 'G21',
    m.toolOffset.some((v) => v !== 0) ? 'G43' : 'G49',
    wcsNames[m.wcs - 1] ?? 'G54',
    m.absolute ? 'G90' : 'G91',
    'G94',
    m.spindle === 'forward' ? 'M3' : m.spindle === 'reverse' ? 'M4' : 'M5',
  ]
  if (m.mist) {
    codes.push('M7')
  }
  if (m.flood) {
    codes.push('M8')
  }
  if (!m.mist && !m.flood) {
    codes.push('M9')
  }
  return codes
}

// ---- parsing ----------------------------------------------------------------------------------

interface Block {
  /** G codes times ten, so G59.1 is 591. */
  g: number[]
  m: number[]
  words: Map<string, number>
  comments: string[]
  blank: boolean
}

const numberPattern = /^[+-]?(?:\d+\.?\d*|\.\d+)/
const wordLetters = 'FGHIJKLMNPRSTXYZ'

function parseBlock(raw: string, line: number, blockDelete: boolean): Block {
  const block: Block = { g: [], m: [], words: new Map(), comments: [], blank: true }
  let i = 0
  let first = true
  while (i < raw.length) {
    const c = raw.charAt(i)
    if (c === ' ' || c === '\t' || c === '\r') {
      i += 1
      continue
    }
    if (c === ';') {
      break
    }
    if (c === '(') {
      const close = raw.indexOf(')', i)
      if (close < 0) {
        throw new InterpError(line, 'comment is not closed with )')
      }
      block.comments.push(raw.slice(i + 1, close))
      i = close + 1
      continue
    }
    if (first && c === '/') {
      if (blockDelete) {
        return { g: [], m: [], words: new Map(), comments: [], blank: true }
      }
      first = false
      i += 1
      continue
    }
    if (first && c === '%') {
      i += 1
      continue
    }
    first = false
    const letter = c.toUpperCase()
    if (letter === '#') {
      throw new InterpError(line, 'parameters (#) are not supported by the simulator')
    }
    if (letter === '[') {
      throw new InterpError(line, 'expressions ([ ]) are not supported by the simulator')
    }
    if (letter === '^' || letter === '@') {
      throw new InterpError(line, `polar coordinates (${letter}) are not supported by the simulator`)
    }
    if (letter === 'O') {
      throw new InterpError(line, 'O words (subroutines and loops) are not supported by the simulator')
    }
    if (letter < 'A' || letter > 'Z') {
      throw new InterpError(line, `unexpected character "${c}"`)
    }
    if (!wordLetters.includes(letter)) {
      throw new InterpError(line, `${letter} word is not supported by the simulator`)
    }
    const rest = raw.slice(i + 1).trimStart()
    const lead = rest.charAt(0)
    if (lead === '#') {
      throw new InterpError(line, 'parameters (#) are not supported by the simulator')
    }
    if (lead === '[') {
      throw new InterpError(line, 'expressions ([ ]) are not supported by the simulator')
    }
    const match = numberPattern.exec(rest)
    if (match === null) {
      throw new InterpError(line, `${letter} word has no number`)
    }
    const value = Number(match[0])
    i = raw.length - rest.length + match[0].length
    block.blank = false
    if (letter === 'G') {
      block.g.push(Math.round(value * 10))
    } else if (letter === 'M') {
      block.m.push(value)
    } else if (letter !== 'N') {
      if (block.words.has(letter)) {
        throw new InterpError(line, `more than one ${letter} word`)
      }
      block.words.set(letter, value)
    }
  }
  return block
}

// ---- execution of one block ---------------------------------------------------------------------

const gGroups: Readonly<Record<number, string>> = {
  0: 'motion',
  10: 'motion',
  20: 'motion',
  30: 'motion',
  40: 'non-modal',
  100: 'non-modal',
  920: 'non-modal',
  530: 'machine coordinates',
  170: 'plane',
  200: 'units',
  210: 'units',
  430: 'tool length',
  490: 'tool length',
  540: 'coordinate system',
  550: 'coordinate system',
  560: 'coordinate system',
  570: 'coordinate system',
  580: 'coordinate system',
  590: 'coordinate system',
  591: 'coordinate system',
  592: 'coordinate system',
  593: 'coordinate system',
  900: 'distance mode',
  910: 'distance mode',
  940: 'feed mode',
}

const mGroups: Readonly<Record<number, string>> = {
  0: 'stopping',
  1: 'stopping',
  2: 'stopping',
  30: 'stopping',
  3: 'spindle',
  4: 'spindle',
  5: 'spindle',
  6: 'tool change',
  7: 'mist',
  8: 'flood',
  9: 'coolant off',
}

const wcsCodes: readonly number[] = [540, 550, 560, 570, 580, 590, 591, 592, 593]

function checkGroups(block: Block, line: number): void {
  const seen = new Map<string, string>()
  const claim = (group: string, name: string): void => {
    const other = seen.get(group)
    if (other !== undefined) {
      throw new InterpError(line, `${other} and ${name} cannot be used on the same line`)
    }
    seen.set(group, name)
  }
  for (const code of block.g) {
    const group = gGroups[code]
    if (group === undefined) {
      throw new InterpError(line, `${gName(code)} is not supported by the simulator`)
    }
    claim(group, gName(code))
  }
  for (const code of block.m) {
    const name = `M${String(code)}`
    const group = mGroups[code]
    if (group === undefined) {
      throw new InterpError(line, `${name} is not supported by the simulator`)
    }
    claim(group, name)
  }
  if (block.m.includes(9) && (block.m.includes(7) || block.m.includes(8))) {
    throw new InterpError(line, 'M9 cannot be used with M7 or M8 on the same line')
  }
}

function findTool(m: Modal, number: number): Tool | undefined {
  return m.tools.find((t) => t.number === number)
}

function arcPoints(start: Vec3, end: Vec3, centre: [number, number], clockwise: boolean): Vec3[] {
  const radius = Math.hypot(start[0] - centre[0], start[1] - centre[1])
  const a0 = Math.atan2(start[1] - centre[1], start[0] - centre[0])
  const a1 = Math.atan2(end[1] - centre[1], end[0] - centre[0])
  let sweep = a1 - a0
  if (clockwise) {
    while (sweep >= -1e-9) {
      sweep -= 2 * Math.PI
    }
  } else {
    while (sweep <= 1e-9) {
      sweep += 2 * Math.PI
    }
  }
  // Chords stay within 0.01 mm of the true arc.
  const step = 2 * Math.acos(Math.max(0, 1 - 0.01 / Math.max(radius, 0.02)))
  const count = Math.min(720, Math.max(4, Math.ceil(Math.abs(sweep) / step)))
  const points: Vec3[] = [start]
  for (let n = 1; n < count; n += 1) {
    const t = n / count
    const a = a0 + sweep * t
    points.push([
      centre[0] + radius * Math.cos(a),
      centre[1] + radius * Math.sin(a),
      start[2] + (end[2] - start[2]) * t,
    ])
  }
  points.push(end)
  return points
}

function arcCentre(
  block: Block,
  used: Set<string>,
  line: number,
  start: Vec3,
  end: Vec3,
  clockwise: boolean,
  scale: number,
): [number, number] {
  const name = clockwise ? 'G2' : 'G3'
  const r = block.words.get('R')
  const i = block.words.get('I')
  const j = block.words.get('J')
  if (block.words.has('K')) {
    throw new InterpError(line, `K word is not used by ${name} in the XY plane (G17)`)
  }
  if (r !== undefined) {
    if (i !== undefined || j !== undefined) {
      throw new InterpError(line, `${name} has both an R word and I or J`)
    }
    used.add('R')
    const radius = r * scale
    const dx = end[0] - start[0]
    const dy = end[1] - start[1]
    const chord = Math.hypot(dx, dy)
    if (chord < 1e-9) {
      throw new InterpError(line, `${name} with R: current point is the same as the end point of the arc`)
    }
    const h2 = radius * radius - (chord * chord) / 4
    if (h2 < -1e-6) {
      throw new InterpError(line, `${name}: arc radius is too small to reach the end point`)
    }
    const h = Math.sqrt(Math.max(0, h2))
    const side = (clockwise ? -1 : 1) * (radius < 0 ? -1 : 1)
    return [
      (start[0] + end[0]) / 2 + (side * h * -dy) / chord,
      (start[1] + end[1]) / 2 + (side * h * dx) / chord,
    ]
  }
  if (i === undefined && j === undefined) {
    throw new InterpError(line, `${name} needs I and J, or R`)
  }
  used.add('I').add('J')
  const centre: [number, number] = [start[0] + (i ?? 0) * scale, start[1] + (j ?? 0) * scale]
  const r0 = Math.hypot(start[0] - centre[0], start[1] - centre[1])
  const r1 = Math.hypot(end[0] - centre[0], end[1] - centre[1])
  if (r0 < 1e-9) {
    throw new InterpError(line, `${name}: zero-radius arc`)
  }
  const difference = Math.abs(r0 - r1)
  if (difference > 0.0127 && difference > 0.001 * r0) {
    throw new InterpError(
      line,
      `${name}: radius to end of arc differs from radius to start (start ${r0.toFixed(3)}, end ${r1.toFixed(3)})`,
    )
  }
  return centre
}

/**
 * Interprets one line of G-code, changing `m` and returning what the machine has to do for it.
 * Throws `InterpError`; `m` may then be partly changed, as with LinuxCNC's interpreter.
 */
export function interpretLine(m: Modal, text: string, line: number, blockDelete: boolean): LineResult {
  const block = parseBlock(text, line, blockDelete)
  const ops: Op[] = []
  for (const comment of block.comments) {
    const message = /^\s*msg,(.*)$/i.exec(comment)
    if (message !== null) {
      ops.push({ kind: 'message', text: (message[1] ?? '').trim() })
    }
  }
  if (block.blank) {
    return { ops, blank: ops.length === 0 }
  }
  checkGroups(block, line)

  const used = new Set<string>()
  const has = (code: number): boolean => block.g.includes(code)
  const word = (letter: string): number | undefined => {
    const value = block.words.get(letter)
    if (value !== undefined) {
      used.add(letter)
    }
    return value
  }

  // Units first for the words on this line, as LinuxCNC does.
  if (has(200)) {
    m.units = 'inch'
  } else if (has(210)) {
    m.units = 'mm'
  }
  const scale = m.units === 'inch' ? mmPerInch : 1

  const feed = word('F')
  if (feed !== undefined) {
    if (feed < 0) {
      throw new InterpError(line, 'F word: negative feed rate')
    }
    m.feed = feed * scale
  }
  const speed = word('S')
  if (speed !== undefined) {
    if (speed < 0) {
      throw new InterpError(line, 'S word: negative spindle speed')
    }
    m.speed = speed
  }
  const toolWord = word('T')
  if (toolWord !== undefined) {
    if (!Number.isInteger(toolWord) || toolWord < 0) {
      throw new InterpError(line, `T word: ${String(toolWord)} is not a tool number`)
    }
    if (toolWord !== 0 && findTool(m, toolWord) === undefined) {
      throw new InterpError(line, `requested tool ${String(toolWord)} not found in the tool table`)
    }
    m.prepared = toolWord
  }
  if (block.m.includes(6)) {
    if (m.prepared === null) {
      throw new InterpError(line, 'M6 needs a tool selected with a T word')
    }
    m.spindle = 'stopped'
    m.tool = m.prepared
    m.prepared = null
    ops.push({ kind: 'tool_change', tool: m.tool })
  }
  if (block.m.includes(3)) {
    m.spindle = 'forward'
  } else if (block.m.includes(4)) {
    m.spindle = 'reverse'
  } else if (block.m.includes(5)) {
    m.spindle = 'stopped'
  }
  if (block.m.includes(7)) {
    m.mist = true
  }
  if (block.m.includes(8)) {
    m.flood = true
  }
  if (block.m.includes(9)) {
    m.mist = false
    m.flood = false
  }
  if (has(40)) {
    const seconds = word('P')
    if (seconds === undefined) {
      throw new InterpError(line, 'G4 needs a P word (seconds)')
    }
    if (seconds < 0) {
      throw new InterpError(line, 'G4: negative dwell time')
    }
    ops.push({ kind: 'dwell', seconds })
  }
  if (has(430)) {
    const number = word('H') ?? m.tool
    const tool = findTool(m, number)
    if (number !== 0 && tool === undefined) {
      throw new InterpError(line, `G43: tool ${String(number)} not found in the tool table`)
    }
    m.toolOffset = [0, 0, tool?.length ?? 0]
  } else if (has(490)) {
    m.toolOffset = [0, 0, 0]
  }
  for (const [index, code] of wcsCodes.entries()) {
    if (has(code)) {
      m.wcs = index + 1
    }
  }
  if (has(900)) {
    m.absolute = true
  } else if (has(910)) {
    m.absolute = false
  }

  const present = toWork(m, m.pos)
  let axesTaken = false
  if (has(100)) {
    axesTaken = true
    g10(m, block, word, line, scale, present)
  } else if (has(920)) {
    axesTaken = true
    let any = false
    for (const axis of axisIndices) {
      const value = word(axisWords[axis] ?? '')
      if (value !== undefined) {
        any = true
        m.g92[axis] += present[axis] - value * scale
      }
    }
    if (!any) {
      throw new InterpError(line, 'G92 needs at least one axis word')
    }
  }

  const explicit = ([0, 10, 20, 30] as const).find((code) => has(code))
  if (explicit !== undefined && axesTaken) {
    throw new InterpError(
      line,
      `${gName(explicit)} cannot be used on the same line as ${has(100) ? 'G10' : 'G92'}: both use axis words`,
    )
  }
  if (explicit !== undefined) {
    m.motion = (explicit / 10) as 0 | 1 | 2 | 3
  }
  const axisGiven = axisWords.some((letter) => block.words.has(letter))
  if (!axesTaken && (axisGiven || (explicit !== undefined && explicit >= 20))) {
    const move = motion(m, block, word, used, line, scale, has(530))
    if (move !== null) {
      ops.push(move)
    }
  } else if (has(530)) {
    throw new InterpError(line, 'G53 needs a G0 or G1 move on the same line')
  }

  for (const letter of block.words.keys()) {
    if (!used.has(letter)) {
      throw new InterpError(line, `${letter} word is not used by any code on this line`)
    }
  }

  if (block.m.includes(0)) {
    ops.push({ kind: 'pause', optional: false })
  } else if (block.m.includes(1)) {
    ops.push({ kind: 'pause', optional: true })
  } else if (block.m.includes(2) || block.m.includes(30)) {
    // Program end resets what LinuxCNC resets: G54, G90, G1, spindle and coolant off.
    m.wcs = 1
    m.absolute = true
    m.motion = 1
    m.spindle = 'stopped'
    m.flood = false
    m.mist = false
    ops.push({ kind: 'end' })
  }
  return { ops, blank: false }
}

function g10(
  m: Modal,
  block: Block,
  word: (letter: string) => number | undefined,
  line: number,
  scale: number,
  present: Vec3,
): void {
  const l = word('L')
  const p = word('P')
  if (l === undefined) {
    throw new InterpError(line, 'G10 needs an L word')
  }
  if (l !== 2 && l !== 20 && l !== 10) {
    throw new InterpError(line, `G10 L${String(l)} is not supported by the simulator`)
  }
  if (p === undefined || !Number.isInteger(p)) {
    throw new InterpError(line, `G10 L${String(l)} needs a P word`)
  }
  if (l === 10) {
    const tool = findTool(m, p)
    if (tool === undefined) {
      throw new InterpError(line, `G10 L10: tool ${String(p)} not found in the tool table`)
    }
    if (block.words.has('X') || block.words.has('Y')) {
      throw new InterpError(line, 'G10 L10: only the Z word (tool length) is supported by the simulator')
    }
    const z = word('Z')
    if (z === undefined) {
      throw new InterpError(line, 'G10 L10 needs a Z word')
    }
    // The length that makes Z read `z` here once the offset is applied (G43).
    tool.length = m.pos[2] - workOffset(m)[2] - m.g92[2] - z * scale
    return
  }
  if (p < 0 || p > 9) {
    throw new InterpError(line, `G10 L${String(l)}: P${String(p)} is not a coordinate system (0 to 9)`)
  }
  const wcs = p === 0 ? m.wcs : p
  const offset = workOffset(m, wcs)
  // Present position in the target system; equals `present` when it is the active one.
  const there = toWork({ ...m, wcs }, m.pos)
  const here = wcs === m.wcs ? present : there
  for (const axis of axisIndices) {
    const value = word(axisWords[axis] ?? '')
    if (value !== undefined) {
      if (l === 2) {
        offset[axis] = value * scale
      } else {
        offset[axis] += here[axis] - value * scale
      }
    }
  }
  const r = word('R')
  if (r !== undefined) {
    if (l !== 2) {
      throw new InterpError(line, 'G10 L20: R word is not used')
    }
    m.rotations[wcs - 1] = r
  }
  m.offsets[wcs - 1] = offset
}

function motion(
  m: Modal,
  block: Block,
  word: (letter: string) => number | undefined,
  used: Set<string>,
  line: number,
  scale: number,
  machineCoordinates: boolean,
): Op | null {
  const name = `G${String(m.motion)}`
  const start = toWork(m, m.pos)
  const given: (number | undefined)[] = axisWords.map((letter) => word(letter))
  let endMachine: Vec3
  let end: Vec3
  if (machineCoordinates) {
    if (m.motion > 1) {
      throw new InterpError(line, `G53 cannot be used with ${name}; only G0 or G1`)
    }
    if (!m.absolute) {
      throw new InterpError(line, 'G53 cannot be used in incremental distance mode (G91)')
    }
    endMachine = [...m.pos]
    for (const axis of axisIndices) {
      const value = given[axis]
      if (value !== undefined) {
        endMachine[axis] = value * scale
      }
    }
    end = toWork(m, endMachine)
  } else {
    end = [...start]
    for (const axis of axisIndices) {
      const value = given[axis]
      if (value !== undefined) {
        end[axis] = m.absolute ? value * scale : start[axis] + value * scale
      }
    }
    endMachine = toMachine(m, end)
  }
  const base = { kind: 'move' as const, line, tool: m.tool, toolOffset: [...m.toolOffset] as Vec3 }
  if (m.motion === 0) {
    const from: Vec3 = [...m.pos]
    m.pos = endMachine
    return { ...base, motion: 'rapid', shape: 'linear', feed: 0, points: [from, endMachine] }
  }
  if (m.feed <= 0) {
    throw new InterpError(line, `cannot do ${name} with zero feed rate`)
  }
  if (m.motion === 1) {
    const from: Vec3 = [...m.pos]
    m.pos = endMachine
    return { ...base, motion: 'feed', shape: 'linear', feed: m.feed, points: [from, endMachine] }
  }
  const clockwise = m.motion === 2
  const centre = arcCentre(block, used, line, start, end, clockwise, scale)
  const points = arcPoints(start, end, centre, clockwise).map((p) => toMachine(m, p))
  // The exact ends, free of rounding from the two transforms.
  points[0] = [...m.pos]
  points[points.length - 1] = endMachine
  m.pos = endMachine
  return { ...base, motion: 'feed', shape: 'arc', feed: m.feed, points }
}

/**
 * Dry run of a whole program over a copy of `modal`. Points are tool tip positions: the
 * controlled point less the tool offset in effect, so paths cut with different tools line up.
 */
export function previewProgram(modal: Modal, lines: string[], blockDelete: boolean, context: number): Preview {
  const m = cloneModal(modal)
  const segments: PreviewSegment[] = []
  const min: Vec3 = [Infinity, Infinity, Infinity]
  const max: Vec3 = [-Infinity, -Infinity, -Infinity]
  let feeds = false
  let error: Preview['error'] = null
  let ended = false
  for (let index = 0; index < lines.length && !ended && error === null; index += 1) {
    try {
      for (const op of interpretLine(m, lines[index] ?? '', index + 1, blockDelete).ops) {
        if (op.kind === 'end') {
          ended = true
        } else if (op.kind === 'move') {
          const points: number[] = []
          for (const p of op.points) {
            for (const axis of axisIndices) {
              const v = p[axis] - op.toolOffset[axis]
              points.push(v)
              if (op.motion === 'feed') {
                feeds = true
                min[axis] = Math.min(min[axis], v)
                max[axis] = Math.max(max[axis], v)
              }
            }
          }
          segments.push({ line: op.line, kind: op.motion, tool: op.tool, points })
        }
      }
    } catch (caught) {
      if (!(caught instanceof InterpError)) {
        throw caught
      }
      error = { line: caught.line, text: caught.message }
    }
  }
  if (error === null && !ended) {
    const last = Math.max(1, lines.length)
    error = { line: last, text: `line ${String(last)}: file ended with no percent sign or program end (M2 or M30)` }
  }
  return {
    type: 'preview',
    context,
    state: error === null ? 'ready' : 'failed',
    error,
    segments,
    bounds: feeds ? { min, max } : null,
  }
}
