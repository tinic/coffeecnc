<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage, translateResult } from '../i18n/messages'
import { allHomed, homingActive, manualBlock, onResult, panel, send } from '../machine'
import { homingMessage } from '../machine-block'
import type { CommandBody, ProbeDirection, SetterMeasured, SetterState } from '../protocol'
import { setterState, touchedOff, type SetterStatusLine } from '../setter-state'
import { formatLength, wcsName } from '../units'
import MeasuredResult from './MeasuredResult.vue'
import SetterStatusBand from './SetterStatusBand.vue'
import WorkpieceProbeDiagram from './WorkpieceProbeDiagram.vue'
import ProbeGuideDialog from './ProbeGuideDialog.vue'
import type { AngleEdge, MaterialGuideTarget } from '../probe-guide'
import type { Angles } from '../arc'
import ModalDialog from './ModalDialog.vue'
import { vTooltip } from '../tooltip'
import HeightMapDialog from './HeightMapDialog.vue'
import HeightMapResult from './HeightMapResult.vue'
import { heightGrid, heightMap, heightMapPointAt, heightRange, measuredPoints, type HeightMapGrid } from '../height-map'
import type { HeightMap } from '../protocol'

const props = defineProps<{ tool: number }>()
const { t } = useI18n()
type Request = Extract<CommandBody, { type: 'fixed_setter'; action: 'material' | 'corner' | 'ball' | 'bore' | 'boss' | 'bore_arc' | 'boss_arc' | 'angle' | 'apply_angle' | 'apply_measurement' | 'inside_corner' | 'pocket' | 'slot' | 'ridge' | 'height_map' }>
// The button names the part face. The protocol still specifies the tool's travel.
const surfaces = [
  { key: 'zTop', label: null, travel: 'z_top', approach: '−Z' },
  { key: 'left', label: 'X−', travel: 'x_plus', approach: '+X' },
  { key: 'right', label: 'X+', travel: 'x_minus', approach: '−X' },
  { key: 'front', label: 'Y−', travel: 'y_plus', approach: '+Y' },
  { key: 'back', label: 'Y+', travel: 'y_minus', approach: '−Y' },
] as const
const surfaceText = {
  zTop: { tip: 'probe.materialProbe.surfaceTip.zTop', busy: 'probe.materialProbe.surfaceBusy.zTop', done: 'probe.materialProbe.surfaceDone.zTop' },
  left: { tip: 'probe.materialProbe.surfaceTip.left', busy: 'probe.materialProbe.surfaceBusy.left', done: 'probe.materialProbe.surfaceDone.left' },
  right: { tip: 'probe.materialProbe.surfaceTip.right', busy: 'probe.materialProbe.surfaceBusy.right', done: 'probe.materialProbe.surfaceDone.right' },
  front: { tip: 'probe.materialProbe.surfaceTip.front', busy: 'probe.materialProbe.surfaceBusy.front', done: 'probe.materialProbe.surfaceDone.front' },
  back: { tip: 'probe.materialProbe.surfaceTip.back', busy: 'probe.materialProbe.surfaceBusy.back', done: 'probe.materialProbe.surfaceDone.back' },
} as const
const surfaceOf = (direction: ProbeDirection): typeof surfaces[number] =>
  surfaces.find((entry) => entry.travel === direction) ?? surfaces[0]
// A corner probes the two faces that meet there; its name follows the edge buttons.
const corners = [
  { label: 'X− Y−', key: 'frontLeft', x: 'x_plus', y: 'y_plus' },
  { label: 'X+ Y−', key: 'frontRight', x: 'x_minus', y: 'y_plus' },
  { label: 'X− Y+', key: 'backLeft', x: 'x_plus', y: 'y_minus' },
  { label: 'X+ Y+', key: 'backRight', x: 'x_minus', y: 'y_minus' },
] as const
const cornerText = {
  frontLeft: { tip: 'probe.materialProbe.cornerTip.frontLeft', busy: 'probe.materialProbe.cornerBusy.frontLeft', done: 'probe.materialProbe.cornerDone.frontLeft' },
  frontRight: { tip: 'probe.materialProbe.cornerTip.frontRight', busy: 'probe.materialProbe.cornerBusy.frontRight', done: 'probe.materialProbe.cornerDone.frontRight' },
  backLeft: { tip: 'probe.materialProbe.cornerTip.backLeft', busy: 'probe.materialProbe.cornerBusy.backLeft', done: 'probe.materialProbe.cornerDone.backLeft' },
  backRight: { tip: 'probe.materialProbe.cornerTip.backRight', busy: 'probe.materialProbe.cornerBusy.backRight', done: 'probe.materialProbe.cornerDone.backRight' },
} as const
const cornerOf = (x: string, y: string): typeof corners[number] =>
  corners.find((entry) => entry.x === x && entry.y === y) ?? corners[0]
