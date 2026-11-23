// Conversational generators: the exact program for one small case of each operation in mm and
// in inches, what the simulator's interpreter makes of each, and the validation messages.

import { describe, expect, it } from 'vitest'

import { bore } from '../src/conversational/bore'
import { chamfer, type Chamfer } from '../src/conversational/chamfer'
import { contour, type Contour } from '../src/conversational/contour'
import { drilling, holes } from '../src/conversational/drilling'
import { facing } from '../src/conversational/facing'
import { operations, parse, uniqueName } from '../src/conversational/operations'
import { circularPocket, rectangularPocket } from '../src/conversational/pocket'
import { chainOperations, number, type Generated } from '../src/conversational/program'
import { slot, type Slot } from '../src/conversational/slot'
import { threadMill, type ThreadMill } from '../src/conversational/thread-mill'
import { splitLines } from '../src/sim/files'
import { newModal, previewProgram } from '../src/sim/gcode'

const mm = { unit: 'mm' as const, tool: { number: 1, diameter: 10, comment: 'End mill (4 flute)' }, rpm: 8000, feed: 600, clearance: 5 }
const inch = { unit: 'inch' as const, tool: { number: 2, diameter: 0.25, comment: '' }, rpm: 6000, feed: 24, clearance: 0.2 }

const cases: Record<string, () => Generated> = {
  facingMm: () => facing({ ...mm, width: 30, height: 20, origin: 'lower-left', along: 'x', stepover: 50, depth: 1, depthPerPass: 0.5, plungeFeed: 200 }),
  facingInch: () => facing({ ...inch, width: 1, height: 0.5, origin: 'centre', along: 'y', stepover: 100, depth: 0.02, depthPerPass: 0.02, plungeFeed: 8 }),
  gridMm: () => drilling({ ...mm, feed: 100, pattern: 'grid', x: 10, y: 10, columns: 3, rows: 2, spacingX: 20, spacingY: 15, count: NaN, radius: NaN, startAngle: NaN, cycle: 'G81', peck: NaN, depth: 5, retract: 2 }),
  circleInch: () => drilling({ ...inch, feed: 4, pattern: 'circle', x: 1, y: 1, columns: NaN, rows: NaN, spacingX: NaN, spacingY: NaN, count: 4, radius: 0.75, startAngle: 45, cycle: 'G83', peck: 0.1, depth: 0.3, retract: 0.1 }),
  boreMm: () => bore({ ...mm, x: 0, y: 0, diameter: 16, depth: 2, pitch: 1, finish: true, finishAllowance: 0.2, plungeFeed: 200 }),
  boreInch: () => bore({ ...inch, x: 1, y: 0.5, diameter: 0.375, depth: 0.1, pitch: 0.05, finish: false, finishAllowance: NaN, plungeFeed: 8 }),
  circularMm: () => circularPocket({ ...mm, x: 0, y: 0, diameter: 30, stepover: 50, depth: 2, depthPerPass: 1, plungeFeed: 200 }),
  circularInch: () => circularPocket({ ...inch, x: 0.5, y: -0.5, diameter: 0.75, stepover: 50, depth: 0.05, depthPerPass: 0.05, plungeFeed: 8 }),
  rectangularMm: () => rectangularPocket({ ...mm, x: 0, y: 0, width: 40, height: 30, cornerRadius: 8, stepover: 50, depth: 1, depthPerPass: 1, plungeFeed: 200 }),
  rectangularInch: () => rectangularPocket({ ...inch, x: 0, y: 0, width: 0.75, height: 0.5, cornerRadius: 0, stepover: 100, depth: 0.05, depthPerPass: 0.05, plungeFeed: 8 }),
}

