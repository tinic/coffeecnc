import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import MaterialProbeControls from '../src/components/MaterialProbeControls.vue'
import ProbeGuideDialog from '../src/components/ProbeGuideDialog.vue'
import { panel, send } from '../src/machine'
import type { CommandResult, ProbeDirection } from '../src/protocol'
import { setterState } from '../src/setter-state'
import { hello, SimMachine } from '../src/sim/machine'
import type { ResultListener } from '../src/store'

const { listeners } = vi.hoisted(() => ({ listeners: new Set<ResultListener>() }))
// The guide's WebGL chunk must not load in the background (#279).
vi.mock('../src/components/ProbeGuideCanvas.vue', () => import('./canvas-stub'))
vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const panel = reactive({ status: null }) as typeof import('../src/machine').panel
  return { panel, allHomed: computed(() => false), homingActive: computed(() => false),
    manualBlock: computed(() => panel.status?.interp === 'idle' ? null : 'A program is running'),
    findTool: (): { diameter: number } => ({ diameter: 2 }),
    send: vi.fn(), onResult: (listener: ResultListener): (() => void) => {
      listeners.add(listener)
      return () => { listeners.delete(listener) }
    } }
})

const global = { stubs: { Teleport: true, GuideCanvas: true } }
let wrapper: VueWrapper | undefined
beforeEach(() => {
  vi.stubGlobal('matchMedia', () => ({ matches: true }))
  vi.mocked(send).mockReset()
  const status = new SimMachine({ emit: (): void => undefined }).status()
  status.machine = 'on'; status.link = 'live'; status.tool.in_spindle = 99
  status.spindle.actual_rpm = 0; status.motion.in_position = true
  panel.status = status
  panel.hello = null
  panel.link = 'live'
  panel.unit = 'mm'
  setterState.value = { calibration: [0, 0, -10], measurement: null, measured_tools: [98, 99] }
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined; vi.unstubAllGlobals() })

function findButton(fixture: VueWrapper, label: string): ReturnType<VueWrapper['findAll']>[number] | undefined {
  return fixture.findAll('button').find((entry) => entry.text() === label || entry.attributes('aria-label') === label)
}
async function selectAction(fixture: VueWrapper, label: string): Promise<void> {
  let button = findButton(fixture, label)
  if (!button) {
    await fixture.get('.probe-utilities button[aria-haspopup=dialog]').trigger('click')
    button = findButton(fixture, label)
  }
  if (!button) throw new Error(`Missing ${label}`)
  expect(button.attributes('disabled')).toBeUndefined()
  await button.trigger('click')
}
async function probe(label: string): Promise<VueWrapper> {
  const fixture = wrapper ??= mount(MaterialProbeControls, { props: { tool: 99 }, global })
  const wasOpen = fixture.findComponent(ProbeGuideDialog).exists()
  const before = vi.mocked(send).mock.calls.length
  await selectAction(fixture, label)
  if (!wasOpen && fixture.findComponent(ProbeGuideDialog).exists()) {
    expect(send).toHaveBeenCalledTimes(before)
    if (!fixture.find('.dimension input').exists()) await fixture.get('.start-probe').trigger('click')
  }
  return fixture
}
async function answer(result: Omit<CommandResult, 'id' | 'type'>): Promise<void> {
  const command = vi.mocked(send).mock.lastCall?.[0]
  if (!command) throw new Error('Missing request')
  for (const listener of listeners) listener(command, { type: 'result', id: 1, ...result })
  await nextTick()
}
const faces: [string, ProbeDirection, 'X' | 'Y' | 'Z', string, string][] = [
  ['Z top', 'z_top', 'Z', 'top surface', '−Z'],
  ['X−', 'x_plus', 'X', 'left edge', '+X'],
  ['X+', 'x_minus', 'X', 'right edge', '−X'],
  ['Y−', 'y_plus', 'Y', 'front edge', '+Y'],
  ['Y+', 'y_minus', 'Y', 'back edge', '−Y'],
]