const insideCorners = [
  { label: 'X− Y−', x: 'x_minus', y: 'y_minus' },
  { label: 'X+ Y−', x: 'x_plus', y: 'y_minus' },
  { label: 'X− Y+', x: 'x_minus', y: 'y_plus' },
  { label: 'X+ Y+', x: 'x_plus', y: 'y_plus' },
] as const
const featureAxes = ['x', 'y'] as const
const arcKinds = ['bore_arc', 'boss_arc'] as const
const narrowFeatures = ['slot', 'ridge'] as const

/** A status line whose text is made when it is shown, so it follows a language change. */
interface Line { text: () => string; level: SetterStatusLine['level'] }
const pending = shallowRef<Request | null>(null)
const pendingCorner = computed(() => pending.value?.action === 'corner' ? [pending.value.x_direction, pending.value.y_direction] : null)
const hint: Line = { text: (): string => '', level: 'info' }
const line = shallowRef<Line>(hint)
const cycleFailed = shallowRef(false)
const toolBlock = computed(() => panel.link === 'live' && panel.status !== null &&
  (panel.status.tool.in_spindle !== props.tool || panel.status.wait.kind === 'tool_change')
  ? t('probe.materialProbe.loadTool', { tool: String(props.tool) }) : null)
const displayLine = computed((): SetterStatusLine => line.value === hint && toolBlock.value !== null
  ? { text: toolBlock.value, level: 'info' } : { text: line.value.text(), level: line.value.level })
const machineReason = computed(() => manualBlock.value ?? (homingActive.value ? homingMessage() : null))
const spindleStopped = computed(() => {
  const spindle = panel.status?.spindle
  return spindle?.direction === 'stopped' && typeof spindle.actual_rpm === 'number' &&
    Math.abs(spindle.actual_rpm) < 1
})
watch([
  machineReason, toolBlock, spindleStopped, (): boolean | undefined => panel.status?.motion.in_position,
  (): boolean | null | undefined => panel.status?.probe_input, (): boolean => touchedOff(props.tool),
], () => {
  if (!cycleFailed.value && line.value.level === 'blocked') line.value = hint
})
// Motion can return to idle in the same update as a failed probe result.
watch([toolBlock, (): string => panel.link, (): string | undefined => panel.status?.machine], () => {
  if (cycleFailed.value) line.value = hint
  cycleFailed.value = false
})
let statusRequest: CommandBody | null = null