const expected: Record<string, string> = {
  facingMm: `(Facing, made by CoffeeCNC)
(Units: mm. Z0 is the top of the stock.)
(Tool: T1, diameter 10, End mill 4 flute)
(Rectangle X 30 by Y 20, X0 Y0 at the lower-left corner)
(Passes along X, zig-zag, 4 per layer, stepover 5 = 50 %)
(Depth 1 in 2 layers of 0.5, plunge feed 200 outside the stock)
(Feed 600, spindle 8000 rpm, clearance Z5)

G17 G21 G40 G80 G90 G94
T1 M6
G43 H1
S8000 M3
G0 Z5

(Layer 1 of 2)
G0 X-6 Y0
G1 Z-0.5 F200
G1 X36 F600
G1 Y5
G1 X-6
G1 Y10
G1 X36
G1 Y15
G1 X-6
G0 Z5

(Layer 2 of 2)
G0 X-6 Y0
G1 Z-1 F200
G1 X36 F600
G1 Y5
G1 X-6
G1 Y10
G1 X36
G1 Y15
G1 X-6
G0 Z5
M5
M2
`,
  facingInch: `(Facing, made by CoffeeCNC)
(Units: in. Z0 is the top of the stock.)
(Tool: T2, diameter 0.25)
(Rectangle X 1 by Y 0.5, X0 Y0 at the centre)
(Passes along Y, zig-zag, 4 per layer, stepover 0.25 = 100 %)
(Depth 0.02 in 1 layer of 0.02, plunge feed 8 outside the stock)
(Feed 24, spindle 6000 rpm, clearance Z0.2)

G17 G20 G40 G80 G90 G94
T2 M6
G43 H2
S6000 M3
G0 Z0.2

(Layer 1 of 1)
G0 X-0.375 Y-0.4
G1 Z-0.02 F8
G1 Y0.4 F24
G1 X-0.125
G1 Y-0.4
G1 X0.125
G1 Y0.4
G1 X0.375
G1 Y-0.4
G0 Z0.2
M5
M2
`,
  gridMm: `(Hole pattern, made by CoffeeCNC)
(Units: mm. Z0 is the top of the stock.)
(Tool: T1, diameter 10, End mill 4 flute)
(Grid of 3 columns by 2 rows from X10 Y10, spacing X 20 Y 15)
(G81 drilling to depth 5, R plane Z2)
(G98: back to the clearance height between holes)
(Feed 100, spindle 8000 rpm, clearance Z5)

G17 G21 G40 G80 G90 G94
T1 M6
G43 H1
S8000 M3
G0 Z5

G0 X10 Y10
G98 G81 X10 Y10 Z-5 R2 F100
X30 Y10
X50 Y10
X50 Y25
X30 Y25
X10 Y25
G80
G0 Z5
M5
M2
`,
  circleInch: `(Hole pattern, made by CoffeeCNC)
(Units: in. Z0 is the top of the stock.)
(Tool: T2, diameter 0.25)
(Bolt circle of 4 holes, centre X1 Y1, radius 0.75, first hole at 45 degrees)
(G83 peck drilling, pecks of 0.1 to depth 0.3, R plane Z0.1)
(G98: back to the clearance height between holes)
(Feed 4, spindle 6000 rpm, clearance Z0.2)

G17 G20 G40 G80 G90 G94
T2 M6
G43 H2
S6000 M3
G0 Z0.2

G0 X1.5303 Y1.5303
G98 G83 X1.5303 Y1.5303 Z-0.3 R0.1 Q0.1 F4
X0.4697 Y1.5303
X0.4697 Y0.4697
X1.5303 Y0.4697
G80
G0 Z0.2
M5
M2
`,
  boreMm: `(Helical bore, made by CoffeeCNC)
(Units: mm. Z0 is the top of the stock.)
(Tool: T1, diameter 10, End mill 4 flute)
(Bore diameter 16 at X0 Y0, depth 2)
(Helix of 2 turns, 1 per turn, G3 climb milling, plunge feed 200)
(Finish pass: 0.2 left on the wall, cut in a helix back up)
(Feed 600, spindle 8000 rpm, clearance Z5)

G17 G21 G40 G80 G90 G94
T1 M6
G43 H1
S8000 M3
G0 Z5

G0 X2.8 Y0
G1 Z0 F200
G3 X2.8 Y0 Z-1 I-2.8 J0
G3 X2.8 Y0 Z-2 I-2.8 J0
(Flat bottom)
G3 X2.8 Y0 I-2.8 J0 F600
(Finish pass)
G1 X3
G3 X3 Y0 I-3 J0
G3 X3 Y0 Z-1 I-3 J0
G3 X3 Y0 Z0 I-3 J0
G1 X0 Y0
G0 Z5
M5
M2
`,
  boreInch: `(Helical bore, made by CoffeeCNC)
(Units: in. Z0 is the top of the stock.)
(Tool: T2, diameter 0.25)
(Bore diameter 0.375 at X1 Y0.5, depth 0.1)
(Helix of 2 turns, 0.05 per turn, G3 climb milling, plunge feed 8)
(Feed 24, spindle 6000 rpm, clearance Z0.2)

G17 G20 G40 G80 G90 G94
T2 M6
G43 H2
S6000 M3
G0 Z0.2

G0 X1.0625 Y0.5
G1 Z0 F8
G3 X1.0625 Y0.5 Z-0.05 I-0.0625 J0
G3 X1.0625 Y0.5 Z-0.1 I-0.0625 J0
(Flat bottom)
G3 X1.0625 Y0.5 I-0.0625 J0 F24
G1 X1 Y0.5
G0 Z0.2
M5
M2
`,
  circularMm: `(Circular pocket, made by CoffeeCNC)
(Units: mm. Z0 is the top of the stock.)
(Tool: T1, diameter 10, End mill 4 flute)
(Diameter 30 at X0 Y0)
(2 rings 5 apart, stepover 50 %, G3 climb milling)
(Depth 2 in 2 layers of 1, plunge straight down at the centre at 200: centre-cutting end mill)
(Feed 600, spindle 8000 rpm, clearance Z5)

G17 G21 G40 G80 G90 G94
T1 M6
G43 H1
S8000 M3
G0 Z5
G91.1

G0 X0 Y0
(Layer 1 of 2)
G1 Z-1 F200
G1 X5 Y0 F600
G3 X5 Y0 I-5 J0
G1 X10 Y0
G3 X10 Y0 I-10 J0
G1 X0 Y0
(Layer 2 of 2)
G1 Z-2 F200
G1 X5 Y0 F600
G3 X5 Y0 I-5 J0
G1 X10 Y0
G3 X10 Y0 I-10 J0
G1 X0 Y0
G0 Z5
M5
M2
`,
  circularInch: `(Circular pocket, made by CoffeeCNC)
(Units: in. Z0 is the top of the stock.)
(Tool: T2, diameter 0.25)
(Diameter 0.75 at X0.5 Y-0.5)
(2 rings 0.125 apart, stepover 50 %, G3 climb milling)
(Depth 0.05 in 1 layer of 0.05, plunge straight down at the centre at 8: centre-cutting end mill)
(Feed 24, spindle 6000 rpm, clearance Z0.2)

G17 G20 G40 G80 G90 G94
T2 M6
G43 H2
S6000 M3
G0 Z0.2
G91.1

G0 X0.5 Y-0.5
(Layer 1 of 1)
G1 Z-0.05 F8
G1 X0.625 Y-0.5 F24
G3 X0.625 Y-0.5 I-0.125 J0
G1 X0.75 Y-0.5
G3 X0.75 Y-0.5 I-0.25 J0
G1 X0.5 Y-0.5
G0 Z0.2
M5
M2
`,
  rectangularMm: `(Rectangular pocket, made by CoffeeCNC)
(Units: mm. Z0 is the top of the stock.)
(Tool: T1, diameter 10, End mill 4 flute)
(Size X 40 by Y 30, corner radius 8, centre X0 Y0)
(3 loops 5 apart, stepover 50 %, G3 climb milling)
(Depth 1 in 1 layer of 1, plunge straight down at the centre at 200: centre-cutting end mill)
(Feed 600, spindle 8000 rpm, clearance Z5)

G17 G21 G40 G80 G90 G94
T1 M6
G43 H1
S8000 M3
G0 Z5
G91.1

G0 X0 Y0
(Layer 1 of 1)
G1 Z-1 F200
G1 X5 Y0 F600
G1 X-5 Y0
G1 X5 Y0
G1 X10 Y0
G1 X10 Y5
G1 X-10 Y5
G1 X-10 Y-5
G1 X10 Y-5
G1 X10 Y0
G1 X15 Y0
G1 X15 Y7
G3 X12 Y10 I-3 J0
G1 X-12 Y10
G3 X-15 Y7 I0 J-3
G1 X-15 Y-7
G3 X-12 Y-10 I3 J0
G1 X12 Y-10
G3 X15 Y-7 I0 J3
G1 X15 Y0
G1 X0 Y0
G0 Z5
M5
M2
`,
  rectangularInch: `(Rectangular pocket, made by CoffeeCNC)
(Units: in. Z0 is the top of the stock.)
(Tool: T2, diameter 0.25)
(Size X 0.75 by Y 0.5, corner radius 0.125, centre X0 Y0)
(2 loops 0.125 apart, stepover 100 %, G3 climb milling)
(Depth 0.05 in 1 layer of 0.05, plunge straight down at the centre at 8: centre-cutting end mill)
(Feed 24, spindle 6000 rpm, clearance Z0.2)

G17 G20 G40 G80 G90 G94
T2 M6
G43 H2
S6000 M3
G0 Z0.2
G91.1

G0 X0 Y0
(Layer 1 of 1)
G1 Z-0.05 F8
G1 X0.125 Y0 F24
G1 X-0.125 Y0
G1 X0.125 Y0
G1 X0.25 Y0
G1 X0.25 Y0.125
G1 X-0.25 Y0.125
G1 X-0.25 Y-0.125
G1 X0.25 Y-0.125
G1 X0.25 Y0
G1 X0 Y0
G0 Z0.2
M5
M2
`,
}

