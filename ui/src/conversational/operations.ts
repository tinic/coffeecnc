// The operations the New operation dialog offers: their fields, defaults per unit, file names,
// and how the entered text becomes a generator's input.

import { parseNumericExpression } from '../numeric-expression'
import { bore } from './bore'
import { drilling } from './drilling'
import { facing, origins, type Origin } from './facing'
import { circularPocket, rectangularPocket } from './pocket'
import { number, unitName, type Common, type Generated, type Unit } from './program'

export type OperationId = 'facing' | 'drilling' | 'bore' | 'circular-pocket' | 'rectangular-pocket'

/** `length` is a size or depth, `coordinate` may be negative; both in the program's unit. */
export type NumberKind = 'length' | 'coordinate' | 'feed' | 'rpm' | 'percent' | 'angle' | 'count'

export type Values = Record<string, string>

interface FieldBase {
  key: string
  label: string
  /** Shown only when this holds for the entered values. */
  when?: (values: Values) => boolean
}
export type Field =
  | (FieldBase & { type: 'number'; kind: NumberKind })
  | (FieldBase & { type: 'choice'; options: readonly (readonly [string, string])[] })

export interface Operation {
  id: OperationId
  title: string
  fields: readonly Field[]
  defaults: (unit: Unit) => Values
  /** The generator's input from the parsed values and the common entries. */
  generate: (values: Parsed, common: Common) => Generated
  /** File name without the extension and without a number that makes it unique. */
  name: (values: Parsed, unit: Unit) => string
}

/** Parsed values: a number for number fields (NaN when not a number), the text for choices. */
export interface Parsed {
  num: (key: string) => number
  text: (key: string) => string
}

export function unitOf(kind: NumberKind, unit: Unit): string {
  switch (kind) {
    case 'length':
    case 'coordinate':
      return unitName(unit)
    case 'feed':
      return unit === 'inch' ? 'in/min' : 'mm/min'
    case 'rpm':
      return 'rpm'
    case 'percent':
      return '%'
    case 'angle':
      return '°'
    case 'count':
      return ''
  }
}

const num = (key: string, label: string, kind: NumberKind, when?: (values: Values) => boolean): Field =>
  ({ type: 'number', key, label, kind, ...(when ? { when } : {}) })
const yesNo = [['yes', 'Yes'], ['no', 'No']] as const

const feeds = (plunge: boolean): Field[] => [
  num('feed', 'Feed', 'feed'),
  ...(plunge ? [num('plungeFeed', 'Plunge feed', 'feed')] : []),
  num('rpm', 'Spindle speed', 'rpm'),
  num('clearance', 'Clearance height', 'length'),
]

/** Millimetre and inch defaults side by side. */
const pick = (unit: Unit, mm: Record<string, number | string>, inch: Record<string, number | string>): Values =>
  Object.fromEntries(Object.entries(unit === 'inch' ? inch : mm).map(([key, value]) => [key, String(value)]))

const speeds = { mm: { feed: 600, plungeFeed: 200, rpm: 8000, clearance: 5 }, inch: { feed: 24, plungeFeed: 8, rpm: 8000, clearance: 0.2 } }

const size = (value: number, unit: Unit): string => `${number(value, unit)}${unitName(unit)}`

const grid = (values: Values): boolean => values['pattern'] === 'grid'
const circle = (values: Values): boolean => values['pattern'] === 'circle'