const guide = shallowRef<MaterialGuideTarget | null>(null)
const choosingFeature = shallowRef(false)
const extendedFeatures = computed(() => panel.hello?.fixed_setter?.material_features === true)
const featureActive = computed(() => {
  const action = pending.value?.action
  return choosingFeature.value || action !== undefined && !['material', 'ball', 'apply_angle', 'apply_measurement'].includes(action)
})
/** Probe actions report what they find and set nothing (#247); the ball calibration always writes. */
const measureOnly = shallowRef(false)
/** The last measure-only result: shown in its card, and applied from there after a review. */
const measuredResult = shallowRef<{ setter: SetterState; measured: SetterMeasured; frame: string } | null>(null)
const showingResult = shallowRef(false)
function openGuide(target: MaterialGuideTarget): void {
  if (pending.value === null) {
    choosingFeature.value = false
    guide.value = target
  }
}
function startGuide(size: number, angle: AngleEdge, angles: Angles): void {
  const target = guide.value
  guide.value = null
  if (target !== null) probe(target, size, angle, angles)
}
/** A Z height map (#247): its grid dialog, and its last result for the card and the 3D view. */
const choosingMap = shallowRef(false)
const mapResult = shallowRef<HeightMap | null>(null)
const showingMap = shallowRef(false)
/** The running map's grid, and the last of its points (1-based, probing order) the probe was over. */
const mapGrid = computed(() => pending.value?.action === 'height_map' ? pending.value : null)
const mapTotal = computed(() => {
  const grid = mapGrid.value
  const size = grid === null ? null : heightGrid([grid.x_min, grid.y_min, grid.x_max, grid.y_max], grid.step)
  return size === null ? 0 : size.columns * size.rows
})
const mapPoint = shallowRef(1)
watch((): [number | undefined, number | undefined] => [
  panel.status?.axes.find((entry) => entry.axis === 'x')?.work, panel.status?.axes.find((entry) => entry.axis === 'y')?.work,
], ([x, y]) => {
  const grid = mapGrid.value
  if (grid === null || x === undefined || y === undefined) return
  const point = heightMapPointAt(grid, x, y)
  if (point !== null && point > mapPoint.value) mapPoint.value = point
})
/** Stops the running map: the same abort as the panel's other stops. Points measured so far stay. */
function stopMap(): void {
  if (pending.value?.action === 'height_map') send({ type: 'abort' })
}
function openMap(): void {
  if (pending.value !== null) return
  choosingFeature.value = false
  choosingMap.value = true
}
function startMap(grid: HeightMapGrid): void {
  choosingMap.value = false
  const status = panel.status
  const [x, y, z] = ['x', 'y', 'z'].map((axis) => status?.axes.find((entry) => entry.axis === axis)?.machine)
  const machine = machineReason.value
  let reason: (() => string) | null = machine === null ? null : (): string => translateMessage(machine)
  if (reason === null && (status?.tool.in_spindle !== props.tool || status.wait.kind === 'tool_change'))
    reason = (): string => t('probe.materialProbe.loadTool', { tool: String(props.tool) })
  if (reason === null && !spindleStopped.value) reason = (): string => t('probe.materialProbe.waitSpindle')
  if (reason === null && !status?.motion.in_position) reason = (): string => t('probe.materialProbe.waitAxes')
  if (reason === null && !touchedOff(props.tool)) reason = (): string => t('probe.materialProbe.touchOffZTop', { tool: `T${String(props.tool)}` })
  if (reason !== null || x === undefined || y === undefined || z === undefined || ![x, y, z].every(Number.isFinite)) {
    line.value = { text: reason ?? ((): string => t('probe.materialProbe.waitPosition')), level: 'blocked' }
    return
  }
  edgeRotation.value = null
  mapResult.value = null
  const request = { type: 'fixed_setter', action: 'height_map', tool: props.tool, x, y, z, ...grid } as const
  pending.value = request
  mapPoint.value = 1
  line.value = { text: (): string => t('probe.heightMap.progress', { point: mapPoint.value, total: mapTotal.value }), level: 'busy' }
  send(request)
}
const askingRotation = shallowRef(false)
/** A measured edge: the turn from the present work axes, and the rotation applying it writes. */
const edgeRotation = shallowRef<{ id: number; wcs: number; degrees: number; current: number; rotation: number; frame: string } | null>(null)
const frame = computed(() => JSON.stringify(panel.status?.offsets))
const shifted = computed(() => panel.status?.offsets.g92.some((value) => value !== 0) === true)
const canApplyRotation = computed(() => edgeRotation.value !== null && frame.value === edgeRotation.value.frame &&
  panel.status?.offsets.wcs === edgeRotation.value.wcs && !pending.value && !machineReason.value && !toolBlock.value)
watch(frame, () => {
  edgeRotation.value = null
  askingRotation.value = false
})
const canApplyMeasured = computed(() => measuredResult.value?.measured.measurement_id !== undefined &&
  frame.value === measuredResult.value.frame && panel.status?.offsets.wcs === measuredResult.value.measured.wcs &&
  !pending.value && !machineReason.value && !toolBlock.value)
const axesText = (axes: string): string => axes.split('').map((axis) => `${axis}0`).join(' ')
function applyMeasured(): void {
  const result = measuredResult.value
  const id = result?.measured.measurement_id
  if (!canApplyMeasured.value || !result || id === undefined) return
  showingResult.value = false
  const request = { type: 'fixed_setter', action: 'apply_measurement', measurement_id: id, tool: props.tool } as const
  pending.value = request
  const values = { wcs: wcsName(result.measured.wcs), axes: axesText(result.measured.axes) }
  line.value = { text: (): string => t('probe.materialProbe.settingMeasured', values), level: 'busy' }
  send(request)
}
const degreesText = (degrees: number): string => `${degrees >= 0 ? '+' : '−'}${Math.abs(degrees).toFixed(3)}°`
function applyRotation(): void {
  const offer = edgeRotation.value
  if (!canApplyRotation.value || !offer) return
  askingRotation.value = false
  const request = { type: 'fixed_setter', action: 'apply_angle', measurement_id: offer.id, tool: props.tool } as const
  pending.value = request
  line.value = { text: (): string => t('probe.materialProbe.settingRotation', { wcs: wcsName(offer.wcs) }), level: 'busy' }
  send(request)
}