function text(generated: Generated): string {
  if ('errors' in generated) throw new Error(generated.errors.join(' '))
  return generated.text
}
function errors(generated: Generated): string[] {
  return 'errors' in generated ? generated.errors : []
}

describe('reaming and boring', () => {
  const operation = operations.find((entry) => entry.id === 'hole-finishing')
  if (!operation) throw new Error('Missing ream / bore operation')

  it('uses G85 for a metric grid and retracts over the work before the next hole', () => {
    const values = { ...operation.defaults('mm'), columns: '2', rows: '1' }
    const input = parse(operation, values, 'mm')
    const program = text(operation.generate(input, { ...mm, feed: 60 }))
    expect(operation.name(input, 'mm')).toBe('ream-grid-2x1')
    expect(program).toContain('G98 G85 X10 Y10 Z-10 R2 F60\nX30 Y10\nG80')
  })

  it('writes G89 for an inch bolt circle with a dwell expression in seconds', () => {
    const values = { ...operation.defaults('inch'), pattern: 'circle', cycle: 'G89', dwell: '3/8s' }
    const input = parse(operation, values, 'inch')
    const program = text(operation.generate(input, { ...inch, feed: 2.4 }))
    expect(operation.name(input, 'inch')).toBe('bore-circle-6-holes')
    expect(program).toContain('G98 G89 X1.5 Y0.5 Z-0.4 R0.08 P0.375 F2.4')
    const invalid = parse(operation, { ...values, dwell: '-1/2s' }, 'inch')
    expect(errors(operation.generate(invalid, inch))[0]).toContain('Bottom dwell')
  })
})

describe('contours', () => {
  const tools = [
    { number: 1, pocket: 1, length: 50, diameter: 10, comment: '' },
    { number: 2, pocket: 2, length: 40, diameter: 6.35, comment: '' },
  ]
  const circular = { ...mm, shape: 'circle', x: 10, y: 12, diameter: 40,
    inside: false, clockwise: true, depth: 2, depthPerPass: 1, plungeFeed: 150 } as const satisfies Contour

  it.each([[false, true, 'G41', 25], [true, true, 'G42', 15], [false, false, 'G42', 25], [true, false, 'G41', 15]] as const)(
    'cuts a compensated circle (inside %s, clockwise %s)',
    (inside, clockwise, compensation, radius) => {
      const lines = splitLines(text(contour({ ...circular, inside, clockwise })))
      expect(lines.filter((line) => line.startsWith(`${compensation} D1 G1`))).toHaveLength(2)
      expect(lines.filter((line) => line.startsWith('G40 G0'))).toHaveLength(2)
      const preview = previewProgram(newModal(tools), lines, false, 1)
      expect(preview.error).toBeNull()
      const circleLine = lines.findIndex((line) => line === `${clockwise ? 'G2' : 'G3'} X10 Y-8 I0 J20`) + 1
      const points = preview.segments.find((s) => s.line === circleLine)?.points ?? []
      expect(points.length).toBeGreaterThan(20)
      for (let i = 0; i < points.length; i += 3) {
        expect(Math.hypot((points[i] ?? NaN) - 10, (points[i + 1] ?? NaN) - 12)).toBeCloseTo(radius, 7)
      }
    },
  )

  it('previews a sharp outside rectangle and a rounded inside rectangle in inches', () => {
    const template = { ...inch, shape: 'rectangle', x: 0.2, y: 0.4, width: 2, height: 1.5,
      clockwise: true, depth: 0.1, depthPerPass: 0.05, plungeFeed: 8 } as const
    for (const [inside, corner] of [[false, 0], [true, 0.2]] as const) {
      const lines = splitLines(text(contour({ ...template, inside, corner })))
      const preview = previewProgram(newModal(tools), lines, false, 1)
      expect(preview.state).toBe('ready')
      expect(preview.error).toBeNull()
      expect(lines.at(-1)).toBe('M2')
    }
  })

  it('refuses an inside contour without room to enter or with an undersized corner', () => {
    expect(errors(contour({ ...circular, inside: true, diameter: 15 }))).toContain(
      'The tool and its lead-in do not fit inside this profile. Use a smaller tool or a larger profile.')
    expect(errors(contour({ ...circular, shape: 'rectangle', inside: true, width: 40, height: 35, corner: 4 }))).toContain(
      'An inside corner radius must be greater than the cutter radius (5).')
  })
})