const cornerCases: [string, 'x_minus' | 'x_plus', 'y_minus' | 'y_plus', string][] = [
  ['X− Y−', 'x_plus', 'y_plus', 'front-left corner'],
  ['X+ Y−', 'x_minus', 'y_plus', 'front-right corner'],
  ['X− Y+', 'x_plus', 'y_minus', 'back-left corner'],
  ['X+ Y+', 'x_minus', 'y_minus', 'back-right corner'],
]

describe('probe setup guides', () => {
  it.each([...faces.map(([label]) => label), ...cornerCases.map(([label]) => 'Outside corner ' + label),
    'Bore centre', 'Boss centre…', 'Edge angle…', 'Calibrate ball…'])('opens and closes %s without any motion', async (label) => {
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await selectAction(wrapper, label)
    expect(wrapper.get('[role="dialog"] .instruction').text()).toContain('Jog here first')
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('[role="dialog"] header button[aria-label="Close"]').trigger('click')
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })
  it('keeps the basic features on machines without the extended probing capability', async () => {
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await wrapper.get('.probe-utilities button[aria-haspopup=dialog]').trigger('click')
    expect(wrapper.get('.feature-options').text()).toContain('Bore centre')
    expect(wrapper.get('.feature-options').text()).toContain('Outside corner')
    expect(wrapper.get('.feature-options').text()).not.toContain('Inside corner')
    expect(wrapper.get('.feature-options').text()).not.toContain('Rectangular pocket')
    expect(send).not.toHaveBeenCalled()
  })
})

describe('corner probe workflow', () => {
  it.each(cornerCases)('probes the %s corner from over the part and sets X0 and Y0', async (label, x, y, name) => {
    const fixture = await probe('Outside corner ' + label)
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'corner', x_direction: x, y_direction: y,
      tool: 99, x: 0, y: 0, z: 0 })
    expect(fixture.get('.setter-band.busy p').text()).toBe(`Probing the ${name}…`)
    expect(fixture.get('.workpiece .part').classes()).toEqual(expect.arrayContaining([x, y]))
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, material: { wcs: 2, axis: 'XY' } } })
    expect(fixture.get('.setter-band.done p').text()).toBe(`G55 X0 Y0 set at the ${label} ${name}.`)
  })

  it('asks for a measured probe first, since the corner goes below Z0', async () => {
    setterState.value = { calibration: [0, 0, -10], measurement: null, measured_tools: [98] }
    const fixture = await probe('Outside corner X− Y−')
    expect(send).not.toHaveBeenCalled()
    expect(fixture.get('.setter-band.blocked p').text()).toBe('Touch off T99 and probe Z top before probing a corner.')
  })
})

describe('bore and boss centres', () => {
  it('centres on a bore from inside and reports its diameter', async () => {
    await probe('Bore centre')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'bore', tool: 99, x: 0, y: 0, z: 0 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null,
      material: { wcs: 2, axis: 'XY' }, center_diameter: 16 } })
    expect(wrapper?.get('.setter-band.done p').text()).toBe('G55 X0 Y0 set at the bore centre, Ø 16.000 mm.')
  })

  it('asks for the approximate boss diameter, then centres on it', async () => {
    const fixture = await probe('Boss centre…')
    expect(send).not.toHaveBeenCalled()
    await fixture.get('.dimension input').setValue('30mm')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'boss', diameter: 30, tool: 99, x: 0, y: 0, z: 0 })
    expect(fixture.get('.setter-band.busy p').text()).toBe('Probing the boss sides…')
  })
})