export const operations: readonly Operation[] = [
  {
    id: 'facing',
    title: 'Facing',
    fields: [
      num('width', 'Size X', 'length'), num('height', 'Size Y', 'length'),
      { type: 'choice', key: 'origin', label: 'X0 Y0 at', options: origins },
      { type: 'choice', key: 'along', label: 'Passes along', options: [['x', 'X'], ['y', 'Y']] },
      num('stepover', 'Stepover', 'percent'),
      num('depth', 'Total depth', 'length'), num('depthPerPass', 'Depth per pass', 'length'),
      ...feeds(true),
    ],
    defaults: (unit) => pick(unit,
      { width: 100, height: 60, origin: 'lower-left', along: 'x', stepover: 70, depth: 1, depthPerPass: 0.5, ...speeds.mm },
      { width: 4, height: 2.5, origin: 'lower-left', along: 'x', stepover: 70, depth: 0.04, depthPerPass: 0.02, ...speeds.inch }),
    generate: (v, common) => facing({ ...common, width: v.num('width'), height: v.num('height'),
      origin: v.text('origin') as Origin, along: v.text('along') === 'y' ? 'y' : 'x', stepover: v.num('stepover'),
      depth: v.num('depth'), depthPerPass: v.num('depthPerPass'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `facing-${number(v.num('width'), unit)}x${size(v.num('height'), unit)}`,
  },
  {
    id: 'drilling',
    title: 'Hole pattern',
    fields: [
      { type: 'choice', key: 'pattern', label: 'Pattern', options: [['grid', 'Grid'], ['circle', 'Bolt circle']] },
      { type: 'choice', key: 'cycle', label: 'Cycle', options: [['G81', 'G81 drill'], ['G83', 'G83 peck']] },
      num('x', 'First hole X', 'coordinate', grid), num('y', 'First hole Y', 'coordinate', grid),
      num('x', 'Centre X', 'coordinate', circle), num('y', 'Centre Y', 'coordinate', circle),
      num('columns', 'Columns', 'count', grid), num('rows', 'Rows', 'count', grid),
      num('spacingX', 'Column spacing X', 'coordinate', grid), num('spacingY', 'Row spacing Y', 'coordinate', grid),
      num('count', 'Holes', 'count', circle), num('radius', 'Radius', 'length', circle),
      num('startAngle', 'First hole angle', 'angle', circle),
      num('depth', 'Hole depth', 'length'),
      num('peck', 'Peck depth', 'length', (values) => values['cycle'] === 'G83'),
      num('retract', 'Retract height (R)', 'length'),
      ...feeds(false),
    ],
    defaults: (unit) => pick(unit,
      { pattern: 'grid', cycle: 'G81', x: 10, y: 10, columns: 3, rows: 2, spacingX: 20, spacingY: 20, count: 6, radius: 25,
        startAngle: 0, depth: 10, peck: 2, retract: 2, ...speeds.mm, feed: 100 },
      { pattern: 'grid', cycle: 'G81', x: 0.5, y: 0.5, columns: 3, rows: 2, spacingX: 1, spacingY: 1, count: 6, radius: 1,
        startAngle: 0, depth: 0.4, peck: 0.08, retract: 0.08, ...speeds.inch, feed: 4 }),
    generate: (v, common) => drilling({ ...common, pattern: v.text('pattern') === 'circle' ? 'circle' : 'grid',
      x: v.num('x'), y: v.num('y'), columns: v.num('columns'), rows: v.num('rows'), spacingX: v.num('spacingX'),
      spacingY: v.num('spacingY'), count: v.num('count'), radius: v.num('radius'), startAngle: v.num('startAngle'),
      cycle: v.text('cycle') === 'G83' ? 'G83' : 'G81', peck: v.num('peck'), depth: v.num('depth'), retract: v.num('retract') }),
    name: (v) => v.text('pattern') === 'circle' ? `bolt-circle-${String(v.num('count'))}-holes`
      : `drill-grid-${String(v.num('columns'))}x${String(v.num('rows'))}`,
  },
  {
    id: 'bore',
    title: 'Helical bore',
    fields: [
      num('x', 'Centre X', 'coordinate'), num('y', 'Centre Y', 'coordinate'),
      num('diameter', 'Bore diameter', 'length'), num('depth', 'Depth', 'length'),
      num('pitch', 'Depth per turn', 'length'),
      { type: 'choice', key: 'finish', label: 'Finish pass', options: yesNo },
      num('finishAllowance', 'Finish allowance', 'length', (values) => values['finish'] === 'yes'),
      ...feeds(true),
    ],
    defaults: (unit) => pick(unit,
      { x: 0, y: 0, diameter: 10, depth: 10, pitch: 1, finish: 'yes', finishAllowance: 0.2, ...speeds.mm },
      { x: 0, y: 0, diameter: 0.375, depth: 0.4, pitch: 0.04, finish: 'yes', finishAllowance: 0.008, ...speeds.inch }),
    generate: (v, common) => bore({ ...common, x: v.num('x'), y: v.num('y'), diameter: v.num('diameter'),
      depth: v.num('depth'), pitch: v.num('pitch'), finish: v.text('finish') === 'yes',
      finishAllowance: v.num('finishAllowance'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `bore-${size(v.num('diameter'), unit)}`,
  },
  {
    id: 'circular-pocket',
    title: 'Circular pocket',
    fields: [
      num('x', 'Centre X', 'coordinate'), num('y', 'Centre Y', 'coordinate'),
      num('diameter', 'Pocket diameter', 'length'), num('stepover', 'Stepover', 'percent'),
      num('depth', 'Depth', 'length'), num('depthPerPass', 'Depth per pass', 'length'),
      ...feeds(true),
    ],
    defaults: (unit) => pick(unit,
      { x: 0, y: 0, diameter: 30, stepover: 40, depth: 5, depthPerPass: 1, ...speeds.mm },
      { x: 0, y: 0, diameter: 1.25, stepover: 40, depth: 0.2, depthPerPass: 0.04, ...speeds.inch }),
    generate: (v, common) => circularPocket({ ...common, x: v.num('x'), y: v.num('y'), diameter: v.num('diameter'),
      stepover: v.num('stepover'), depth: v.num('depth'), depthPerPass: v.num('depthPerPass'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `circular-pocket-${size(v.num('diameter'), unit)}`,
  },
  {
    id: 'rectangular-pocket',
    title: 'Rectangular pocket',
    fields: [
      num('x', 'Centre X', 'coordinate'), num('y', 'Centre Y', 'coordinate'),
      num('width', 'Size X', 'length'), num('height', 'Size Y', 'length'),
      num('cornerRadius', 'Corner radius', 'length'), num('stepover', 'Stepover', 'percent'),
      num('depth', 'Depth', 'length'), num('depthPerPass', 'Depth per pass', 'length'),
      ...feeds(true),
    ],
    defaults: (unit) => pick(unit,
      { x: 0, y: 0, width: 40, height: 30, cornerRadius: 0, stepover: 40, depth: 5, depthPerPass: 1, ...speeds.mm },
      { x: 0, y: 0, width: 1.5, height: 1.25, cornerRadius: 0, stepover: 40, depth: 0.2, depthPerPass: 0.04, ...speeds.inch }),
    generate: (v, common) => rectangularPocket({ ...common, x: v.num('x'), y: v.num('y'), width: v.num('width'),
      height: v.num('height'), cornerRadius: v.num('cornerRadius'), stepover: v.num('stepover'), depth: v.num('depth'),
      depthPerPass: v.num('depthPerPass'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `rectangular-pocket-${number(v.num('width'), unit)}x${size(v.num('height'), unit)}`,
  },
]

/** The fields shown for these values; a key appears once. */
export function visibleFields(operation: Operation, values: Values): Field[] {
  return operation.fields.filter((field) => field.when?.(values) ?? true)
}

/** Reads the entered text: arithmetic is allowed, lengths may carry mm or in. */
export function parse(operation: Operation, values: Values, unit: Unit): Parsed {
  const kinds = new Map(visibleFields(operation, values).flatMap((field) => field.type === 'number' ? [[field.key, field.kind] as const] : []))
  return {
    num: (key): number => {
      const kind = kinds.get(key)
      return kind === undefined ? NaN : parseNumericExpression(values[key] ?? '', unitOf(kind, unit)) ?? NaN
    },
    text: (key) => values[key] ?? '',
  }
}

/** `base.ngc`, or `base-2.ngc` and so on when that name is taken. */
export function uniqueName(base: string, taken: ReadonlySet<string>): string {
  const clean = base.replace(/[^A-Za-z0-9._-]+/g, '-')
  for (let n = 1; ; n += 1) {
    const name = n === 1 ? `${clean}.ngc` : `${clean}-${String(n)}.ngc`
    if (!taken.has(name)) return name
  }
}