function probe(target: MaterialGuideTarget, size: number, angle: AngleEdge, angles: Angles): void {
  if (pending.value) return
  cycleFailed.value = false
  edgeRotation.value = null
  askingRotation.value = false
  measuredResult.value = null
  showingResult.value = false
  const status = panel.status
  const [x, y, z] = ['x', 'y', 'z'].map((axis) => status?.axes.find((entry) => entry.axis === axis)?.machine)
  const machine = machineReason.value
  let reason: (() => string) | null = machine === null ? null : (): string => translateMessage(machine)
  if (reason === null && (status?.tool.in_spindle !== props.tool || status.wait.kind === 'tool_change'))
    reason = (): string => t('probe.materialProbe.loadTool', { tool: String(props.tool) })
  if (reason === null && !spindleStopped.value)
    reason = (): string => t('probe.materialProbe.waitSpindle')
  if (reason === null && !status?.motion.in_position) reason = (): string => t('probe.materialProbe.waitAxes')
  if (reason === null && !touchedOff(props.tool)) {
    const tool = `T${String(props.tool)}`
    if (target.kind === 'surface' && target.direction === 'z_top')
      reason = (): string => t('probe.materialProbe.touchOffZTop', { tool })
    if (target.kind === 'corner')
      reason = (): string => t('probe.materialProbe.touchOffCorner', { tool })
    if (target.kind === 'boss' || target.kind === 'boss_arc') reason = (): string => t('probe.materialProbe.touchOffBoss', { tool })
    if (target.kind === 'ridge') reason = (): string => t('probe.materialProbe.touchOffRidge', { tool })
    if (target.kind === 'ball') reason = (): string => t('probe.materialProbe.touchOffGauge', { tool })
  }
  if (reason !== null || x === undefined || y === undefined || z === undefined || ![x, y, z].every(Number.isFinite)) {
    line.value = { text: reason ?? ((): string => t('probe.materialProbe.waitPosition')), level: 'blocked' }
    return
  }
  const position = { type: 'fixed_setter', tool: props.tool, x, y, z } as const
  let request: Request
  let busy: () => string
  switch (target.kind) {
    case 'surface': {
      request = { ...position, action: 'material', direction: target.direction }
      const key = surfaceText[surfaceOf(target.direction).key].busy
      busy = (): string => t(key)
      break
    }
    case 'corner':
    case 'inside_corner': {
      request = { ...position, action: target.kind, x_direction: target.x, y_direction: target.y }
      const key = target.kind === 'inside_corner' ? 'probe.materialProbe.insideCornerBusy'
        : cornerText[cornerOf(target.x, target.y).key].busy
      busy = (): string => t(key)
      break
    }
    case 'angle':
      request = { ...position, action: 'angle', direction: angle, spacing: size }
      busy = angle === 'y_plus' ? (): string => t('probe.materialProbe.angleFrontBusy') : (): string => t('probe.materialProbe.angleLeftBusy')
      break
    case 'ball':
      request = { ...position, action: 'ball', gauge_width: size }
      busy = (): string => t('probe.materialProbe.ballBusy')
      break
    case 'boss':
      request = { ...position, action: 'boss', diameter: size }
      busy = (): string => t('probe.materialProbe.bossBusy')
      break
    case 'bore_arc':
      request = { ...position, action: 'bore_arc', angles: [...angles] }
      busy = (): string => t('probe.materialProbe.arcBoreBusy')
      break
    case 'boss_arc':
      request = { ...position, action: 'boss_arc', angles: [...angles], diameter: size }
      busy = (): string => t('probe.materialProbe.arcBossBusy')
      break
    case 'bore':
    case 'pocket':
      request = { ...position, action: target.kind }
      busy = target.kind === 'bore' ? (): string => t('probe.materialProbe.boreBusy') : (): string => t('probe.materialProbe.pocketBusy')
      break
    case 'slot': {
      request = { ...position, action: 'slot', axis: target.axis }
      const axis = target.axis.toUpperCase()
      busy = (): string => t('probe.materialProbe.slotBusy', { axis })
      break
    }
    case 'ridge': {
      request = { ...position, action: 'ridge', axis: target.axis, width: size }
      const axis = target.axis.toUpperCase()
      busy = (): string => t('probe.materialProbe.ridgeBusy', { axis })
      break
    }
  }
  // Only the actions the controller can run measure-only: a later action writes unless it says so.
  if (measureOnly.value && (request.action === 'material' || request.action === 'corner' ||
      request.action === 'inside_corner' || request.action === 'pocket' || request.action === 'slot' ||
      request.action === 'ridge' || request.action === 'bore' || request.action === 'boss' || request.action === 'bore_arc' ||
      request.action === 'boss_arc' || request.action === 'angle'))
    request = { ...request, measure_only: true }
  pending.value = request
  line.value = { text: busy, level: 'busy' }
  send(request)
}

