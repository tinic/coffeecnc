// The operations the New operation dialog offers: their fields, defaults per unit, file names,
// and how the entered text becomes a generator's input.

import { parseNumericExpression } from '../numeric-expression'
import { bore } from './bore'
import { contour } from './contour'
import { drilling, type Drilling } from './drilling'
import { facing, origins, type Origin } from './facing'
import { circularPocket, rectangularPocket, type PocketOptions } from './pocket'
import { number, unitName, type Common, type Generated, type LabelKey, type OperationsKey, type Unit } from './program'
import { slot, type SlotEnds } from './slot'
import { rigidTapping } from './rigid-tapping'
import { threadMill } from './thread-mill'

export type OperationId = 'facing' | 'drilling' | 'bore' | 'circular-pocket' | 'rectangular-pocket' | 'contour' | 'slot' | 'hole-finishing' | 'thread-mill' | 'rigid-tap'

/** `length` is a size or depth, `coordinate` may be negative; both in the program's unit. */
export type NumberKind = 'length' | 'coordinate' | 'feed' | 'rpm' | 'percent' | 'angle' | 'count' | 'duration'

export type Values = Record<string, string>

/** The message key of an option's name. */
export type OptionKey = OperationsKey<'option'> | 'common.yes' | 'common.no'

// Labels, titles and notes are message keys: the dialog translates them where it shows them.
interface FieldBase {
  key: string
  label: LabelKey
  /** Shown only when this holds for the entered values. */
  when?: (values: Values) => boolean
}
export type Field =
  | (FieldBase & { type: 'number'; kind: NumberKind })
  | (FieldBase & { type: 'choice'; options: readonly (readonly [string, OptionKey])[]; select?: boolean })

export interface Operation {
  id: OperationId
  title: OperationsKey<'operation'>
  /** Shown above the fields: what the operation needs from the tool or the setup. */
  note?: (values: Values) => OperationsKey<'operation'>
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
    case 'duration':
      return 's'
  }
}

const num = (key: string, label: LabelKey, kind: NumberKind, when?: (values: Values) => boolean): Field =>
  ({ type: 'number', key, label, kind, ...(when ? { when } : {}) })
const yesNo = [['yes', 'common.yes'], ['no', 'common.no']] as const

const feeds = (plunge: boolean, label: LabelKey = 'operations.label.plungeFeed'): Field[] => [
  num('feed', 'operations.label.feed', 'feed'),
  ...(plunge ? [num('plungeFeed', label, 'feed')] : []),
  num('rpm', 'operations.label.spindleSpeed', 'rpm'),
  num('clearance', 'operations.label.clearance', 'length'),
]

/** Millimetre and inch defaults side by side. */
const pick = (unit: Unit, mm: Record<string, number | string>, inch: Record<string, number | string>): Values =>
  Object.fromEntries(Object.entries(unit === 'inch' ? inch : mm).map(([key, value]) => [key, String(value)]))

const speeds = { mm: { feed: 600, plungeFeed: 200, rpm: 8000, clearance: 5 }, inch: { feed: 24, plungeFeed: 8, rpm: 8000, clearance: 0.2 } }

const size = (value: number, unit: Unit): string => `${number(value, unit)}${unitName(unit)}`

const grid = (values: Values): boolean => values['pattern'] === 'grid'
const circle = (values: Values): boolean => values['pattern'] === 'circle'
const slotClosed = (values: Values): boolean => values['ends'] === 'closed'
const rectangularProfile = (values: Values): boolean => values['shape'] === 'rectangle'
const circularProfile = (values: Values): boolean => !rectangularProfile(values)

const inclinedEntry = (values: Values): boolean => values['entry'] === 'helix' || values['entry'] === 'ramp'
const pocketNote = (values: Values): OperationsKey<'operation'> =>
  inclinedEntry(values) ? 'operations.operation.inclinedEntry' : 'operations.operation.centreCutting'
