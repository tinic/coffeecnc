import { describe, expect, it } from 'vitest'

import type { Tool, Vec3 } from '../src/protocol'
import { defaultFiles, samplesDirectory, splitLines } from '../src/sim/files'
import {
  InterpError,
  activeCodes,
  interpretLine,
  newModal,
  previewProgram,
  toMachine,
  toWork,
  type Modal,
  type Op,
} from '../src/sim/gcode'

const tools: Tool[] = [
  { number: 1, pocket: 1, length: 50, diameter: 10, comment: 'end mill' },
  { number: 2, pocket: 2, length: 80, diameter: 5, comment: 'drill' },
]

function modal(): Modal {
  return newModal(tools.map((tool) => ({ ...tool })))
}

function run(m: Modal, ...lines: string[]): Op[] {
  return lines.flatMap((line, index) => interpretLine(m, line, index + 1, false).ops)
}

function moves(ops: Op[]): Extract<Op, { kind: 'move' }>[] {
  return ops.filter((op) => op.kind === 'move')
}

function rounded(p: Vec3 | undefined): number[] {
  return (p ?? [NaN, NaN, NaN]).map((v) => Math.round(v * 1000) / 1000 + 0)
}

function error(m: Modal, line: string, lineNumber = 12): string {
  try {
    interpretLine(m, line, lineNumber, false)
  } catch (caught) {
    if (caught instanceof InterpError) {
      return caught.message
    }
    throw caught
  }
  return ''
}

describe('linear moves', () => {
  it('moves to absolute and incremental targets', () => {
    const m = modal()
    run(m, 'G0 X10 Y20 Z5', 'G91', 'G1 X5 F100', 'G1 Y-2.5 Z-1')
    expect(rounded(m.pos)).toEqual([15, 17.5, 4])
    expect(m.feed).toBe(100)
  })

  it('keeps the motion mode from line to line', () => {
    const m = modal()
    const ops = moves(run(m, 'G1 X10 F200', 'Y10', 'G0 Z5', 'X0'))
    expect(ops.map((op) => op.motion)).toEqual(['feed', 'feed', 'rapid', 'rapid'])
  })

  it('reads N words, comments, lower case and missing spaces', () => {
    const m = modal()
    run(m, 'n10 g1x1.5y-2 f50 (first) ; rest', '(only a comment)', '', '%')
    expect(rounded(m.pos)).toEqual([1.5, -2, 0])
  })

  it('converts inch programs to millimetres', () => {
    const m = modal()
    run(m, 'G20', 'G1 X1 Y0.5 F10')
    expect(rounded(m.pos)).toEqual([25.4, 12.7, 0])
    expect(m.feed).toBeCloseTo(254)
    expect(m.units).toBe('inch')
    run(m, 'G21 G1 X1')
    expect(rounded(m.pos)).toEqual([1, 12.7, 0])
  })

  it('refuses a feed move without a feed rate', () => {
    expect(error(modal(), 'G1 X10')).toBe('line 12: cannot do G1 with zero feed rate')
  })
})

