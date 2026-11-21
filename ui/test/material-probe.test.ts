import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import MaterialProbeControls from '../src/components/MaterialProbeControls.vue'
import NumberDialog from '../src/components/NumberDialog.vue'
import { panel, send } from '../src/machine'
import type { CommandResult, ProbeDirection } from '../src/protocol'
import { setterState } from '../src/setter-state'
import { hello, SimMachine } from '../src/sim/machine'
import type { ResultListener } from '../src/store'

const { listeners } = vi.hoisted(() => ({ listeners: new Set<ResultListener>() }))
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

let wrapper: VueWrapper | undefined
beforeEach(() => {
  vi.mocked(send).mockReset()
  const status = new SimMachine({ emit: (): void => undefined }).status()
  status.machine = 'on'; status.link = 'live'; status.tool.in_spindle = 99
  status.spindle.actual_rpm = 0; status.motion.in_position = true
  panel.status = status
  panel.hello = null
  panel.link = 'live'
  setterState.value = { calibration: [0, 0, -10], measurement: null, measured_tools: [98, 99] }
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

async function probe(label: string): Promise<VueWrapper> {
  const fixture = wrapper ??= mount(MaterialProbeControls, { props: { tool: 99 } })
  const button = fixture.findAll('button').find((entry) => entry.text() === label)
  if (!button) throw new Error(`Missing ${label}`)
  expect(button.attributes('disabled')).toBeUndefined()
  await button.trigger('click')
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

describe('corner probe workflow', () => {
  it.each(cornerCases)('probes the %s corner from over the part and sets X0 and Y0', async (label, x, y, name) => {
    const fixture = await probe(label)
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'corner', x_direction: x, y_direction: y,
      tool: 99, x: 0, y: 0, z: 0 })
    expect(fixture.get('.setter-band.busy p').text()).toBe(`Probing the ${name}…`)
    expect(fixture.get('.workpiece .part').classes()).toEqual(expect.arrayContaining([x, y]))
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, material: { wcs: 2, axis: 'XY' } } })
    expect(fixture.get('.setter-band.done p').text()).toBe(`G55 X0 Y0 set at the ${label} ${name}.`)
  })

  it('asks for a measured probe first, since the corner goes below Z0', async () => {
    setterState.value = { calibration: [0, 0, -10], measurement: null, measured_tools: [98] }
    const fixture = await probe('X− Y−')
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
    fixture.findComponent(NumberDialog).vm.$emit('confirm', 30, '30')
    await nextTick()
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'boss', diameter: 30, tool: 99, x: 0, y: 0, z: 0 })
    expect(fixture.get('.setter-band.busy p').text()).toBe('Probing the boss sides…')
  })
})

describe('edge angle', () => {
  it('probes the front edge at two points and reports the turn', async () => {
    const fixture = await probe('Edge angle…')
    fixture.findComponent(NumberDialog).vm.$emit('confirm', 50, '50')
    await nextTick()
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'angle', direction: 'y_plus', spacing: 50,
      tool: 99, x: 0, y: 0, z: 0 })
    await answer({ outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, edge_angle: -0.1 } })
    expect(fixture.get('.setter-band.done p').text()).toBe('Front edge turns −0.100° (0.087 mm over 50 mm).')
  })
  it('selects the left edge and applies the recorded rotation only after confirmation', async () => {
    if (!panel.status) throw new Error('Missing machine status')
    panel.status.offsets.wcs = 2
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global: { stubs: { Teleport: true } } })
    const fixture = await probe('Edge angle…')
    await probe('Left · X−')
    expect(fixture.get('.entry .label').text()).toBe('Distance along Y')
    fixture.findComponent(NumberDialog).vm.$emit('confirm', 50, '50')
    await nextTick()
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

  it('withdraws the confirmation when the measured work system changes', async () => {
    if (!panel.status) throw new Error('Missing machine status')
    panel.status.offsets.wcs = 2
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 }, global: { stubs: { Teleport: true } } })
    const fixture = await probe('Edge angle…')
    fixture.findComponent(NumberDialog).vm.$emit('confirm', 50, '50')
    await nextTick()
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
    const fixture = wrapper ??= mount(MaterialProbeControls, { props: { tool: 99 } })
    await fixture.get('button.calibrate').trigger('click')
    const dialog = fixture.findComponent(NumberDialog)
    expect(dialog.exists()).toBe(true)
    dialog.vm.$emit('confirm', 20.1, '20.1')
    await nextTick()
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
    wrapper = mount(MaterialProbeControls, { props: { tool: 99 } })
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'status' })
    await answer({ outcome: 'accepted', reason: '', setter: {
      calibration: [0, 0, -10], measurement: null, measured_tools: [98, 99],
    } })
    await probe('Z top')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'material', direction: 'z_top',
      tool: 99, x: 0, y: 0, z: 0 })
  })
})