const removeListener = onResult((command, result) => {
  if (command === statusRequest) {
    statusRequest = null
    if (result.outcome === 'accepted') setterState.value = result.setter ?? null
    return
  }
  if (command !== pending.value) return
  const request = pending.value
  pending.value = null
  cycleFailed.value = result.outcome !== 'accepted'
  if (result.outcome === 'timed_out') send({ type: 'abort' })
  /** The machine's reason in the panel's language, or the fallback when it gave none. */
  const failure = (fallback: () => string): Line => ({
    text: (): string => result.reason ? translateResult(result) : fallback(), level: 'blocked',
  })
  if (request.action === 'height_map') {
    const map = result.setter?.height_map
    // A map that stopped after some points keeps them, labelled partial, for the card and export.
    const partial = map?.partial !== undefined && measuredPoints(map).length > 0
    if (!map || (result.outcome !== 'accepted' && !partial)) {
      line.value = failure((): string => t('probe.heightMap.failed'))
      return
    }
    if (result.setter) setterState.value = result.setter
    mapResult.value = map
    heightMap.value = map
    if (partial) {
      const values = { count: String(measuredPoints(map).length), total: String(map.points.length) }
      const reason = failure((): string => t('probe.heightMap.failed')).text
      line.value = { text: (): string => t('probe.heightMap.partial', { ...values, reason: reason() }), level: 'blocked' }
      return
    }
    const { low, high } = heightRange(map)
    const values = { count: String(map.points.length), low: formatLength(low, panel.unit), high: formatLength(high, panel.unit),
      unit: panel.unit === 'inch' ? 'in' : 'mm' }
    line.value = { text: (): string => t('probe.heightMap.done', values), level: 'done' }
    return
  }
  if (request.action === 'apply_measurement') {
    measuredResult.value = null
    const material = result.setter?.material
    if (result.outcome === 'accepted' && material) {
      setterState.value = result.setter ?? null
      const values = { wcs: wcsName(material.wcs), axes: axesText(material.axis) }
      line.value = { text: (): string => t('probe.materialProbe.measuredSet', values), level: 'done' }
    } else {
      line.value = failure((): string => t('probe.materialProbe.noOrigin'))
    }
    return
  }
  const measured = 'measure_only' in request && request.measure_only ? result.setter?.measured : undefined
  if (result.outcome === 'accepted' && result.setter && measured) {
    measuredResult.value = { setter: result.setter, measured, frame: frame.value }
    if (request.action !== 'angle') {
      setterState.value = result.setter
      const format = (value: number): string => formatLength(value, panel.unit)
      const values = {
        wcs: wcsName(measured.wcs),
        values: (['x', 'y', 'z'] as const).flatMap((axis) => {
          const value = measured.work[axis]
          return value === undefined ? [] : [`${axis.toUpperCase()} ${format(value)}`]
        }).join(' '),
      }
      line.value = { text: (): string => t('probe.materialProbe.measuredDone', values), level: 'done' }
      return
    }
  }
  if (request.action === 'apply_angle') {
    const degrees = result.setter?.edge_angle
    edgeRotation.value = null
    if (result.outcome === 'accepted' && degrees !== undefined) {
      setterState.value = result.setter ?? null
      line.value = { text: (): string => t('probe.materialProbe.rotationSet', { degrees: degreesText(degrees) }), level: 'done' }
    } else {
      line.value = failure((): string => t('probe.materialProbe.noRotation'))
    }
    return
  }
  if (request.action === 'angle') {
    const degrees = result.setter?.edge_angle
    if (result.outcome !== 'accepted' || degrees === undefined) {
      line.value = failure((): string => t('probe.materialProbe.noAngle'))
      return
    }
    if (result.setter) setterState.value = result.setter
    const rise = Math.tan(degrees * Math.PI / 180) * request.spacing
    const offer = result.setter?.edge_rotation
    const current = panel.status?.offsets.rotation_xy ?? 0
    if (offer) {
      edgeRotation.value = {
        id: offer.measurement_id, wcs: offer.wcs, degrees, current, rotation: offer.rotation ?? current + degrees, frame: frame.value,
      }
    }
    const values = { degrees: degreesText(degrees), rise: Math.abs(rise).toFixed(3), spacing: String(Math.abs(request.spacing)) }
    line.value = request.direction === 'y_plus'
      ? { text: (): string => t('probe.materialProbe.angleFrontDone', values), level: 'done' }
      : { text: (): string => t('probe.materialProbe.angleLeftDone', values), level: 'done' }
    return
  }
  if (request.action === 'bore_arc' || request.action === 'boss_arc') {
    const measured = result.setter?.center_diameter
    if (result.outcome !== 'accepted' || measured === undefined || !result.setter?.material) {
      line.value = failure((): string => t('probe.materialProbe.noOrigin'))
      return
    }
    setterState.value = result.setter
    const values = { wcs: wcsName(result.setter.material.wcs), diameter: measured.toFixed(3), span: (result.setter.arc_span ?? 0).toFixed(0),
      uncertainty: (result.setter.center_uncertainty ?? 0).toFixed(3) }
    line.value = { text: (): string => t('probe.materialProbe.arcDone', values), level: 'done' }
    return
  }
  if (request.action === 'bore' || request.action === 'boss') {
    const measured = result.setter?.center_diameter
    if (result.outcome !== 'accepted' || measured === undefined || !result.setter?.material) {
      line.value = failure((): string => t('probe.materialProbe.noOrigin'))
      return
    }
    setterState.value = result.setter
    const values = { wcs: wcsName(result.setter.material.wcs), diameter: measured.toFixed(3) }
    line.value = request.action === 'bore'
      ? { text: (): string => t('probe.materialProbe.boreDone', values), level: 'done' }
      : { text: (): string => t('probe.materialProbe.bossDone', values), level: 'done' }
    return
  }
  if (request.action === 'ball') {
    const measured = result.setter?.ball_diameter
    if (result.outcome !== 'accepted' || measured === undefined) {
      line.value = failure((): string => t('probe.materialProbe.noBall'))
      return
    }
    if (result.setter) setterState.value = result.setter
    const values = { tool: String(props.tool), diameter: measured.toFixed(3) }
    line.value = { text: (): string => t('probe.materialProbe.ballDone', values), level: 'done' }
    return
  }
  const material = result.setter?.material
  if (result.outcome !== 'accepted' || !material) {
    line.value = failure((): string => t('probe.materialProbe.noOrigin'))
    return
  }
  if (result.setter) setterState.value = result.setter
  const wcs = wcsName(material.wcs)
  if (request.action === 'corner') {
    const corner = cornerOf(request.x_direction, request.y_direction)
    const key = cornerText[corner.key].done
    line.value = { text: (): string => t(key, { wcs, corner: corner.label }), level: 'done' }
    return
  }
  if (request.action === 'inside_corner') {
    const corner = insideCorners.find(entry => entry.x === request.x_direction && entry.y === request.y_direction)?.label ?? ''
    line.value = { text: (): string => t('probe.materialProbe.insideCornerDone', { wcs, corner }), level: 'done' }
    return
  }
  if (request.action === 'pocket' || request.action === 'slot' || request.action === 'ridge') {
    const size = result.setter?.feature_size
    const format = (value: number): string => `${formatLength(value, panel.unit)} ${panel.unit === 'inch' ? 'in' : 'mm'}`
    const width = request.action === 'pocket' ? undefined : size?.[request.axis]
    const span = request.action === 'pocket' && size?.x !== undefined && size.y !== undefined
      ? 'X ' + format(size.x) + ' × Y ' + format(size.y) : width !== undefined ? format(width) : null
    if (!span) {
      line.value = { text: (): string => t('probe.materialProbe.noWidth'), level: 'blocked' }
      return
    }
    const values = { wcs, axes: material.axis === 'XY' ? 'X0 Y0' : `${material.axis}0`, span }
    const key = request.action === 'pocket' ? 'probe.materialProbe.pocketDone'
      : request.action === 'slot' ? 'probe.materialProbe.slotDone' : 'probe.materialProbe.ridgeDone'
    line.value = { text: (): string => t(key, values), level: 'done' }
    return
  }
  const surface = surfaceOf(request.direction)
  const key = surfaceText[surface.key].done
  const values = { wcs, axis: material.axis, edge: surface.label ?? '' }
  line.value = { text: (): string => t(key, values), level: 'done' }
})
watch([
  (): unknown => panel.hello, (): string => panel.link, (): string | undefined => panel.status?.machine,
  allHomed, homingActive,
], () => {
  edgeRotation.value = null
  askingRotation.value = false
  measuredResult.value = null
  showingResult.value = false
  if (!panel.hello?.fixed_setter) return
  setterState.value = null
  statusRequest = null
  if (panel.link !== 'live') return
  statusRequest = { type: 'fixed_setter', action: 'status' }
  send(statusRequest)
}, { immediate: true })
onBeforeUnmount(() => {
  if (pending.value) send({ type: 'abort' })
  removeListener()
})
</script>