describe('chamfers', () => {
  const tools = [
    { number: 1, pocket: 1, length: 0, diameter: 6, comment: '' },
    { number: 2, pocket: 2, length: 0, diameter: 6.35, comment: '' },
  ]
  const outside = { ...mm, tool: { ...mm.tool, diameter: 6 }, x: 0, y: 0, inside: false,
    shape: 'rectangle', width: 40, height: 30, corner: 0, chamferWidth: 1, includedAngle: 90,
    tipDiameter: 0, tipBelow: 0.5, openingDepth: NaN, entryFeed: 60, feed: 150 } as const satisfies Chamfer
  const inside = { ...inch, x: 0.5, y: -0.25, inside: true, shape: 'circle', diameter: 1,
    chamferWidth: 0.04, includedAngle: 90, tipDiameter: 0.04, tipBelow: 0.02,
    openingDepth: 0.2, entryFeed: 2.4, feed: 6 } as const satisfies Chamfer

  it('rolls into a metric outside rectangle and rounds the tool path around sharp corners', () => {
    const program = text(chamfer(outside))
    expect(program).toContain('G0 X1.001 Y-16.501\nG1 Z-1.5 F60\nG3 X0 Y-15.5 I-1.001 J0 F150')
    expect(program).toContain('G1 X-20 Y-15.5\nG2 X-20.5 Y-15 I0 J0.5')
    expect(program).toContain('G3 X-1.001 Y-16.501 I0 J-1.001\nG0 Z5')
    expect(previewProgram(newModal(tools), splitLines(program), false, 1).error).toBeNull()
  })

  it('accounts for a flat tip and enters a circular hole from its centre', () => {
    const program = text(chamfer(inside))
    expect(program).toContain('G17 G20')
    expect(program).toContain('G0 X0.5 Y-0.25\nG1 Z-0.06 F2.4\nG3 X0.5 Y-0.71 I0 J-0.23 F6')
    expect(program).toContain('G3 X0.5 Y-0.71 I0 J0.46\nG3 X0.5 Y-0.25 I0 J0.23')
    expect(previewProgram(newModal(tools), splitLines(program), false, 1).error).toBeNull()
  })

  it('offsets an inside rounded rectangle and checks the opening and cutter geometry', () => {
    const rounded = { ...outside, inside: true, corner: 3, openingDepth: 8 }
    expect(text(chamfer(rounded))).toContain('G3 X19.5 Y-12 I0 J2.5')
    expect(errors(chamfer({ ...rounded, corner: 0 }))[0]).toContain('inside corner radius')
    expect(errors(chamfer({ ...inside, openingDepth: 0.05 }))[0]).toContain('deeper than 0.06 in')
    expect(errors(chamfer({ ...inside, diameter: 0.12 }))[0]).toContain('too small')
    expect(errors(chamfer({ ...outside, tipDiameter: 4 }))[0]).toContain('maximum cutting diameter')
  })
})