const pocketDefaults = { entry: 'plunge', entryAngle: 3, walls: 'size', coolant: 'off' }
const pocketFields: Field[] = [
  { type: 'choice', key: 'entry', label: 'operations.label.pocketEntry', select: true,
    options: [['plunge', 'operations.option.plunge'], ['helix', 'operations.option.helix'], ['ramp', 'operations.option.ramp']] },
  num('entryAngle', 'operations.label.entryAngle', 'angle', inclinedEntry),
  { type: 'choice', key: 'walls', label: 'operations.label.pocketWalls', select: true,
    options: [['size', 'operations.option.toSize'], ['leave', 'operations.option.leaveStock'], ['finish', 'operations.option.finishWalls']] },
  num('wallStock', 'operations.label.wallStock', 'length', (values) => values['walls'] === 'leave' || values['walls'] === 'finish'),
  { type: 'choice', key: 'coolant', label: 'operations.label.pocketCoolant',
    options: [['off', 'operations.option.coolantOff'], ['flood', 'operations.option.coolantFlood'],
      ['mist', 'operations.option.coolantMist'], ['both', 'operations.option.coolantBoth']] },
]

function pocketOptions(v: Parsed): PocketOptions {
  const entry = v.text('entry')
  const coolant = v.text('coolant')
  const walls = v.text('walls')
  return {
    entry: entry === 'helix' || entry === 'ramp' ? entry : 'plunge',
    entryAngle: v.num('entryAngle'),
    wallStock: walls === 'leave' || walls === 'finish' ? v.num('wallStock') : 0,
    finish: walls === 'finish',
    coolant: coolant === 'flood' || coolant === 'mist' || coolant === 'both' ? coolant : 'off',
  }
}

const holePattern: Field = { type: 'choice', key: 'pattern', label: 'operations.label.pattern', options: [['grid', 'operations.option.grid'], ['circle', 'operations.option.boltCircle']] }

const holeLocations: Field[] = [
  num('x', 'operations.label.firstHoleX', 'coordinate', grid), num('y', 'operations.label.firstHoleY', 'coordinate', grid),
  num('x', 'operations.label.centreX', 'coordinate', circle), num('y', 'operations.label.centreY', 'coordinate', circle),
  num('columns', 'operations.label.columns', 'count', grid), num('rows', 'operations.label.rows', 'count', grid),
  num('spacingX', 'operations.label.columnSpacingX', 'coordinate', grid), num('spacingY', 'operations.label.rowSpacingY', 'coordinate', grid),
  num('count', 'operations.label.holes', 'count', circle), num('radius', 'operations.label.radius', 'length', circle),
  num('startAngle', 'operations.label.firstHoleAngle', 'angle', circle),
  num('depth', 'operations.label.holeDepth', 'length'),
]

const retractHeight = num('retract', 'operations.label.retractHeightR', 'length')

type HoleGeometry = Omit<Drilling, keyof Common | 'cycle' | 'peck' | 'dwell'>
function holeGeometry(v: Parsed): HoleGeometry {
  return { pattern: v.text('pattern') === 'circle' ? 'circle' : 'grid',
    x: v.num('x'), y: v.num('y'), columns: v.num('columns'), rows: v.num('rows'),
    spacingX: v.num('spacingX'), spacingY: v.num('spacingY'), count: v.num('count'),
    radius: v.num('radius'), startAngle: v.num('startAngle'), depth: v.num('depth'), retract: v.num('retract') }
}

const holeDefaults = {
  mm: { pattern: 'grid', x: 10, y: 10, columns: 3, rows: 2, spacingX: 20, spacingY: 20, count: 6, radius: 25,
    startAngle: 0, depth: 10, peck: 2, retract: 2 },
  inch: { pattern: 'grid', x: 0.5, y: 0.5, columns: 3, rows: 2, spacingX: 1, spacingY: 1, count: 6, radius: 1,
    startAngle: 0, depth: 0.4, peck: 0.08, retract: 0.08 },
}