describe('recess and ridge setup', () => {
  async function choose(label: string): Promise<VueWrapper> {
    panel.hello = { ...hello, fixed_setter: {
      reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, material_features: true,
    } }
    const setter = setterState.value
    if (!setter) throw new Error('Missing setter status')
    const fixture = wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await answer({ outcome: 'accepted', reason: '', setter })
    vi.mocked(send).mockClear()
    await fixture.get('.probe-utilities button[aria-haspopup=dialog]').trigger('click')
    const button = fixture.findAll('.feature-options button').find(entry =>
      (entry.attributes('aria-label') ?? entry.text()) === label)
    if (!button) throw new Error('Missing feature ' + label)
    await button.trigger('click')
    expect(send).not.toHaveBeenCalled()
    return fixture
  }

  it('keeps a missed second wall visible as the controller returns to idle', async () => {
    const fixture = await choose('Inside corner X− Y+')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'inside_corner',
      tool: 99, x: 0, y: 0, z: 0, x_direction: 'x_minus', y_direction: 'y_plus' })
    const status = panel.status
    if (!status) throw new Error('Missing status')
    status.interp = 'reading'
    status.motion.in_position = false
    await nextTick()
    status.interp = 'idle'
    status.motion.in_position = true
    await answer({ outcome: 'failed', reason: '+Y face: No probe contact within the search travel.' })
    expect(fixture.get('.setter-band.blocked p').text()).toContain('+Y face')
  })

  it('probes a 3-point bore along the entered directions and refuses a set too close together', async () => {
    const fixture = await choose('3-point bore')
    const fields = fixture.findAll('.arc-angles input')
    expect(fields.map(field => (field.element as HTMLInputElement).value)).toEqual(['0', '120', '240'])
    await fields[1]?.setValue('5')
    expect(fixture.get('.start-probe').attributes('disabled')).toBeDefined()
    // An 80° arc is below the 90° minimum; 90° is enough.
    await fields[0]?.setValue('-80')
    await fields[1]?.setValue('-40')
    await fields[2]?.setValue('0')
    expect(fixture.get('.start-probe').attributes('disabled')).toBeDefined()
    expect(fixture.get('.arc-angles small').text()).toContain('covering at least 90°')
    await fields[0]?.setValue('-90')
    await fields[1]?.setValue('-45')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'bore_arc', angles: [-90, -45, 0],
      tool: 99, x: 0, y: 0, z: 0 })
    expect(fixture.get('.setter-band.busy p').text()).toBe('Probing three bore walls…')
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null,
      material: { wcs: 2, axis: 'XY' }, center_diameter: 16, arc_span: 117.3, center_uncertainty: 0.0053 } })
    expect(fixture.get('.setter-band.done p').text())
      .toBe('G55 X0 Y0 set at the circle centre, Ø 16.000 mm, from contacts over 117°. Centre estimated ±0.005 mm (latch repeatability only).')
  })

  it('shows the clearance and depth rows for a 3-point boss, probed from beside it, not for a bore', async () => {
    for (const [label, beside] of [['3-point boss', true], ['3-point bore', false]] as const) {
      const fixture = await choose(label)
      const limits = fixture.get('.limits').text()
      expect(limits.includes('Traverse clearance'), label).toBe(beside)
      expect(limits.includes('Ball centre at contact'), label).toBe(beside)
      // Only gross faults change the diameter enough: the operator can check it first.
      expect(fixture.text().includes('use Measure only to check the reported diameter'), label).toBe(beside)
      fixture.unmount()
      wrapper = undefined
    }
  })

  it('asks a 3-point boss for its approximate diameter as well', async () => {
    const fixture = await choose('3-point boss')
    expect(fixture.get('.start-probe').attributes('disabled')).toBeDefined()
    await fixture.get('.dimension input').setValue('30mm')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'boss_arc', angles: [0, 120, 240],
      diameter: 30, tool: 99, x: 0, y: 0, z: 0 })
  })

  it('reports a one-axis ridge width in the display units', async () => {
    panel.unit = 'inch'
    const fixture = await choose('ridge across Y')
    expect(fixture.get('.start-probe').attributes('disabled')).toBeDefined()
    await fixture.get('.dimension input').setValue('1/2in')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'ridge',
      tool: 99, x: 0, y: 0, z: 0, axis: 'y', width: 12.7 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null,
      material: { wcs: 2, axis: 'Y' }, feature_size: { y: 12.7 } } })
    expect(fixture.get('.setter-band.done p').text()).toBe('G55 Y0 set at the ridge centre · 0.5000 in')
  })
})