describe('internal thread milling', () => {
  const tools = [
    { number: 1, pocket: 1, length: 0, diameter: 6, comment: '' },
    { number: 2, pocket: 2, length: 0, diameter: 6.35, comment: '' },
  ]
  const metric = { ...mm, tool: { ...mm.tool, diameter: 6 }, x: 10, y: -5, majorDiameter: 10,
    pilotDiameter: 8.5, pilotDepth: 12, depth: 3.75, pitch: 1.5, passes: 2, leftHand: false,
    feed: 60, entryFeed: 50 } satisfies ThreadMill

  it('cuts ascending right-hand helices with one pitch per turn and a partial last turn', () => {
    const program = text(threadMill(metric))
    expect(program).toContain('M70\nG61 G91.1\nG0 X10 Y-5')
    expect(program).toContain('G1 Z-4.125 F50\nG3 X11.625 Y-5 Z-3.75 I0.8125 J0 F60')
    expect(program).toContain('G3 X12 Y-5 Z-2.25 I-2 J0\nG3 X12 Y-5 Z-0.75 I-2 J0')
    expect(program).toContain('G3 X8 Y-5 Z0 I-2 J0\nG3 X10 Y-5 Z0.375 I1 J0')
    expect(program).toContain('G0 Z5\nM72\nM5\nM2\n')
    const preview = previewProgram(newModal(tools), splitLines(program), false, 1)
    expect(preview.error).toBeNull()
    expect(preview.state).toBe('ready')
  })

  it('keeps an imperial fractional pitch and descends for a left-hand thread', () => {
    const operation = operations.find((entry) => entry.id === 'thread-mill')
    if (!operation) throw new Error('Missing thread milling operation')
    const values = { ...operation.defaults('inch'), pitch: '1/28in', hand: 'left', x: '1/4in' }
    const program = text(operation.generate(parse(operation, values, 'inch'), inch))
    expect(program).toContain('G17 G20')
    expect(program).toContain('G0 X0.25 Y0\n\n(Radial pass 1 of 2)\nG1 Z0.008929 F2')
    expect(program).toContain('Z-0.035714 I-0.046875 J0')
    expect(program).toContain('Z-0.357143 I-0.046875 J0')
    expect(program).toContain('G3 X0.25 Y0 Z-0.383929')
    expect(previewProgram(newModal(tools), splitLines(program), false, 1).error).toBeNull()
  })

  it('refuses an undersized pilot, shallow hole, insufficient clearance or a sub-pitch thread', () => {
    expect(errors(threadMill({ ...metric, pilotDiameter: 6 }))[0]).toContain('wider than the cutter')
    expect(errors(threadMill({ ...metric, pilotDepth: 4 }))[0]).toContain('deeper than 4.125')
    expect(errors(threadMill({ ...metric, clearance: 0.2 }))[0]).toContain('clearance')
    expect(errors(threadMill({ ...metric, depth: 0.75 }))[0]).toContain('between 1 and 500 turns')
  })
})

describe('slots and keyways', () => {
  const tools = [
    { number: 1, pocket: 1, length: 50, diameter: 10, comment: '' },
    { number: 2, pocket: 2, length: 40, diameter: 6.35, comment: '' },
  ]
  const closed = { ...mm, along: 'x', ends: 'closed', x: 20, y: 10, length: 50, width: 16,
    depth: 3.7, depthPerPass: 2, angle: 1, plunge: false, entryFeed: 120 } as const satisfies Slot
  const open = { ...inch, along: 'y', ends: 'high', x: 0.2, y: 0.5, length: 1.5, width: 0.375,
    depth: 0.1, depthPerPass: 0.05, angle: NaN, plunge: false, entryFeed: 4 } as const satisfies Slot

  it('ramps a closed slot within the maximum angle and sweeps both rounded ends at every layer', () => {
    const program = text(slot(closed))
    expect(program).toContain('G0 X3 Y10\nG1 Z0 F120')
    const rampZ = [...program.matchAll(/^G1 X(?:3|37) Y10 Z(-[\d.]+)(?: F120)?$/gm)].map((match) => Number(match[1]))
    expect(rampZ).toHaveLength(8)
    rampZ.forEach((z, index) => {
      expect(Math.abs(z - (rampZ[index - 1] ?? 0))).toBeLessThanOrEqual(34 * Math.tan(Math.PI / 180))
    })
    expect(rampZ.at(-1)).toBe(-3.7)
    expect(program.match(/^G3 X3 Y7 I0 J-3$/gm)).toHaveLength(2)
    expect(program.match(/^G3 X37 Y13 I0 J3$/gm)).toHaveLength(2)
    expect(previewProgram(newModal(tools), splitLines(program), false, 1).error).toBeNull()
  })

  it('starts outside the chosen stock edge and rotates its arcs onto the Y axis in inch', () => {
    const program = text(slot(open))
    expect(program).toContain('G0 X0.2 Y1.3751\nG1 Z-0.05 F4')
    expect(program).toContain('G1 X0.2 Y-0.0625 F24')
    expect(program).not.toMatch(/^G1 X0\.2 Y1\.3751$/m)
    expect(program).toContain('G3 X0.1375 Y1.3751 I-0.0625 J0')
    expect(previewProgram(newModal(tools), splitLines(program), false, 1).error).toBeNull()
  })

  it('allows short open slots and checks that a remaining closed end fits', () => {
    const both = slot({ ...closed, ends: 'both', length: 5, width: 16 })
    const one = slot({ ...closed, ends: 'low', length: 10, width: 16 })
    for (const made of [both, one])
      expect(previewProgram(newModal(tools), splitLines(text(made)), false, 1).error).toBeNull()
    expect(errors(slot({ ...closed, ends: 'low', length: 5, width: 16 }))[0]).toContain('half the slot width')
  })

  it('uses a centre-line pass for a tool-width keyway and rejects unmachinable dimensions', () => {
    const program = text(slot({ ...closed, width: 10, plunge: true }))
    expect(program).not.toMatch(/^G3 /m)
    expect(program).toContain('G1 Z-1.85 F120\nG1 X40 Y10 F600')
    expect(errors(slot({ ...closed, width: 9 }))[0]).toContain('at least the tool diameter')
    expect(errors(slot({ ...closed, length: 15 }))[0]).toContain('longer than it is wide')
    expect(errors(slot({ ...closed, angle: 0.00001 }))[0]).toContain('too short for this ramp angle')
  })
})