describe('arcs', () => {
  it('G2 with I J runs clockwise through the quarter points', () => {
    const m = modal()
    run(m, 'G0 X10 Y0')
    const [arc] = moves(run(m, 'G2 X0 Y-10 I-10 J0 F100'))
    expect(arc?.shape).toBe('arc')
    expect(rounded(arc?.points.at(-1))).toEqual([0, -10, 0])
    for (const p of arc?.points ?? []) {
      expect(Math.hypot(p[0], p[1])).toBeCloseTo(10, 6)
      expect(p[0]).toBeGreaterThanOrEqual(-1e-9)
      expect(p[1]).toBeLessThanOrEqual(1e-9)
    }
  })

  it('G3 with I J runs counterclockwise', () => {
    const m = modal()
    run(m, 'G0 X10 Y0')
    const [arc] = moves(run(m, 'G3 X0 Y10 I-10 F100'))
    const middle = arc?.points[Math.floor((arc.points.length - 1) / 2)]
    expect(middle?.[0]).toBeCloseTo(10 * Math.SQRT1_2, 0)
    expect(middle?.[1]).toBeCloseTo(10 * Math.SQRT1_2, 0)
  })

  it('an arc without an end point is a full circle', () => {
    const m = modal()
    run(m, 'G0 X5 Y0')
    const [arc] = moves(run(m, 'G2 I-5 F100'))
    let length = 0
    const points = arc?.points ?? []
    for (let n = 1; n < points.length; n += 1) {
      const a = points[n - 1] ?? [0, 0, 0]
      const b = points[n] ?? [0, 0, 0]
      length += Math.hypot(b[0] - a[0], b[1] - a[1])
    }
    expect(length).toBeCloseTo(2 * Math.PI * 5, 1)
    expect(rounded(m.pos)).toEqual([5, 0, 0])
  })

  it('R arcs take the short way for positive R and the long way for negative R', () => {
    const short = modal()
    run(short, 'G0 X10 Y0')
    const [minor] = moves(run(short, 'G3 X0 Y10 R10 F100'))
    expect(minor?.points.every((p) => p[0] >= -1e-9 && p[1] >= -1e-9)).toBe(true)

    const long = modal()
    run(long, 'G0 X10 Y0')
    const [major] = moves(run(long, 'G3 X0 Y10 R-10 F100'))
    // Centre at (10, 10): the long way round passes X20.
    expect(Math.max(...(major?.points ?? []).map((p) => p[0]))).toBeCloseTo(20, 1)
  })

  it('a helix changes Z evenly along the arc', () => {
    const m = modal()
    run(m, 'G0 X5 Y0 Z0')
    const [arc] = moves(run(m, 'G2 X5 Y0 Z-2 I-5 F100'))
    const points = arc?.points ?? []
    const last = points.length - 1
    points.forEach((p, n) => {
      expect(p[2]).toBeCloseTo((-2 * n) / last, 6)
    })
    expect(rounded(m.pos)).toEqual([5, 0, -2])
  })

  it('refuses arcs it cannot make', () => {
    const m = modal()
    run(m, 'G0 X10 Y0', 'F100')
    expect(error(m, 'G2 X0 Y5 I-10')).toMatch(/^line 12: G2: radius to end of arc differs from radius to start/)
    expect(error(m, 'G2 X0 Y0 R2')).toBe('line 12: G2: arc radius is too small to reach the end point')
    expect(error(m, 'G3 X0 Y10')).toBe('line 12: G3 needs I and J, or R')
    expect(error(m, 'G3 X0 Y10 K2')).toBe('line 12: K word is not used by G3 in the XY plane (G17)')
  })
})

describe('modal save and restore', () => {
  it('restores the saved modes without moving or resetting the active motion code', () => {
    const m = modal()
    run(m, 'G55 G21 G90 G99 F60 S500 M3', 'G43 H1', 'M70',
      'G54 G20 G91 G98 F2 S1000 M5', 'G49', 'G0 X0.1')
    const position = [...m.pos]
    expect(run(m, 'M72')).toEqual([])
    expect(m).toMatchObject({ wcs: 2, units: 'mm', absolute: true, retract: 99,
      feed: 60, speed: 500, spindle: 'forward', motion: 0, pos: position, lengthTool: 1, toolOffset: [0, 0, 50] })
    expect(error(modal(), 'M72')).toContain('previously saved M70')
  })
})