describe('measure only', () => {
  // Every probe button the panel offers, found from the rendered panel: a new action that does
  // not carry measure_only fails here instead of writing an offset while Measure only is on.
  it('sends measure_only on every probe action except the ball calibration', async () => {
    panel.hello = { ...hello, fixed_setter: {
      reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, material_features: true,
    } }
    const setter = setterState.value
    if (!setter) throw new Error('Missing setter status')
    const fixture = wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await answer({ outcome: 'accepted', reason: '', setter })
    await fixture.get('.measure-only').trigger('click')
    expect(fixture.get('.calibrate').attributes('disabled')).toBeDefined()
    vi.mocked(send).mockClear()
    const openFeatures = async (): Promise<ReturnType<VueWrapper['findAll']>> => {
      await fixture.get('.probe-utilities button[aria-haspopup=dialog]').trigger('click')
      return fixture.findAll('.feature-options button')
    }
    const featureCount = (await openFeatures()).length
    await fixture.get('[role="dialog"] .close, [role="dialog"] button[aria-label="Close"]').trigger('click')
    const surfaceCount = fixture.findAll('.probe-actions button').length
    const start = async (button: ReturnType<VueWrapper['findAll']>[number] | undefined): Promise<void> => {
      if (!button) throw new Error('Missing probe button')
      await button.trigger('click')
      if (fixture.find('.dimension input').exists()) await fixture.get('.dimension input').setValue('20mm')
      await fixture.get('.start-probe').trigger('click')
      const request = vi.mocked(send).mock.lastCall?.[0]
      expect(request, JSON.stringify(request)).toMatchObject({ type: 'fixed_setter', measure_only: true })
      await answer({ outcome: 'failed', reason: 'No probe contact within the search travel.' })
    }
    for (let index = 0; index < featureCount; index++) await start((await openFeatures())[index])
    for (let index = 0; index < surfaceCount; index++) await start(fixture.findAll('.probe-actions button')[index])
    const actions = vi.mocked(send).mock.calls.map(([request]) => 'action' in request ? request.action : '')
    expect(actions).toHaveLength(featureCount + surfaceCount)
    expect(new Set(actions)).toEqual(new Set(['bore', 'boss', 'angle', 'pocket', 'corner', 'inside_corner', 'bore_arc',
      'boss_arc', 'slot', 'ridge', 'material']))
  })
})

