import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import MaterialProbeControls from '../src/components/MaterialProbeControls.vue'
import ProbeGuideDialog from '../src/components/ProbeGuideDialog.vue'
import { panel, send } from '../src/machine'
import type { CommandResult, ProbeDirection } from '../src/protocol'
import { setterState } from '../src/setter-state'
import { heightMap } from '../src/height-map'
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

describe('height map', () => {
  it('sends the grid in work millimetres and shows the map', async () => {
    panel.unit = 'inch'
    const fixture = wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await fixture.get('.probe-utilities button[aria-haspopup=dialog]').trigger('click')
    await findButton(fixture, 'Height map…')?.trigger('click')
    const fields = fixture.findAll('.grid-fields input')
    for (const [index, value] of ['0', '1', '0', '0.5', '0.5', '0.5'].entries()) await fields[index]?.setValue(value)
    expect(fixture.get('.summary').text()).toBe('3 × 2 points, about 1 min.')
    await fixture.get('.start-map').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'height_map', tool: 99, x: 0, y: 0, z: 0,
      x_min: 0, x_max: 25.4, y_min: 0, y_max: 12.7, step: 12.7, clearance: 12.7 })
    const map = { wcs: 1, columns: 2, rows: 1, points: [
      { work: [0, 0, 0.0254], machine: [0, 0, 1] }, { work: [25.4, 0, -0.0254], machine: [25.4, 0, 1] },
    ] as { work: [number, number, number]; machine: [number, number, number] }[] }
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, height_map: map } })
    expect(fixture.get('.setter-band.done p').text()).toBe('Height map: 2 points, Z -0.0010 to 0.0010 in. Nothing was set.')
    expect(heightMap.value).toEqual(map)
    await findButton(fixture, 'Map')?.trigger('click')
    expect(fixture.get('[role="dialog"]').text()).toContain('G54, 2 points: Z -0.0010 to 0.0010 in, a spread of 0.0020 in.')
    vi.mocked(send).mockClear()
    await findButton(fixture, 'Save CSV')?.trigger('click')
    const saved = vi.mocked(send).mock.lastCall?.[0]
    expect(saved).toMatchObject({ type: 'save_program' })
    expect(saved?.type === 'save_program' && saved.path).toMatch(/^heightmap-\d{8}-\d{6}\.csv$/)
    const lines = saved?.type === 'save_program' ? saved.text.split('\n') : []
    expect(lines[0]).toMatch(/^# CoffeeCNC height map system=G54 unit=in rotation_deg=0 date=\S+ points=2 grid_points=2$/)
    expect(lines[1]).toBe('work_x_in,work_y_in,work_z_in,machine_x_in,machine_y_in,machine_z_in')
    // The probe file stays LinuxCNC's own format; its frame goes beside it.
    vi.mocked(send).mockClear()
    await findButton(fixture, 'Save LinuxCNC probe file')?.trigger('click')
    const paths = vi.mocked(send).mock.calls.map(([command]) => command.type === 'save_program' ? command.path : '')
    expect(paths).toHaveLength(2)
    expect(paths[0]).toMatch(/^heightmap-\d{8}-\d{6}\.txt\.json$/)
    expect(paths[1]).toBe(paths[0]?.replace(/\.json$/, ''))
  })

  async function startMap(fixture: VueWrapper): Promise<void> {
    await fixture.get('.probe-utilities button[aria-haspopup=dialog]').trigger('click')
    await findButton(fixture, 'Height map…')?.trigger('click')
    const fields = fixture.findAll('.grid-fields input')
    // X 0 to 20, Y 0 to 10 at 10 mm: 3 × 2 points.
    for (const [index, value] of ['0', '20', '0', '10', '10', '5'].entries()) await fields[index]?.setValue(value)
    await fixture.get('.start-map').trigger('click')
  }
  const at = async (x: number, y: number): Promise<void> => {
    for (const axis of panel.status?.axes ?? []) {
      if (axis.axis === 'x') axis.work = x
      if (axis.axis === 'y') axis.work = y
    }
    await nextTick()
  }

  it('sends no measure_only with a height map: it never writes', async () => {
    const fixture = wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await fixture.get('.measure-only').trigger('click')
    expect(fixture.get('.measure-only').attributes('aria-pressed')).toBe('true')
    await startMap(fixture)
    const request = vi.mocked(send).mock.lastCall?.[0]
    expect(request).toMatchObject({ type: 'fixed_setter', action: 'height_map' })
    expect(request).not.toHaveProperty('measure_only')
  })

  it('shows the point it probes and stops on the band', async () => {
    const fixture = wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await startMap(fixture)
    expect(fixture.get('.setter-band p').text()).toBe('Probing the height map: point 1 of 6…')
    // Probing order: the first row out, the second back. Between points the count holds.
    await at(10, 0)
    expect(fixture.get('.setter-band p').text()).toBe('Probing the height map: point 2 of 6…')
    await at(15, 5)
    expect(fixture.get('.setter-band p').text()).toBe('Probing the height map: point 2 of 6…')
    await at(20, 10)
    expect(fixture.get('.setter-band p').text()).toBe('Probing the height map: point 4 of 6…')
    expect(findButton(fixture, 'Map')).toBeUndefined()
    vi.mocked(send).mockClear()
    await findButton(fixture, 'Stop')?.trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'abort' })
  })

  it('keeps the points of a stopped map as a partial map, and saves them labelled partial', async () => {
    const fixture = wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await startMap(fixture)
    const map = { wcs: 1, columns: 3, rows: 2, rotation: 0, partial: 'The height map was stopped.', points: [
      { work: [0, 0, 0.1], machine: [0, 0, 1] }, { work: [10, 0, 0.2], machine: [10, 0, 1] }, null, null, null, null,
    ] as ({ work: [number, number, number]; machine: [number, number, number] } | null)[] }
    await answer({ outcome: 'failed', reason: 'The height map was stopped.', setter: { calibration: null, measurement: null, height_map: map } })
    expect(fixture.get('.setter-band p').text()).toBe('The height map was stopped. Kept 2 of 6 points as a partial map.')
    expect(heightMap.value).toEqual(map)
    expect(findButton(fixture, 'Stop')).toBeUndefined()
    await findButton(fixture, 'Map')?.trigger('click')
    const card = fixture.get('[role="dialog"]').text()
    expect(card).toContain('G54, 2 points: Z 0.100 to 0.200 mm')
    expect(card).toContain('Partial map: 2 of 6 points. The height map was stopped.')
    vi.mocked(send).mockClear()
    await findButton(fixture, 'Save CSV')?.trigger('click')
    const saved = vi.mocked(send).mock.lastCall?.[0]
    expect(saved?.type === 'save_program' && saved.path).toMatch(/^heightmap-\d{8}-\d{6}-partial\.csv$/)
    const lines = saved?.type === 'save_program' ? saved.text.split('\n') : []
    expect(lines[0]).toContain(' points=2 grid_points=6 partial="The height map was stopped."')
    expect(lines).toHaveLength(5)
  })

  it('refuses a grid whose worst case exceeds the 2 h limit', async () => {
    // A 5 mm/min latch: about 98 s a point, so 150 points are over 4 h.
    panel.hello = { ...hello, fixed_setter: { reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5,
      latch_feed_mm_min: 5, latch_backoff_mm: 1, material_travel_mm: 20 } }
    const fixture = wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global })
    await fixture.get('.probe-utilities button[aria-haspopup=dialog]').trigger('click')
    await findButton(fixture, 'Height map…')?.trigger('click')
    const fields = fixture.findAll('.grid-fields input')
    // 15 × 10 points; the clearance stays at the present Z, so there is no descent.
    for (const [index, value] of ['0', '140', '0', '90', '10'].entries()) await fields[index]?.setValue(value)
    expect(fixture.get('.summary').text()).toBe('Up to 245 min, more than the 120 min limit. Use fewer points.')
    expect(fixture.get('.start-map').attributes('disabled')).toBeDefined()
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