describe('conversational programs', () => {
  it.each(Object.keys(cases))('%s is the expected program', (name) => {
    expect(text(cases[name]?.() ?? { errors: ['no case'] })).toBe(expected[name])
  })

  it.each(Object.keys(cases))('%s interprets without error and ends at the clearance height', (name) => {
    const tools = [{ number: 1, pocket: 1, length: 50, diameter: 10, comment: '' }, { number: 2, pocket: 2, length: 40, diameter: 6.35, comment: '' }]
    const preview = previewProgram(newModal(tools), splitLines(expected[name] ?? ''), false, 1)
    expect(preview.error).toBeNull()
    expect(preview.state).toBe('ready')
    expect(preview.segments.some((segment) => segment.kind === 'feed')).toBe(true)
  })

  it('joins a facing and drilling operation with one program end and reaches the second toolpath', () => {
    const face = { tool: 1, text: expected['facingMm'] ?? '' }
    const drill = { tool: 2, text: (expected['gridMm'] ?? '').replace('T1 M6\nG43 H1', 'T2 M6\nG43 H2') }
    const joined = text(chainOperations([face, drill], 'mm', -0.5))
    expect(text(chainOperations([face], 'mm'))).toBe(face.text)
    expect(errors(chainOperations([face, drill], 'mm'))).toHaveLength(1)
    expect(joined).toContain('G0 Z5\nM5\n\nG90 G53 G0 Z-0.5\n\n(Hole pattern')
    expect(joined.indexOf('G53')).toBeLessThan(joined.indexOf('T2 M6'))
    expect(joined.match(/^T\d+ M6$/gm)).toEqual(['T1 M6', 'T2 M6'])
    expect(joined.match(/^M2$/gm)).toEqual(['M2'])
    expect(joined.endsWith('G80\nG0 Z5\nM5\nM2\n')).toBe(true)
    const tools = [
      { number: 1, pocket: 1, length: 50, diameter: 10, comment: '' },
      { number: 2, pocket: 2, length: 40, diameter: 5, comment: '' },
    ]
    const preview = previewProgram(newModal(tools), splitLines(joined), false, 1)
    expect(preview.error).toBeNull()
    expect(preview.segments.some((segment) => segment.kind === 'feed' && segment.points.includes(-5))).toBe(true)
  })

  it('keeps one tool loaded across consecutive operations that use it', () => {
    const face = { tool: 1, text: expected['facingMm'] ?? '' }
    const drill = { tool: 1, text: expected['gridMm'] ?? '' }
    const joined = text(chainOperations([face, drill], 'mm'))
    expect(joined.match(/^T\d+ M6$/gm)).toEqual(['T1 M6'])
    expect(joined).not.toContain('G53')
    expect(joined).toContain('G43 H1\nS')
    expect(joined).toContain('G98 G81')
  })

  it('writes numbers with the decimals of the unit and never -0', () => {
    expect(number(1 / 3, 'mm')).toBe('0.333')
    expect(number(1 / 3, 'inch')).toBe('0.3333')
    expect(number(-0.00001, 'mm')).toBe('0')
    expect(number(25, 'mm')).toBe('25')
  })

  it('drills grid rows back and forth and the bolt circle counter-clockwise', () => {
    const grid = holes({ ...mm, pattern: 'grid', x: 0, y: 0, columns: 2, rows: 2, spacingX: 10, spacingY: 5, count: NaN, radius: NaN,
      startAngle: NaN, cycle: 'G81', peck: NaN, depth: 1, retract: 1 })
    expect(grid).toEqual([[0, 0], [10, 0], [10, 5], [0, 5]])
    const circle = holes({ ...mm, pattern: 'circle', x: 0, y: 0, columns: NaN, rows: NaN, spacingX: NaN, spacingY: NaN, count: 4, radius: 10,
      startAngle: 0, cycle: 'G81', peck: NaN, depth: 1, retract: 1 }).map(([x, y]) => [Math.round(x) + 0, Math.round(y) + 0])
    expect(circle).toEqual([[10, 0], [0, 10], [-10, 0], [0, -10]])
  })
})