describe('edge angle', () => {
  it('probes the front edge at two points and reports the turn', async () => {
    const fixture = await probe('Edge angle…')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'angle', direction: 'y_plus', spacing: 50,
      tool: 99, x: 0, y: 0, z: 0 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, edge_angle: -0.1 } })
    expect(fixture.get('.setter-band.done p').text()).toBe('Front edge turns −0.100° from the work X axis (0.087 mm over 50 mm).')
  })
  it('in a rotated work system offers the present rotation plus the measured turn', async () => {
    if (!panel.status) throw new Error('Missing machine status')
    panel.status.offsets.wcs = 2
    panel.status.offsets.rotation_xy = 7.5
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    const fixture = await probe('Edge angle…')
    await fixture.get('.start-probe').trigger('click')
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null,
      edge_angle: 0.25, edge_rotation: { wcs: 2, measurement_id: 9, rotation: 7.75 } } })
    expect(fixture.get('.setter-band.done p').text()).toContain('Front edge turns +0.250° from the work X axis')
    await probe('Apply rotation…')
    expect(fixture.get('[role="dialog"]').text()).toContain(
      'Turn the G55 XY axes +0.250° about its stored origin. Its rotation changes from +7.500° to +7.750°.')
    await probe('Apply rotation')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'apply_angle', tool: 99, measurement_id: 9 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, edge_angle: 7.75 } })
    expect(fixture.get('.setter-band.done p').text()).toBe('Work rotation set to +7.750°.')
  })
  it('selects the left edge and applies the recorded rotation only after confirmation', async () => {
    if (!panel.status) throw new Error('Missing machine status')
    panel.status.offsets.wcs = 2
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    const fixture = await probe('Edge angle…')
    await probe('Left · X−')
    expect(fixture.get('.instruction p').text()).toContain('along Y')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'angle', direction: 'x_plus',
      spacing: 50, tool: 99, x: 0, y: 0, z: 0 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null,
      edge_angle: 2.862, edge_rotation: { wcs: 2, measurement_id: 8 } } })
    expect(fixture.get('.setter-band.done p').text()).toContain('Left edge turns +2.862°')
    await probe('Apply rotation…')
    expect(fixture.get('[role="dialog"]').text()).toContain('G55 XY axes +2.862°')
    expect(send).toHaveBeenCalledTimes(1)
    await probe('Apply rotation')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'apply_angle', tool: 99, measurement_id: 8 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, edge_angle: 2.862 } })
    expect(fixture.get('.setter-band.done p').text()).toBe('Work rotation set to +2.862°.')
  })

  it('measures only when toggled, shows the result card and sets the origin after review', async () => {
    if (!panel.status) throw new Error('Missing machine status')
    panel.status.offsets.wcs = 2
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    const fixture = await probe('Measure only')
    expect(fixture.get('.measure-only').attributes('aria-pressed')).toBe('true')
    expect(fixture.get('.calibrate').attributes('disabled')).toBeDefined()
    await probe('Bore centre')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'bore', measure_only: true, tool: 99, x: 0, y: 0, z: 0 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, center_diameter: 16,
      measured: { measurement_id: 4, wcs: 2, axes: 'XY', work: { x: 4, y: -2 }, machine: [9, -5, 6], faces: [
        { direction: 'x_plus', work: [12, -2, -6], machine: [17, -5, 6] },
      ] } } })
    expect(fixture.get('.setter-band.done p').text()).toBe('Measured X 4.000 Y -2.000 in G55. Nothing was set.')
    await probe('Result')
    const card = fixture.get('[role="dialog"]').text()
    expect(card).toContain('Measured only: no offset was set.')
    expect(card).toContain('X 4.000  Y -2.000 mm')
    expect(card).toContain('X 9.000  Y -5.000  Z 6.000 mm')
    expect(card).toContain('Diameter16.000 mm')
    expect(card).toContain('+X')
    await probe('Set X0 Y0…')
    expect(send).toHaveBeenCalledTimes(1)
    expect(fixture.get('[role="dialog"]').text()).toContain('The point that read X 4.000  Y -2.000 becomes X0 Y0.')
    await probe('Set X0 Y0')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'apply_measurement', measurement_id: 4, tool: 99 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, material: { wcs: 2, axis: 'XY' } } })
    expect(fixture.get('.setter-band.done p').text()).toBe('G55 X0 Y0 set at the measured point.')
    expect(fixture.find('[role="dialog"]').exists()).toBe(false)
  })

  it('cannot set a measurement once the work system changed', async () => {
    if (!panel.status) throw new Error('Missing machine status')
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    const fixture = await probe('Measure only')
    await probe('Z top')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'material', direction: 'z_top', measure_only: true, tool: 99, x: 0, y: 0, z: 0 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null,
      measured: { measurement_id: 5, wcs: 1, axes: 'Z', work: { z: 1.25 }, machine: [0, 0, 12], faces: [] } } })
    expect(fixture.get('.setter-band.done p').text()).toBe('Measured Z 1.250 in G54. Nothing was set.')
    panel.status.offsets.g5x[2] += 1
    await nextTick()
    await probe('Result')
    expect(fixture.get('[role="dialog"]').text()).toContain('The work system changed after this measurement.')
    expect(findButton(fixture, 'Set Z0…')?.attributes('disabled')).toBeDefined()
  })

  it('withdraws the confirmation when the measured work system changes', async () => {
    if (!panel.status) throw new Error('Missing machine status')
    panel.status.offsets.wcs = 2
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    const fixture = await probe('Edge angle…')
    await fixture.get('.start-probe').trigger('click')
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null,
      edge_angle: -0.1, edge_rotation: { wcs: 2, measurement_id: 8 } } })
    await probe('Apply rotation…')
    panel.status.offsets.g5x[0] += 1
    await nextTick()
    expect(fixture.find('[role="dialog"]').exists()).toBe(false)
    expect(fixture.find('.rotation-action').exists()).toBe(false)
    expect(send).toHaveBeenCalledTimes(1)
  })

})