describe('offsets', () => {
  it('applies the active work coordinate system', () => {
    const m = modal()
    m.offsets[0] = [100, 50, -20]
    m.offsets[1] = [200, 60, -30]
    run(m, 'G54 G0 X1 Y2 Z3')
    expect(rounded(m.pos)).toEqual([101, 52, -17])
    run(m, 'G55 G0 X1 Y2 Z3')
    expect(rounded(m.pos)).toEqual([201, 62, -27])
    run(m, 'G59.3 G0 X1 Y2 Z3')
    expect(m.wcs).toBe(9)
    expect(rounded(m.pos)).toEqual([1, 2, 3])
  })

  it('G10 L2 sets an offset, G10 L20 makes the present position read a value', () => {
    const m = modal()
    run(m, 'G10 L2 P2 X10 Y20 Z-5')
    expect(m.offsets[1]).toEqual([10, 20, -5])
    run(m, 'G0 X30 Y40 Z-10', 'G10 L20 P1 X0 Y5')
    expect(m.offsets[0]).toEqual([30, 35, 0])
    expect(rounded(toWork(m, m.pos))).toEqual([0, 5, -10])
    run(m, 'G10 L20 P0 Z0')
    expect(rounded(toWork(m, m.pos))).toEqual([0, 5, 0])
  })

  it('G10 L2 R rotates the system about its origin', () => {
    const m = modal()
    run(m, 'G10 L2 P1 X100 Y100 R90', 'G0 X10 Y0')
    expect(rounded(m.pos)).toEqual([100, 110, 0])
    expect(rounded(toWork(m, m.pos))).toEqual([10, 0, 0])
  })

  it('applies the tool offset outside the rotation, as LinuxCNC does', () => {
    // machine = R(work + g92) + g5x + tool. A Z-only tool offset would hide a wrong order.
    const m = modal()
    run(m, 'G10 L2 P1 X100 Y50 Z-20 R90')
    m.g92 = [1, 2, 3]
    m.toolOffset = [7, -4, 60]
    // work (10, 0, 5): + g92 = (11, 2, 8); rotated 90° = (-2, 11, 8); + g5x = (98, 61, -12); + tool.
    expect(rounded(toMachine(m, [10, 0, 5]))).toEqual([105, 57, 48])
    expect(rounded(toWork(m, [105, 57, 48]))).toEqual([10, 0, 5])
    run(m, 'G0 X10 Y0 Z5')
    expect(rounded(m.pos)).toEqual([105, 57, 48])
  })

  it('work to machine and back is the identity for any offsets', () => {
    const m = modal()
    for (const degrees of [0, 90, -37.5, 180]) {
      run(m, `G10 L2 P1 X12.5 Y-40 Z-7 R${String(degrees)}`)
      m.g92 = [3, -2, 1]
      m.toolOffset = [0.75, -1.25, 48.2]
      for (const p of [[0, 0, 0], [10, -20, 30], [-123.456, 78.9, -0.001]] as Vec3[]) {
        const back = toWork(m, toMachine(m, p))
        for (const axis of [0, 1, 2] as const) {
          expect(back[axis]).toBeCloseTo(p[axis], 9)
        }
      }
    }
  })

  it('subtracts the tool offset from preview points exactly once', () => {
    const m = modal()
    run(m, 'G10 L2 P1 X100 Y50 Z-20 R90')
    m.toolOffset = [7, -4, 60]
    m.pos = toMachine(m, [0, 0, 0])
    const preview = previewProgram(m, ['G1 X10 F100', 'M2'], false, 1)
    // The tip is at R(work) + g5x whatever the tool offset is.
    expect(preview.segments[0]?.points.map((v) => Math.round(v * 1000) / 1000 + 0)).toEqual([100, 50, -20, 100, 60, -20])
  })

  it('G92 shifts every system', () => {
    const m = modal()
    run(m, 'G0 X10 Y10', 'G92 X0 Y0', 'G0 X5')
    expect(m.g92).toEqual([10, 10, 0])
    expect(rounded(m.pos)).toEqual([15, 10, 0])
  })

  it('G43 applies the loaded tool length and G49 removes it', () => {
    const m = modal()
    m.tool = 1
    run(m, 'G43', 'G0 Z10')
    expect(m.toolOffset).toEqual([0, 0, 50])
    expect(rounded(m.pos)).toEqual([0, 0, 60])
    run(m, 'G43 H2', 'G0 Z10')
    expect(rounded(m.pos)).toEqual([0, 0, 90])
    run(m, 'G49', 'G0 Z10')
    expect(rounded(m.pos)).toEqual([0, 0, 10])
    expect(error(m, 'G43 H7')).toBe('line 12: G43: tool 7 not found in the tool table')
  })

  it('G10 L10 sets the tool length so that Z reads the value once applied', () => {
    const m = modal()
    m.offsets[0] = [0, 0, -100]
    m.pos = [0, 0, -30]
    run(m, 'G10 L10 P1 Z2', 'G43 H1')
    expect(m.tools[0]?.length).toBe(68)
    expect(rounded(toWork(m, m.pos))).toEqual([0, 0, 2])
  })

  it('G53 moves in machine coordinates, ignoring offsets', () => {
    const m = modal()
    m.offsets[0] = [100, 50, -20]
    m.tool = 1
    run(m, 'G43', 'G53 G0 Z0', 'G53 G0 X5')
    expect(rounded(m.pos)).toEqual([5, 0, 0])
    expect(error(m, 'G91 G53 G0 X1')).toBe('line 12: G53 cannot be used in incremental distance mode (G91)')
  })
})

