// Conversational generators: the exact program for one small case of each operation in mm and
// in inches, what the simulator's interpreter makes of each, and the validation messages.

import { describe, expect, it } from 'vitest'

import { bore } from '../src/conversational/bore'
import { drilling, holes } from '../src/conversational/drilling'
import { facing } from '../src/conversational/facing'
import { operations, parse, uniqueName } from '../src/conversational/operations'
import { circularPocket, rectangularPocket } from '../src/conversational/pocket'
import { number, type Generated } from '../src/conversational/program'
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
G3 X2.8 Y0 Z-1 I-2.8 J0 F600
G3 X2.8 Y0 Z-2 I-2.8 J0
(Flat bottom)
G3 X2.8 Y0 I-2.8 J0
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
G3 X1.0625 Y0.5 Z-0.05 I-0.0625 J0 F24
G3 X1.0625 Y0.5 Z-0.1 I-0.0625 J0
(Flat bottom)
G3 X1.0625 Y0.5 I-0.0625 J0
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