<template>
  <section class="material-probe" :aria-label="t('probe.materialProbe.region')">
    <div class="probe-layout">
      <div class="probe-actions">
        <button
          v-for="face in surfaces" :key="face.travel"
          v-tooltip="t(surfaceText[face.key].tip, { approach: face.approach })"
          type="button" class="btn" :class="{ on: pending?.action === 'material' && pending.direction === face.travel }"
          @click="openGuide({ kind: 'surface', direction: face.travel })"
        >
          {{ face.label ?? t('probe.surface.zTop') }}
        </button>
      </div>
      <WorkpieceProbeDiagram :travel="pending?.action === 'material' ? pending.direction : null" :corner="pendingCorner" />
    </div>
    <div class="probe-utilities">
      <button
        class="btn" type="button" aria-haspopup="dialog" :class="{ on: featureActive }"
        @click="pending === null && (choosingFeature = true)"
      >
        {{ t('probe.materialProbe.features') }}
      </button>
      <button
        v-tooltip="t('probe.materialProbe.calibrateTip')"
        type="button" class="btn calibrate" :class="{ on: pending?.action === 'ball' }" :disabled="measureOnly"
        @click="openGuide({ kind: 'ball' })"
      >
        {{ t('probe.materialProbe.calibrate') }}
      </button>
      <button
        v-tooltip="t('probe.materialProbe.measureOnlyTip')"
        type="button" class="btn measure-only" :class="{ on: measureOnly }" :aria-pressed="measureOnly"
        :disabled="pending !== null" @click="measureOnly = !measureOnly"
      >
        {{ t('probe.materialProbe.measureOnly') }}
      </button>
    </div>
    <SetterStatusBand class="probe-status" :line="displayLine">
      <template #actions>
        <button v-if="mapGrid" class="btn rotation-action stop-map" type="button" @click="stopMap">{{ t('probe.heightMap.stop') }}</button>
        <button v-else-if="mapResult" class="btn rotation-action" type="button" aria-haspopup="dialog" @click="showingMap = true">{{ t('probe.heightMap.result') }}</button>
        <button v-if="measuredResult" class="btn rotation-action" type="button" aria-haspopup="dialog" @click="showingResult = true">{{ t('probe.materialProbe.result') }}</button>
        <button v-if="canApplyRotation" class="btn rotation-action" type="button" @click="askingRotation = true">{{ t('probe.materialProbe.applyRotationMenu') }}</button>
      </template>
    </SetterStatusBand>
    <ProbeGuideDialog
      v-if="guide" :target="guide" :limits="panel.hello?.fixed_setter" :tool="tool"
      @start="startGuide" @close="guide = null"
    />
    <ModalDialog v-if="choosingFeature" :title="t('probe.materialProbe.featureTitle')" size="medium" fresh-gesture @close="choosingFeature = false">
      <div class="feature-options">
        <div class="feature-types">
          <button class="btn" type="button" @click="openGuide({ kind: 'bore' })">{{ t('probe.materialProbe.boreCentre') }}</button>
          <button class="btn" type="button" @click="openGuide({ kind: 'boss' })">{{ t('probe.materialProbe.bossCentre') }}</button>
          <button class="btn" type="button" @click="openGuide({ kind: 'angle' })">{{ t('probe.materialProbe.edgeAngle') }}</button>
          <button class="btn" type="button" @click="openMap">{{ t('probe.heightMap.button') }}</button>
          <button v-if="extendedFeatures" class="btn" type="button" @click="openGuide({ kind: 'pocket' })">{{ t('probe.materialProbe.pocket') }}</button>
        </div>
        <div class="feature-row">
          <span>{{ t('probe.materialProbe.outsideCorner') }}</span>
          <div class="feature-buttons corners">
            <button
              v-for="corner in corners" :key="corner.label"
              v-tooltip="t(cornerText[corner.key].tip)"
              class="btn" type="button" :aria-label="t('probe.materialProbe.outsideCornerLabel', { corner: corner.label })"
              @click="openGuide({ kind: 'corner', x: corner.x, y: corner.y })"
            >
              {{ corner.label }}
            </button>
          </div>
        </div>
        <template v-if="extendedFeatures">
          <div class="feature-row">
            <span>{{ t('probe.materialProbe.insideCorner') }}</span>
            <div class="feature-buttons corners">
              <button
                v-for="corner in insideCorners" :key="corner.label" class="btn" type="button"
                :aria-label="t('probe.materialProbe.insideCornerLabel', { corner: corner.label })" @click="openGuide({ kind: 'inside_corner', x: corner.x, y: corner.y })"
              >
                {{ corner.label }}
              </button>
            </div>
          </div>
          <div class="feature-row">
            <span>{{ t('probe.materialProbe.arcFeature') }}</span>
            <div class="feature-buttons">
              <button
                v-for="kind in arcKinds" :key="kind" class="btn" type="button"
                :aria-label="kind === 'bore_arc' ? t('probe.materialProbe.arcBoreLabel') : t('probe.materialProbe.arcBossLabel')"
                @click="openGuide({ kind })"
              >
                {{ kind === 'bore_arc' ? t('probe.materialProbe.arcBore') : t('probe.materialProbe.arcBoss') }}
              </button>
            </div>
          </div>
          <div v-for="kind in narrowFeatures" :key="kind" class="feature-row">
            <span class="feature-name">{{ kind === 'slot' ? t('probe.materialProbe.slot') : t('probe.materialProbe.ridge') }}</span>
            <div class="feature-buttons">
              <button
                v-for="axis in featureAxes" :key="axis" class="btn" type="button"
                :aria-label="kind === 'slot' ? t('probe.materialProbe.slotAcrossLabel', { axis: axis.toUpperCase() }) : t('probe.materialProbe.ridgeAcrossLabel', { axis: axis.toUpperCase() })"
                @click="openGuide({ kind, axis })"
              >
                {{ t('probe.materialProbe.across', { axis: axis.toUpperCase() }) }}
              </button>
            </div>
          </div>
        </template>
      </div>
    </ModalDialog>
    <HeightMapDialog v-if="choosingMap" @start="startMap" @close="choosingMap = false" />
    <HeightMapResult v-if="showingMap && mapResult" :map="mapResult" @close="showingMap = false" />
    <MeasuredResult
      v-if="showingResult && measuredResult" :result="measuredResult.setter" :measured="measuredResult.measured"
      :can-apply="canApplyMeasured" :stale="frame !== measuredResult.frame" @apply="applyMeasured" @close="showingResult = false"
    />
    <ModalDialog v-if="askingRotation && edgeRotation" :title="t('probe.materialProbe.rotationTitle')" size="small" fresh-gesture @close="askingRotation = false">
      <p class="rotation-effect">
        {{ t('probe.materialProbe.rotationEffect', {
          wcs: wcsName(edgeRotation.wcs), degrees: degreesText(edgeRotation.degrees),
          current: degreesText(edgeRotation.current), rotation: degreesText(edgeRotation.rotation),
        }) }}
      </p>
      <p v-if="shifted" class="rotation-effect">{{ t('probe.materialProbe.shiftStays') }}</p>
      <template #footer>
        <button class="btn" type="button" @click="askingRotation = false">{{ t('common.cancel') }}</button>
        <button class="btn primary" type="button" :disabled="!canApplyRotation" @click="applyRotation">{{ t('probe.materialProbe.applyRotation') }}</button>
      </template>
    </ModalDialog>
  </section>