export const operations: readonly Operation[] = [
  {
    id: 'facing',
    title: 'operations.operation.facing',
    fields: [
      num('width', 'operations.label.sizeX', 'length'), num('height', 'operations.label.sizeY', 'length'),
      { type: 'choice', key: 'origin', label: 'operations.label.origin', options: origins },
      { type: 'choice', key: 'along', label: 'operations.label.along', options: [['x', 'operations.option.x'], ['y', 'operations.option.y']] },
      num('stepover', 'operations.label.stepover', 'percent'),
      num('depth', 'operations.label.totalDepth', 'length'), num('depthPerPass', 'operations.label.depthPerPass', 'length'),
      ...feeds(true),
    ],
    defaults: (unit) => pick(unit,
      { width: 100, height: 60, origin: 'lower-left', along: 'x', stepover: 70, depth: 1, depthPerPass: 0.5, ...speeds.mm },
      { width: 4, height: 2.5, origin: 'lower-left', along: 'x', stepover: 70, depth: 0.04, depthPerPass: 0.02, ...speeds.inch }),
    generate: (v, common) => facing({ ...common, width: v.num('width'), height: v.num('height'),
      origin: v.text('origin') as Origin, along: v.text('along') === 'y' ? 'y' : 'x', stepover: v.num('stepover'),
      depth: v.num('depth'), depthPerPass: v.num('depthPerPass'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `facing-${number(v.num('width'), unit)}x${size(v.num('height'), unit)}`, // i18n-ignore
  },
  {
    id: 'drilling',
    title: 'operations.operation.drilling',
    fields: [
      holePattern,
      { type: 'choice', key: 'cycle', label: 'operations.label.cycle', options: [['G81', 'operations.option.g81'], ['G83', 'operations.option.g83']] },
      ...holeLocations,
      num('peck', 'operations.label.peckDepth', 'length', (values) => values['cycle'] === 'G83'),
      retractHeight,
      ...feeds(false),
    ],
    defaults: (unit) => pick(unit,
      { ...holeDefaults.mm, cycle: 'G81', ...speeds.mm, feed: 100 },
      { ...holeDefaults.inch, cycle: 'G81', ...speeds.inch, feed: 4 }),
    generate: (v, common) => drilling({ ...common, ...holeGeometry(v),
      cycle: v.text('cycle') === 'G83' ? 'G83' : 'G81', peck: v.num('peck') }),
    name: (v) => v.text('pattern') === 'circle' ? `bolt-circle-${String(v.num('count'))}-holes` // i18n-ignore
      : `drill-grid-${String(v.num('columns'))}x${String(v.num('rows'))}`, // i18n-ignore
  },
  {
    id: 'hole-finishing',
    title: 'operations.operation.holeFinishing',
    note: () => 'operations.operation.holeFinishingSetup',
    fields: [
      holePattern,
      { type: 'choice', key: 'cycle', label: 'operations.label.cycle',
        options: [['G85', 'operations.option.g85'], ['G89', 'operations.option.g89']] },
      ...holeLocations,
      num('dwell', 'operations.label.boreDwell', 'duration', (values) => values['cycle'] === 'G89'),
      retractHeight,
      ...feeds(false),
    ],
    defaults: (unit) => pick(unit,
      { ...holeDefaults.mm, cycle: 'G85', dwell: 0.5, ...speeds.mm, rpm: 500, feed: 60 },
      { ...holeDefaults.inch, cycle: 'G85', dwell: 0.5, ...speeds.inch, rpm: 500, feed: 2.4 }),
    generate: (v, common) => drilling({ ...common, ...holeGeometry(v),
      cycle: v.text('cycle') === 'G89' ? 'G89' : 'G85', peck: NaN, dwell: v.num('dwell') }),
    name: (v): string => {
      const operation = v.text('cycle') === 'G89' ? 'bore' : 'ream' // i18n-ignore
      return v.text('pattern') === 'circle' ? `${operation}-circle-${String(v.num('count'))}-holes` // i18n-ignore
        : `${operation}-grid-${String(v.num('columns'))}x${String(v.num('rows'))}` // i18n-ignore
    },
  },
  {
    id: 'rigid-tap',
    title: 'operations.operation.rigidTap',
    note: () => 'operations.operation.rigidTapSetup',
    fields: [
      holePattern, ...holeLocations,
      num('pitch', 'operations.label.tapPitch', 'length'),
      num('pilotDepth', 'operations.label.tapPilotDepth', 'length'),
      num('retract', 'operations.label.tapStartHeight', 'length'),
      num('rpm', 'operations.label.spindleSpeed', 'rpm'),
      num('clearance', 'operations.label.clearance', 'length'),
    ],
    defaults: (unit) => pick(unit,
      { ...holeDefaults.mm, columns: 1, rows: 1, x: 0, y: 0, pitch: 1, pilotDepth: 15, rpm: 300, clearance: 5 },
      { ...holeDefaults.inch, columns: 1, rows: 1, x: 0, y: 0, pitch: '1/20', pilotDepth: 0.6, rpm: 300, clearance: 0.2 }),
    generate: (v, common) => rigidTapping({ ...common, ...holeGeometry(v), pitch: v.num('pitch'), pilotDepth: v.num('pilotDepth') }),
    name: (v) => v.text('pattern') === 'circle' ? `tap-circle-${String(v.num('count'))}-holes` // i18n-ignore
      : `tap-grid-${String(v.num('columns'))}x${String(v.num('rows'))}`, // i18n-ignore
  },
  {
    id: 'bore',
    title: 'operations.operation.bore',
    fields: [
      num('x', 'operations.label.centreX', 'coordinate'), num('y', 'operations.label.centreY', 'coordinate'),
      num('diameter', 'operations.label.boreDiameter', 'length'), num('depth', 'operations.label.depth', 'length'),
      num('pitch', 'operations.label.depthPerTurn', 'length'),
      { type: 'choice', key: 'finish', label: 'operations.label.finishPass', options: yesNo },
      num('finishAllowance', 'operations.label.finishAllowance', 'length', (values) => values['finish'] === 'yes'),
      ...feeds(true),
    ],
    defaults: (unit) => pick(unit,
      { x: 0, y: 0, diameter: 10, depth: 10, pitch: 1, finish: 'yes', finishAllowance: 0.2, ...speeds.mm },
      { x: 0, y: 0, diameter: 0.375, depth: 0.4, pitch: 0.04, finish: 'yes', finishAllowance: 0.008, ...speeds.inch }),
    generate: (v, common) => bore({ ...common, x: v.num('x'), y: v.num('y'), diameter: v.num('diameter'),
      depth: v.num('depth'), pitch: v.num('pitch'), finish: v.text('finish') === 'yes',
      finishAllowance: v.num('finishAllowance'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `bore-${size(v.num('diameter'), unit)}`, // i18n-ignore
  },
  {
    id: 'circular-pocket',
    title: 'operations.operation.circularPocket',
    note: pocketNote,
    fields: [
      num('x', 'operations.label.centreX', 'coordinate'), num('y', 'operations.label.centreY', 'coordinate'),
      num('diameter', 'operations.label.pocketDiameter', 'length'), num('stepover', 'operations.label.stepover', 'percent'),
      num('depth', 'operations.label.depth', 'length'), num('depthPerPass', 'operations.label.depthPerPass', 'length'),
      ...feeds(true, 'operations.label.entryFeed'), ...pocketFields,
    ],
    defaults: (unit) => pick(unit,
      { x: 0, y: 0, diameter: 30, stepover: 40, depth: 5, depthPerPass: 1, wallStock: 0.2, ...pocketDefaults, ...speeds.mm },
      { x: 0, y: 0, diameter: 1.25, stepover: 40, depth: 0.2, depthPerPass: 0.04, wallStock: 0.008, ...pocketDefaults, ...speeds.inch }),
    generate: (v, common) => circularPocket({ ...common, ...pocketOptions(v), x: v.num('x'), y: v.num('y'), diameter: v.num('diameter'),
      stepover: v.num('stepover'), depth: v.num('depth'), depthPerPass: v.num('depthPerPass'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `circular-pocket-${size(v.num('diameter'), unit)}`, // i18n-ignore
  },
  {
    id: 'rectangular-pocket',
    title: 'operations.operation.rectangularPocket',
    note: pocketNote,
    fields: [
      num('x', 'operations.label.centreX', 'coordinate'), num('y', 'operations.label.centreY', 'coordinate'),
      num('width', 'operations.label.sizeX', 'length'), num('height', 'operations.label.sizeY', 'length'),
      num('cornerRadius', 'operations.label.cornerRadius', 'length'), num('stepover', 'operations.label.stepover', 'percent'),
      num('depth', 'operations.label.depth', 'length'), num('depthPerPass', 'operations.label.depthPerPass', 'length'),
      ...feeds(true, 'operations.label.entryFeed'), ...pocketFields,
    ],
    defaults: (unit) => pick(unit,
      { x: 0, y: 0, width: 40, height: 30, cornerRadius: 0, stepover: 40, depth: 5, depthPerPass: 1, wallStock: 0.2, ...pocketDefaults, ...speeds.mm },
      { x: 0, y: 0, width: 1.5, height: 1.25, cornerRadius: 0, stepover: 40, depth: 0.2, depthPerPass: 0.04, wallStock: 0.008, ...pocketDefaults, ...speeds.inch }),
    generate: (v, common) => rectangularPocket({ ...common, ...pocketOptions(v), x: v.num('x'), y: v.num('y'), width: v.num('width'),
      height: v.num('height'), cornerRadius: v.num('cornerRadius'), stepover: v.num('stepover'), depth: v.num('depth'),
      depthPerPass: v.num('depthPerPass'), plungeFeed: v.num('plungeFeed') }),
    name: (v, unit) => `rectangular-pocket-${number(v.num('width'), unit)}x${size(v.num('height'), unit)}`, // i18n-ignore
  },
  {
    id: 'contour',
    title: 'operations.operation.contour',
    note: () => 'operations.operation.contourSetup',
    fields: [
      { type: 'choice', key: 'shape', label: 'operations.label.profileShape',
        options: [['circle', 'operations.option.profileCircle'], ['rectangle', 'operations.option.profileRectangle']] },
      { type: 'choice', key: 'side', label: 'operations.label.profileSide',
        options: [['outside', 'operations.option.profileOutside'], ['inside', 'operations.option.profileInside']] },
      { type: 'choice', key: 'direction', label: 'operations.label.profileDirection', select: true,
        options: [['cw', 'operations.option.clockwise'], ['ccw', 'operations.option.counterclockwise']] },
      num('x', 'operations.label.centreX', 'coordinate'), num('y', 'operations.label.centreY', 'coordinate'),
      num('diameter', 'operations.label.profileDiameter', 'length', circularProfile),
      num('width', 'operations.label.sizeX', 'length', rectangularProfile),
      num('height', 'operations.label.sizeY', 'length', rectangularProfile),
      num('corner', 'operations.label.cornerRadius', 'length', rectangularProfile),
      num('depth', 'operations.label.depth', 'length'),
      num('depthPerPass', 'operations.label.depthPerPass', 'length'),
      ...feeds(true),
    ],
    defaults: (unit) => pick(unit,
      { shape: 'circle', side: 'outside', direction: 'cw', x: 0, y: 0, diameter: 40, width: 50, height: 40,
        corner: 5, depth: 3, depthPerPass: 1, ...speeds.mm },
      { shape: 'circle', side: 'outside', direction: 'cw', x: 0, y: 0, diameter: 1.5, width: 2, height: 1.5,
        corner: 0.2, depth: 0.12, depthPerPass: 0.04, ...speeds.inch }),
    generate: (v, common) => contour({ ...common, x: v.num('x'), y: v.num('y'),
      inside: v.text('side') === 'inside', clockwise: v.text('direction') === 'cw',
      depth: v.num('depth'), depthPerPass: v.num('depthPerPass'), plungeFeed: v.num('plungeFeed'),
      ...(v.text('shape') === 'rectangle'
        ? { shape: 'rectangle', width: v.num('width'), height: v.num('height'), corner: v.num('corner') }
        : { shape: 'circle', diameter: v.num('diameter') }) }),
    name: (v, unit) => v.text('shape') === 'rectangle'
      ? `contour-${number(v.num('width'), unit)}x${size(v.num('height'), unit)}` // i18n-ignore
      : `contour-${size(v.num('diameter'), unit)}`, // i18n-ignore
  },
  {
    id: 'slot',
    title: 'operations.operation.slot',
    note: () => 'operations.operation.slotSetup',
    fields: [
      { type: 'choice', key: 'along', label: 'operations.label.slotAxis',
        options: [['x', 'operations.option.x'], ['y', 'operations.option.y']] },
      { type: 'choice', key: 'ends', label: 'operations.label.slotEnds',
        options: [['closed', 'operations.option.slotClosed'], ['low', 'operations.option.slotLow'],
          ['high', 'operations.option.slotHigh'], ['both', 'operations.option.slotBoth']] },
      num('x', 'operations.label.centreX', 'coordinate'), num('y', 'operations.label.centreY', 'coordinate'),
      num('length', 'operations.label.slotLength', 'length'), num('width', 'operations.label.slotWidth', 'length'),
      num('depth', 'operations.label.depth', 'length'), num('depthPerPass', 'operations.label.depthPerPass', 'length'),
      { type: 'choice', key: 'slotEntry', label: 'operations.label.slotEntry',
        options: [['ramp', 'operations.option.slotRamp'], ['plunge', 'operations.option.slotPlunge']], when: slotClosed },
      num('angle', 'operations.label.slotAngle', 'angle', (v) => slotClosed(v) && v['slotEntry'] === 'ramp'),
      ...feeds(false), num('plungeFeed', 'operations.label.slotEntryFeed', 'feed'),
    ],
    defaults: (unit) => pick(unit,
      { along: 'x', ends: 'closed', x: 0, y: 0, length: 40, width: 10, depth: 3, depthPerPass: 1,
        slotEntry: 'ramp', angle: 3, ...speeds.mm, feed: 300, plungeFeed: 100 },
      { along: 'x', ends: 'closed', x: 0, y: 0, length: 1.5, width: 0.375, depth: 0.12, depthPerPass: 0.04,
        slotEntry: 'ramp', angle: 3, ...speeds.inch, feed: 12, plungeFeed: 4 }),
    generate: (v, common) => slot({ ...common, x: v.num('x'), y: v.num('y'),
      along: v.text('along') === 'y' ? 'y' : 'x', ends: v.text('ends') as SlotEnds,
      length: v.num('length'), width: v.num('width'), depth: v.num('depth'), depthPerPass: v.num('depthPerPass'),
      plunge: v.text('slotEntry') === 'plunge', angle: v.num('angle'), entryFeed: v.num('plungeFeed') }),
    name: (v, unit) => `slot-${v.text('along')}-${number(v.num('length'), unit)}x${size(v.num('width'), unit)}`, // i18n-ignore
  },
  {
    id: 'thread-mill',
    title: 'operations.operation.threadMill',
    note: () => 'operations.operation.threadMillSetup',
    fields: [
      num('x', 'operations.label.centreX', 'coordinate'), num('y', 'operations.label.centreY', 'coordinate'),
      num('major', 'operations.label.threadMajor', 'length'), num('pitch', 'operations.label.threadPitch', 'length'),
      num('pilot', 'operations.label.threadPilot', 'length'), num('pilotDepth', 'operations.label.threadPilotDepth', 'length'),
      num('depth', 'operations.label.threadDepth', 'length'), num('passes', 'operations.label.threadPasses', 'count'),
      { type: 'choice', key: 'hand', label: 'operations.label.threadHand',
        options: [['right', 'operations.option.threadRight'], ['left', 'operations.option.threadLeft']] },
      ...feeds(true, 'operations.label.entryFeed'),
    ],
    defaults: (unit) => pick(unit,
      { x: 0, y: 0, major: 10, pitch: 1.5, pilot: 8.5, pilotDepth: 13, depth: 9, hand: 'right', passes: 2,
        ...speeds.mm, feed: 60, plungeFeed: 50 },
      { x: 0, y: 0, major: 0.375, pitch: '1/16', pilot: 0.3125, pilotDepth: 0.55, depth: 0.375, hand: 'right', passes: 2,
        ...speeds.inch, feed: 2.4, plungeFeed: 2 }),
    generate: (v, common) => threadMill({ ...common, x: v.num('x'), y: v.num('y'),
      majorDiameter: v.num('major'), pitch: v.num('pitch'), pilotDiameter: v.num('pilot'), pilotDepth: v.num('pilotDepth'),
      depth: v.num('depth'), passes: v.num('passes'), leftHand: v.text('hand') === 'left', entryFeed: v.num('plungeFeed') }),
    name: (v, unit) => `thread-${size(v.num('major'), unit)}-${v.text('hand') === 'left' ? 'lh' : 'rh'}`, // i18n-ignore
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

/** `base.ngc`, or `base-2.ngc` and so on when that name is taken. */ // i18n-ignore
export function uniqueName(base: string, taken: ReadonlySet<string>): string {
  const clean = base.replace(/[^A-Za-z0-9._-]+/g, '-')
  for (let n = 1; ; n += 1) {
    const name = n === 1 ? `${clean}.ngc` : `${clean}-${String(n)}.ngc` // i18n-ignore
    if (!taken.has(name)) return name
  }
}
