<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage, translateResult } from '../i18n/messages'
import { allHomed, homingActive, manualBlock, onResult, panel, send } from '../machine'
import { homingMessage } from '../machine-block'
import type { CommandBody, ProbeDirection } from '../protocol'
import { setterState, touchedOff, type SetterStatusLine } from '../setter-state'
import { formatLength, wcsName } from '../units'
import SetterStatusBand from './SetterStatusBand.vue'
import WorkpieceProbeDiagram from './WorkpieceProbeDiagram.vue'
import ProbeGuideDialog from './ProbeGuideDialog.vue'
import type { AngleEdge, MaterialGuideTarget } from '../probe-guide'
import ModalDialog from './ModalDialog.vue'
import { vTooltip } from '../tooltip'

const props = defineProps<{ tool: number }>()
const { t } = useI18n()
type Request = Extract<CommandBody, { type: 'fixed_setter'; action: 'material' | 'corner' | 'ball' | 'bore' | 'boss' | 'angle' | 'apply_angle' | 'inside_corner' | 'pocket' | 'slot' | 'ridge' | 'runout' | 'clear_runout' }>
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
  return choosingFeature.value || action !== undefined && !['material', 'ball', 'apply_angle'].includes(action)
})
function openGuide(target: MaterialGuideTarget): void {
  if (pending.value === null) {
    choosingFeature.value = false
    guide.value = target
  }
}
function startGuide(size: number, angle: AngleEdge, centred: boolean): void {
  const target = guide.value
  guide.value = null
  if (target !== null) probe(target, size, angle, centred)
}
/** The runout compensation the controller applies to this probe's XY results. */
const runout = computed(() => setterState.value?.runout?.tool === props.tool ? setterState.value.runout : null)
const signed = (mm: number): string => `${mm >= 0 ? '+' : '−'}${Math.abs(mm).toFixed(3)}`
const runoutValues = computed(() => {
  const value = runout.value
  return value ? { ex: signed(value.eccentricity[0]), ey: signed(value.eccentricity[1]), rx: signed(value.radius_left[0]), ry: signed(value.radius_left[1]) } : null
})
function clearRunout(): void {
  if (pending.value || !runout.value) return
  choosingFeature.value = false
  const request = { type: 'fixed_setter', action: 'clear_runout', tool: props.tool } as const
  pending.value = request
  line.value = { text: (): string => t('probe.materialProbe.runoutClearing'), level: 'busy' }
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

function probe(target: MaterialGuideTarget, size: number, angle: AngleEdge, centred = true): void {
  if (pending.value) return
  cycleFailed.value = false
  edgeRotation.value = null
  askingRotation.value = false
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
    if (target.kind === 'boss') reason = (): string => t('probe.materialProbe.touchOffBoss', { tool })
    if (target.kind === 'ridge') reason = (): string => t('probe.materialProbe.touchOffRidge', { tool })
    if (target.kind === 'ball') reason = (): string => t('probe.materialProbe.touchOffGauge', { tool })
    if (target.kind === 'runout_pin') reason = (): string => t('probe.materialProbe.touchOffPin', { tool })
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
    case 'runout_ring':
    case 'runout_pin':
      request = { ...position, action: 'runout', diameter: size, outside: target.kind === 'runout_pin', centred }
      busy = (): string => t('probe.materialProbe.runoutBusy')
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
  if (request.action === 'runout' || request.action === 'clear_runout') {
    if (result.outcome !== 'accepted') {
      line.value = failure((): string => t('probe.materialProbe.noRunout'))
      return
    }
    setterState.value = result.setter ?? null
    const saved = result.setter?.runout
    if (request.action === 'clear_runout' || !saved) {
      line.value = { text: (): string => t('probe.materialProbe.runoutCleared'), level: 'done' }
      return
    }
    const values = { tool: String(props.tool), ex: signed(saved.eccentricity[0]), ey: signed(saved.eccentricity[1]),
      rx: signed(saved.radius_left[0]), ry: signed(saved.radius_left[1]) }
    line.value = saved.centred
      ? { text: (): string => t('probe.materialProbe.runoutDone', values), level: 'done' }
      : { text: (): string => t('probe.materialProbe.runoutRadiusDone', values), level: 'done' }
    return
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
        type="button" class="btn calibrate" :class="{ on: pending?.action === 'ball' }" @click="openGuide({ kind: 'ball' })"
      >
        {{ t('probe.materialProbe.calibrate') }}
      </button>
    </div>
    <SetterStatusBand class="probe-status" :line="displayLine">
      <template #actions>
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
            <span>{{ t('probe.materialProbe.runout') }}</span>
            <div class="feature-buttons runout">
              <button class="btn" type="button" :aria-label="t('probe.materialProbe.runoutRingLabel')" @click="openGuide({ kind: 'runout_ring' })">{{ t('probe.materialProbe.runoutRing') }}</button>
              <button class="btn" type="button" :aria-label="t('probe.materialProbe.runoutPinLabel')" @click="openGuide({ kind: 'runout_pin' })">{{ t('probe.materialProbe.runoutPin') }}</button>
              <button class="btn" type="button" :disabled="!runout" :aria-label="t('probe.materialProbe.runoutClearLabel')" @click="clearRunout">{{ t('probe.materialProbe.runoutClear') }}</button>
            </div>
          </div>
          <p v-if="runoutValues" class="runout-in-use">{{ t('probe.materialProbe.runoutInUse', runoutValues) }}</p>
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
.feature-buttons.runout { grid-template-columns: repeat(3, minmax(44px, 1fr)); }
.runout-in-use { margin: -3px 0 0; color: var(--text-dim); font-size: 12px; }
.feature-options button { min-height: 44px; padding-inline: 6px; }
.feature-buttons button { white-space: nowrap; }
@container (max-width: 400px) {
  .probe-layout { grid-template-columns: minmax(0, 1fr); gap: 8px; }
}
</style>