describe('machine functions', () => {
  it('sets spindle, coolant and tool state in order', () => {
    const m = modal()
    const ops = run(m, 'T2 M6', 'S1200 M3 M8', 'G4 P1.5', 'M0', 'M1', 'M5 M9', 'M30')
    expect(ops.map((op) => op.kind)).toEqual(['tool_change', 'dwell', 'pause', 'pause', 'end'])
    expect(m.tool).toBe(2)
    expect(m.speed).toBe(1200)
    expect(m.spindle).toBe('stopped')
    expect(m.flood).toBe(false)
  })

  it('reports active codes', () => {
    const m = modal()
    run(m, 'G55 G91 G20', 'S100 M4 M7')
    expect(activeCodes(m)).toEqual(['G1', 'G17', 'G20', 'G49', 'G55', 'G91', 'G94', 'M4', 'M7'])
  })

  it('turns (MSG, …) comments into operator messages', () => {
    expect(run(modal(), '(MSG, check the part)')).toEqual([{ kind: 'message', text: 'check the part' }])
  })

  it('skips deleted blocks only when block delete is on', () => {
    const m = modal()
    expect(interpretLine(m, '/ G0 X5', 1, true)).toEqual({ ops: [], blank: true })
    expect(interpretLine(m, '/ G0 X5', 1, false).ops).toHaveLength(1)
  })

  it('refuses tools that are not in the table and M6 without a tool', () => {
    expect(error(modal(), 'T9 M6')).toBe('line 12: requested tool 9 not found in the tool table')
    expect(error(modal(), 'M6')).toBe('line 12: M6 needs a tool selected with a T word')
  })
})

describe('what the interpreter does not support', () => {
  it('refuses unsupported compensated corners, a short lead, and an arc immediately after G40', () => {
    const inside = modal()
    run(inside, 'G1 G41 D1 X10 Y0 F100')
    expect(error(inside, 'G1 X10 Y10')).toContain('use a tangent arc')
    expect(error(modal(), 'G41 D1 G1 X5 F100')).toContain('longer than the cutter radius')
    const outside = modal()
    run(outside, 'G1 G42 D1 X10 Y0 F100', 'G1 X10 Y10')
    expect(error(outside, 'G40 G3 X12 Y12 I0 J2')).toContain('straight move before an arc')
  })

  it('names the line and the word, and never skips', () => {
    const m = modal()
    expect(error(m, 'G41 D9')).toBe('line 12: G41: tool 9 needs a nonnegative diameter in the tool table')
    expect(error(m, 'G82 R.1 Z-.5 P1')).toBe('line 12: G82 is not supported by the simulator')
    expect(error(m, 'G38.2 Z-1')).toBe('line 12: G38.2 is not supported by the simulator')
    expect(error(m, 'G93')).toBe('line 12: G93 is not supported by the simulator')
    expect(error(m, 'G95')).toBe('line 12: G95 is not supported by the simulator')
    expect(error(m, 'M98 P100')).toBe('line 12: M98 is not supported by the simulator')
    expect(error(m, '#1=0')).toBe('line 12: parameters (#) are not supported by the simulator')
    expect(error(m, 'G0 Z#7')).toBe('line 12: parameters (#) are not supported by the simulator')
    expect(error(m, 'G0 X[1+2]')).toBe('line 12: expressions ([ ]) are not supported by the simulator')
    expect(error(m, 'O1 while [#9 lt #6]')).toBe(
      'line 12: O words (subroutines and loops) are not supported by the simulator',
    )
    expect(error(m, 'G0 A90')).toBe('line 12: A word is not supported by the simulator')
    expect(error(m, 'G0 X1 P2')).toBe('line 12: P word is not used by any code on this line')
    expect(error(m, 'G0 G1 X1')).toBe('line 12: G0 and G1 cannot be used on the same line')
    expect(error(m, 'G10 L1 P1 Z5')).toBe('line 12: G10 L1 is not supported by the simulator')
    expect(error(m, 'G0 X')).toBe('line 12: X word has no number')
    expect(error(m, '(open')).toBe('line 12: comment is not closed with )')
  })
})