</template>

<style scoped>
.probe-status { height: 48px; margin-bottom: 0; }
.rotation-action { flex: none; min-height: 44px; padding-inline: 8px; font-size: 12px; }
/* Stop takes the danger tones, as a stop button does elsewhere; the band's busy accent on the
   control fill is under 4.5:1 in the light theme. */
.btn.stop-map:not(:disabled, [aria-disabled='true']) { background: var(--stop-soft); color: var(--stop); }
.rotation-effect { margin: 0; line-height: 1.5; }
.material-probe { min-width: 0; container-type: inline-size; }
.probe-layout { display: grid; grid-template-columns: 1fr minmax(186px, 0.96fr); align-items: stretch; gap: 12px; }
.probe-actions { display: grid; grid-template-columns: repeat(4, minmax(44px, 1fr)); gap: 6px; }
.probe-actions button:first-child { grid-column: 1 / -1; }
.probe-actions button { min-height: 56px; padding-inline: 4px; white-space: nowrap; }
.probe-utilities { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
.probe-utilities button { min-height: 44px; }
.feature-options { display: grid; gap: 9px; }
.feature-types { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
.feature-row { display: grid; grid-template-columns: 108px minmax(0, 1fr); gap: 10px; align-items: center; }
.feature-name { text-transform: capitalize; }
.feature-buttons { display: grid; grid-template-columns: repeat(2, minmax(44px, 1fr)); gap: 6px; }
.feature-buttons.corners { grid-template-columns: repeat(4, minmax(44px, 1fr)); }
.feature-options button { min-height: 44px; padding-inline: 6px; }
.feature-buttons button { white-space: nowrap; }
@container (max-width: 400px) {
  .probe-layout { grid-template-columns: minmax(0, 1fr); gap: 8px; }
}
</style>