describe('material removal', () => {
  /** Feed polylines of the deepest layer, as XY segments. */
  function bottomPaths(generated: Generated): [number, number, number, number][] {
    const tools = [{ number: 1, pocket: 1, length: 0, diameter: 10, comment: '' }]
    const preview = previewProgram(newModal(tools), splitLines(text(generated)), false, 1)
    const bottom = Math.min(...preview.segments.flatMap((segment) => segment.points.filter((_, i) => i % 3 === 2)))
    const lines: [number, number, number, number][] = []
    for (const segment of preview.segments.filter((s) => s.kind === 'feed')) {
      const p = segment.points
      for (let i = 3; i < p.length; i += 3) {
        if (Math.abs((p[i + 2] ?? NaN) - bottom) < 1e-6 && Math.abs((p[i - 1] ?? NaN) - bottom) < 1e-6)
          lines.push([p[i - 3] ?? NaN, p[i - 2] ?? NaN, p[i] ?? NaN, p[i + 1] ?? NaN])
      }
    }
    return lines
  }
  function distance(x: number, y: number, [ax, ay, bx, by]: [number, number, number, number]): number {
    const dx = bx - ax
    const dy = by - ay
    const t = dx === 0 && dy === 0 ? 0 : Math.max(0, Math.min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return Math.hypot(x - ax - t * dx, y - ay - t * dy)
  }

  it.each([100, 90])('leaves nothing standing in a sharp-cornered pocket at a stepover of %i percent', (stepover) => {
    // A 10 mm tool; the tool centre's path at the wall is 30 by 18 or 20, so one loop of stepover
    // spacing separates the centre line from the wall.
    const height = stepover === 100 ? 30 : 28
    const paths = bottomPaths(rectangularPocket({ ...mm, x: 0, y: 0, width: 40, height, cornerRadius: 0, stepover,
      depth: 1, depthPerPass: 1, plungeFeed: 200 }))
    const a = 15
    const b = height / 2 - 5
    const uncut: string[] = []
    for (let x = -a; x <= a; x += 0.1) {
      for (let y = -b; y <= b; y += 0.1) {
        if (!paths.some((path) => distance(x, y, path) <= 5 + 1e-6)) uncut.push(`${x.toFixed(1)},${y.toFixed(1)}`)
      }
    }
    expect(uncut).toEqual([])
  })
})

describe('pocket entry and finishing', () => {
  const setup = { ...mm, x: 0, y: 0, stepover: 50, depth: 2, depthPerPass: 1, plungeFeed: 200 }
  const tools = [{ number: 1, pocket: 1, length: 0, diameter: 10, comment: '' }]

  it('helices down, roughs inside the allowance and finishes at each layer', () => {
    const program = text(circularPocket({
      ...setup, diameter: 30, entry: 'helix', entryAngle: 3, wallStock: 1, finish: true, coolant: 'flood',
    }))
    expect(program).toContain('G91.1\nM9\nM8\n\nG0 X2.5 Y0\nG1 Z0 F200')
    expect(program.match(/^G3 .* Z-/gm)).toHaveLength(4)
    expect(program).toContain('G3 X2.5 Y0 I-2.5 J0 Z-0.5')
    expect(program).toContain('G3 X9 Y0 I-9 J0\n(Finish walls)\nG1 X10 Y0\nG3 X10 Y0 I-10 J0')
    expect(program.match(/\(Finish walls\)/g)).toHaveLength(2)
    expect(program.endsWith('G0 Z5\nM9\nM5\nM2\n')).toBe(true)
    expect(previewProgram(newModal(tools), splitLines(program), false, 1).error).toBeNull()
  })

  it('ramps along the longer axis and keeps the requested wall stock in a rough-only pocket', () => {
    const program = text(rectangularPocket({
      ...setup, width: 20, height: 30, cornerRadius: 7, entry: 'ramp', entryAngle: 3, wallStock: 1, coolant: 'both',
    }))
    expect(program).toContain('M9\nM7\nM8\n\nG0 X0 Y-9\nG1 Z0 F200')
    expect(program.endsWith('G0 Z5\nM9\nM5\nM2\n')).toBe(true)
    expect(program).toContain('G1 X0 Y9 Z-0.5\nG1 X0 Y-9 Z-1')
    expect(program).not.toContain('(Finish walls)')
    const preview = previewProgram(newModal(tools), splitLines(program), false, 1)
    expect(preview.error).toBeNull()
    const feed = preview.segments.filter((segment) => segment.kind === 'feed')
    const x = feed.flatMap((segment) => segment.points.filter((_, i) => i % 3 === 0))
    const y = feed.flatMap((segment) => segment.points.filter((_, i) => i % 3 === 1))
    expect(Math.max(...x)).toBeCloseTo(4)
    expect(Math.max(...y)).toBeCloseTo(9)
  })

  it('forces coolant off before entry when the operator picks Off', () => {
    const program = text(circularPocket({ ...setup, diameter: 30, coolant: 'off' }))
    expect(program).toContain('G91.1\nM9\n\nG0 X0 Y0')
    expect(program.match(/^M9$/gm)).toEqual(['M9'])
  })

  it('retracts and stops pocket coolant before lifting for the next tool in a job', () => {
    const pocket = text(circularPocket({ ...setup, diameter: 30, coolant: 'flood' }))
    const drill = (expected['gridMm'] ?? '').replace('T1 M6\nG43 H1', 'T2 M6\nG43 H2')
    const job = text(chainOperations([{ tool: 1, text: pocket }, { tool: 2, text: drill }], 'mm', -0.5))
    expect(job).toContain('G0 Z5\nM9\nM5\n\nG90 G53 G0 Z-0.5\n\n(Hole pattern')
    expect(job.match(/^M2$/gm)).toEqual(['M2'])
  })

  it('refuses stock that leaves no room and entry angles below the usable precision', () => {
    expect(errors(circularPocket({ ...setup, diameter: 20, wallStock: 5 }))).toContain('Wall stock leaves too little room for the tool.')
    expect(errors(circularPocket({ ...setup, diameter: 20, entry: 'helix', entryAngle: 0.00001 })))
      .toContain('The entry path or angle is too small for this program’s coordinate precision.')
    expect(errors(rectangularPocket({ ...setup, width: 20, height: 30, cornerRadius: 0, entry: 'ramp', entryAngle: 90 })))
      .toContain('The entry angle must be greater than 0° and less than 90°.')
  })
})

describe('validation', () => {
  it('refuses zero and negative entries with the field named', () => {
    expect(errors(facing({ ...mm, width: 0, height: -5, origin: 'centre', along: 'x', stepover: 0, depth: 1, depthPerPass: NaN, plungeFeed: 100 }))).toEqual([
      'Size X must be more than zero.', 'Size Y must be more than zero.',
      'Stepover must be more than 0 % and at most 100 % of the tool diameter.', 'Depth per pass must be more than zero.',
    ])
    expect(errors(facing({ ...mm, feed: 0, rpm: -1, clearance: 0, width: 1, height: 1, origin: 'centre', along: 'x', stepover: 50, depth: 1, depthPerPass: 1, plungeFeed: 1 }))).toEqual([
      'Feed must be more than zero.', 'Spindle speed must be more than zero.', 'Clearance height must be more than zero.',
    ])
  })

  it('needs a tool from the table with a diameter', () => {
    expect(errors(circularPocket({ ...mm, tool: { number: 0, diameter: NaN, comment: '' }, x: 0, y: 0, diameter: 20, stepover: 50, depth: 1, depthPerPass: 1, plungeFeed: 100 }))).toEqual([
      'Choose a tool from the tool table.', 'The tool diameter in the tool table must be more than zero.',
    ])
  })

  it('refuses a tool too big for the pocket or the bore', () => {
    const pocket = { ...mm, x: 0, y: 0, stepover: 50, depth: 1, depthPerPass: 1, plungeFeed: 100 }
    expect(errors(circularPocket({ ...pocket, diameter: 10 }))).toEqual(['The tool (diameter 10) is too big for this pocket.'])
    expect(errors(rectangularPocket({ ...pocket, width: 30, height: 9, cornerRadius: 0 }))).toEqual(['The tool (diameter 10) is too big for this pocket.'])
    expect(errors(rectangularPocket({ ...pocket, width: 30, height: 20, cornerRadius: 3 }))).toEqual([
      'The corner radius is smaller than the tool radius (5). Enter 0 or at least the tool radius.',
    ])
    const hole = { ...mm, x: 0, y: 0, depth: 1, pitch: 1, finish: false, finishAllowance: NaN, plungeFeed: 100 }
    expect(errors(bore({ ...hole, diameter: 9 }))).toEqual(['The tool (diameter 10) is too big for this bore.'])
    expect(errors(bore({ ...hole, diameter: 25 }))).toEqual([
      'The bore is more than twice the tool diameter: a core would stay in the middle. Use a circular pocket.',
    ])
    expect(errors(bore({ ...hole, diameter: 12, finish: true, finishAllowance: 1 }))).toEqual([
      'The finish allowance must be less than half the difference of bore and tool diameter.',
    ])
  })

  it('writes no arc whose radius is zero as written', () => {
    const pocket = { ...mm, x: 0, y: 0, stepover: 50, depth: 1, depthPerPass: 1, plungeFeed: 100 }
    const hole = { ...mm, x: 0, y: 0, depth: 1, pitch: 1, finish: false, finishAllowance: NaN, plungeFeed: 100 }
    // A path radius of 0.0003 mm is written as 0.
    expect(errors(circularPocket({ ...pocket, diameter: 10.0006 }))).toEqual(['The tool (diameter 10) is too big for this pocket.'])
    expect(errors(bore({ ...hole, diameter: 10.0006 }))).toEqual(['The tool (diameter 10) is too big for this bore.'])
    expect(errors(bore({ ...hole, diameter: 12, finish: true, finishAllowance: 0.9997 }))).toEqual([
      'The finish allowance must be less than half the difference of bore and tool diameter.',
    ])
    const tools = [{ number: 1, pocket: 1, length: 50, diameter: 10, comment: '' }, { number: 2, pocket: 2, length: 40, diameter: 3, comment: '' }]
    for (const generated of [
      // Corner path radius 0.0003 mm.
      rectangularPocket({ ...pocket, width: 30, height: 20, cornerRadius: 5.0003 }),
      // Corner radius half the smaller side: the innermost loop's corner radius is 2e-16, not 0.
      rectangularPocket({ ...pocket, tool: { number: 2, diameter: 3, comment: '' }, stepover: 30, width: 26.8, height: 6.8, cornerRadius: 3.4 }),
    ]) {
      const program = text(generated)
      expect(program).not.toMatch(/^G3 .* I0 J0( |$)/m)
      expect(previewProgram(newModal(tools), splitLines(program), false, 1).error).toBeNull()
    }
  })

  it('checks the hole pattern', () => {
    const base = { ...mm, x: 0, y: 0, columns: 2.5, rows: 0, spacingX: 0, spacingY: 0, count: NaN, radius: NaN, startAngle: NaN,
      cycle: 'G83' as const, peck: 0, depth: 3, retract: 6 }
    expect(errors(drilling({ ...base, pattern: 'grid' }))).toEqual([
      'Columns must be a whole number from 1 to 1000.', 'Rows must be a whole number from 1 to 1000.', 'Column spacing must not be zero.',
      'The retract height must not be above the clearance height.', 'Peck depth must be more than zero.',
    ])
    expect(errors(drilling({ ...base, pattern: 'circle', count: 0, radius: -1, startAngle: 0, retract: 2, peck: 1 }))).toEqual([
      'Number of holes must be a whole number from 1 to 1000.', 'Bolt circle radius must be more than zero.',
    ])
  })
})

describe('dialog entries', () => {
  it('parses arithmetic and units, and names the file from the entries', () => {
    const op = operations.find((candidate) => candidate.id === 'facing')
    if (op === undefined) throw new Error('no facing')
    const values = { ...op.defaults('mm'), width: '2in', height: '120/2' }
    const parsed = parse(op, values, 'mm')
    expect(parsed.num('width')).toBeCloseTo(50.8)
    expect(parsed.num('height')).toBe(60)
    expect(op.name(parsed, 'mm')).toBe('facing-50.8x60mm')
  })

  it('has valid defaults in both units for a 6 mm tool', () => {
    for (const op of operations)
      for (const unit of ['mm', 'inch'] as const) {
        const parsed = parse(op, op.defaults(unit), unit)
        const generated = op.generate(parsed, { unit, tool: { number: 2, diameter: unit === 'inch' ? 0.25 : 6, comment: '' },
          rpm: parsed.num('rpm'), feed: parsed.num('feed'), clearance: parsed.num('clearance') })
        expect(errors(generated), `${op.id} ${unit}`).toEqual([])
      }
  })

  it('picks a name that is not taken', () => {
    expect(uniqueName('bore-10mm', new Set())).toBe('bore-10mm.ngc')
    expect(uniqueName('bore-10mm', new Set(['bore-10mm.ngc', 'bore-10mm-2.ngc']))).toBe('bore-10mm-3.ngc')
    expect(uniqueName('a b/c', new Set())).toBe('a-b-c.ngc')
  })
})