describe('cutter compensation exits and tool selection', () => {
  it('allows a short exit and a Z-only exit, then accepts an arc', () => {
    const m = modal()
    run(m, 'G41 D1 G1 X10 F100', 'G40 G0 X10.1 Y0')
    expect(rounded(m.pos)).toEqual([10.1, 0, 0])
    const at = modal()
    const path = moves(run(at, 'G41 D1 G1 X10 F100', 'G40 G0 Z5', 'G3 X20 Y10 I0 J10'))
    expect(path[1]?.points.map(rounded)).toEqual([[10, 5, 0], [10, 0, 5]])
    expect(rounded(at.pos)).toEqual([20, 10, 5])
  })

  it('uses the spindle tool for D0 and makes no offset for an empty spindle or zero diameter', () => {
    const loaded = modal()
    run(loaded, 'T1 M6', 'G41 D0 G1 X10 F100')
    expect(rounded(loaded.pos)).toEqual([10, 5, 0])
    const empty = modal()
    run(empty, 'G41 D0 G1 X1 F100')
    expect(rounded(empty.pos)).toEqual([1, 0, 0])
    const zero = newModal([{ number: 3, pocket: 3, comment: '', length: 50, diameter: 0 }])
    run(zero, 'G42 D3 G1 X1 F100', 'G40 G3 X2 Y1 I0 J1')
    expect(rounded(zero.pos)).toEqual([2, 1, 0])
  })
})

describe('preview', () => {
  it('returns one segment per move with tool tip positions', () => {
    const m = modal()
    m.offsets[0] = [100, 100, -50]
    const preview = previewProgram(
      m,
      ['T1 M6', 'G43', 'G0 X0 Y0 Z5', 'G1 Z-1 F100', 'G1 X10', 'T2 M6', 'G43', 'G1 Y10 Z-1', 'M2'],
      false,
      7,
    )
    expect(preview.state).toBe('ready')
    expect(preview.context).toBe(7)
    expect(preview.segments.map((s) => [s.line, s.kind, s.tool])).toEqual([
      [3, 'rapid', 1],
      [4, 'feed', 1],
      [5, 'feed', 1],
      [8, 'feed', 2],
    ])
    expect(preview.segments[2]?.points).toEqual([100, 100, -51, 110, 100, -51])
    // The second tool is 30 mm longer: its tip starts that much lower, and cuts at the same height.
    expect(preview.segments[3]?.points).toEqual([110, 100, -81, 110, 110, -51])
    expect(preview.bounds).toEqual({ min: [100, 100, -81], max: [110, 110, -45] })
    // The dry run leaves the real state alone.
    expect(m.tool).toBe(0)
    expect(m.pos).toEqual([0, 0, 0])
  })

  it('fails at the first line the interpreter refuses and keeps the path so far', () => {
    const preview = previewProgram(modal(), ['G0 X5', 'G1 X10 F100', 'G41 D9', 'G1 X20', 'M2'], false, 1)
    expect(preview.state).toBe('failed')
    expect(preview.error).toEqual({ line: 3, text: 'line 3: G41: tool 9 needs a nonnegative diameter in the tool table' })
    expect(preview.segments).toHaveLength(2)
  })

  it('fails when the program does not end', () => {
    const preview = previewProgram(modal(), ['G0 X5'], false, 1)
    expect(preview.error?.text).toBe('line 1: file ended with no percent sign or program end (M2 or M30)')
  })

  it('fails honestly on the unmodified LinuxCNC samples', () => {
    const files = defaultFiles()
    const failure = (name: string): Preview['error'] => {
      const text = files.get(`${samplesDirectory}/${name}`)
      expect(text).toBeDefined()
      const preview = previewProgram(modal(), splitLines(text ?? ''), false, 1)
      expect(preview.state).toBe('failed')
      return preview.error
    }
    expect(failure('gmoccapy_2_tools_with_cutter_radius_compensation.ngc')).toEqual({
      line: 17,
      text: 'line 17: requested tool 3 not found in the tool table',
    })
    expect(failure('gridprobe.ngc')).toEqual({
      line: 11,
      text: 'line 11: parameters (#) are not supported by the simulator',
    })
    expect(failure('hole-circle.ngc')).toEqual({
      line: 10,
      text: 'line 10: polar coordinates (^) are not supported by the simulator',
    })
  })
})

type Preview = ReturnType<typeof previewProgram>