describe('ball calibration', () => {
  it('asks for the gauge width, probes both sides and reports the saved diameter', async () => {
    const fixture = wrapper ??= mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await fixture.get('button.calibrate').trigger('click')
    expect(fixture.findComponent(ProbeGuideDialog).exists()).toBe(true)
    await fixture.get('.dimension input').setValue('40.2mm/2')
    await fixture.get('.start-probe').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'ball', gauge_width: 20.1,
      tool: 99, x: 0, y: 0, z: 0 })
    expect(fixture.get('.setter-band.busy p').text()).toBe('Probing both sides of the gauge…')
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, ball_diameter: 2.1 } })
    expect(fixture.get('.setter-band.done p').text()).toBe('T99 ball Ø2.100 mm saved to the tool table.')
  })
})

describe('material probe workflow', () => {
  it.each(faces)('approaches the %s face and confirms the active WCS', async (label, direction, axis, face, approach) => {
    const fixture = await probe(label)
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'material', direction,
      tool: 99, x: 0, y: 0, z: 0 })
    const button = fixture.findAll('button').find((entry) => entry.text() === label)
    expect(button?.attributes('aria-description')).toContain(`Travel ${approach}`)
    expect(fixture.get('.setter-band.busy p').text()).toBe(`Probing ${face}…`)
    expect(fixture.get('.workpiece .part').classes()).toContain(direction)
    expect(fixture.findAll('.workpiece button')).toHaveLength(0)
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, material: { wcs: 2, axis } } })
    expect(fixture.get('.setter-band.done p').text()).toBe(`G55 ${axis}0 set at the ${axis === 'Z' ? face : `${label} ${face}`}.`)
    expect(fixture.find('[role="dialog"]').exists()).toBe(false)
  })

  it('reports a running program and an unloaded probe from the enabled buttons', async () => {
    const status = panel.status
    if (!status) throw new Error('Missing status')
    status.interp = 'reading'
    const fixture = await probe('X+')
    expect(fixture.get('.setter-band.blocked p').text()).toBe('A program is running')
    status.interp = 'idle'; status.tool.in_spindle = 1
    await nextTick()
    expect(fixture.get('.setter-band.info p').text()).toBe('Load T99 to probe material.')
    await probe('Z top')
    expect(fixture.get('.setter-band.blocked p').text()).toBe('Load T99 to probe material.')
    expect(send).not.toHaveBeenCalled()
  })

  it('requires a measured probe for Z and reports controller failures in place', async () => {
    setterState.value = null
    const fixture = await probe('Z top')
    expect(fixture.get('.setter-band.blocked p').text()).toContain('Touch off T99')
    expect(send).not.toHaveBeenCalled()
    await probe('Y−')
    await answer({ outcome: 'failed', reason: 'No probe contact within the search travel.' })
    expect(fixture.get('.setter-band.blocked p').text()).toBe('No probe contact within the search travel.')
  })

  it('reads the probe measurement when the operator opens Touch off directly', async () => {
    panel.hello = { ...hello, fixed_setter: {
      reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, material_travel_mm: 20,
    } }
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'status' })
    await answer({ outcome: 'accepted', reason: '', setter: {
      calibration: [0, 0, -10], measurement: null, measured_tools: [98, 99],
    } })
    await probe('Z top')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'material', direction: 'z_top',
      tool: 99, x: 0, y: 0, z: 0 })
  })
})
