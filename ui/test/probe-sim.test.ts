import { describe, expect, it } from 'vitest'
import type { CommandBody, CommandResult, FromMachine, ProbeDirection, Vec3 } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'
import { applyScene } from '../src/sim/scenes'

type MovingProbe = Extract<CommandBody, { type: 'fixed_setter'; x: number }>
type Mode = MovingProbe extends infer C ? C extends MovingProbe ? Omit<C, 'type' | 'tool' | 'x' | 'y' | 'z'> : never : never

function scene(setup = 'top'): {
  sim: SimMachine; out: FromMachine[]; send: (command: CommandBody) => number
  result: (id: number) => CommandResult | undefined; submit: (mode: Mode) => number
  cycle: (mode: Mode) => CommandResult
} {
  const out: FromMachine[] = []
  const sim = new SimMachine({ emit: (message): void => { out.push(message) } })
  applyScene(sim, 'probe', setup)
  let nextId = 0
  function send(command: CommandBody): number {
    const id = ++nextId
    sim.handle({ ...command, id })
    return id
  }
  function result(id: number): CommandResult | undefined {
    return out.findLast(message => message.type === 'result' && message.id === id) as CommandResult | undefined
  }
  function submit(mode: Mode): number {
    const status = sim.status()
    const [x, y, z] = status.axes.map(axis => axis.machine) as Vec3
    return send({ type: 'fixed_setter', tool: status.tool.in_spindle, x, y, z, ...mode })
  }
  function cycle(mode: Mode): CommandResult {
    const before = out.length
    const id = submit(mode)
    expect(result(id), JSON.stringify(result(id))).toBeUndefined()
    sim.advanceUntil(() => result(id) !== undefined, 30_000)
    const reply = result(id)
    if (!reply) throw new Error('Probe did not finish')
    expect(reply.outcome, reply.reason).toBe('accepted')
    expect(out.slice(before).some(message => message.type === 'status' && message.probe_input === true)).toBe(true)
    expect(sim.status()).toMatchObject({ interp: 'idle', probe_input: false })
    return reply
  }
  return { sim, out, send, result, submit, cycle }
}

describe('browser probe scene', () => {
  it('advertises the probe and setter only in the probe scene', () => {
    const p = scene()
    p.send({ type: 'ready' })
    expect(p.out.findLast(message => message.type === 'hello')).toMatchObject({ touch_probe_tool: 99,
      fixed_setter: { reference_tool: 98, material_travel_mm: 35 } })
    expect(p.sim.status()).toMatchObject({ machine: 'on', tool: { in_spindle: 99 },
      spindle: { actual_rpm: 0 }, probe_input: false })
    expect(p.sim.status().active_codes).toContain('G43')
    const out: FromMachine[] = []
    const stock = new SimMachine({ emit: (message): void => { out.push(message) } })
    stock.handle({ type: 'ready', id: 1 })
    expect(out.find(message => message.type === 'hello')).not.toHaveProperty('fixed_setter')
    expect(stock.status().probe_input).toBe(null)
  })

  it('probes Z top and sets the active work offset once the probe retracts', () => {
    const { sim, cycle, send } = scene()
    expect(sim.status().offsets.g5x[2]).toBe(-118)
    send({ type: 'select_wcs', wcs: 2 })
    const result = cycle({ action: 'material', direction: 'z_top' })
    expect(result.setter?.material).toEqual({ wcs: 2, axis: 'Z' })
    expect(sim.status().offsets.g5x[2]).toBe(-120)
    expect(sim.modal.offsets[0]?.[2]).toBe(-118)
    expect(sim.status().axes[2]?.work).toBe(8)
  })

  it.each([
    ['left', 'x_plus', 'X', 0, 109.98], ['right', 'x_minus', 'X', 0, 190.02],
    ['front', 'y_plus', 'Y', 1, 69.98], ['back', 'y_minus', 'Y', 1, 130.02],
  ] as const)('probes the %s stock face', (setup, direction, axis, index, offset) => {
    const { sim, cycle } = scene(setup)
    expect(cycle({ action: 'material', direction }).setter?.material).toEqual({ wcs: 1, axis })
    expect(sim.status().offsets.g5x[index]).toBeCloseTo(offset)
  })

  it.each([
    ['front-left', 'x_plus', 'y_plus', 109.98, 69.98],
    ['front-right', 'x_minus', 'y_plus', 190.02, 69.98],
    ['back-left', 'x_plus', 'y_minus', 109.98, 130.02],
    ['back-right', 'x_minus', 'y_minus', 190.02, 130.02],
  ] as const)('probes both faces of the %s corner', (setup, x, y, ox, oy) => {
    const { sim, cycle } = scene(setup)
    expect(cycle({ action: 'corner', x_direction: x, y_direction: y }).setter?.material?.axis).toBe('XY')
    expect(sim.status().offsets.g5x[0]).toBeCloseTo(ox)
    expect(sim.status().offsets.g5x[1]).toBeCloseTo(oy)
  })

  it('finds the bore centre and measures its diameter from the walls', () => {
    const { sim, cycle } = scene('bore')
    const result = cycle({ action: 'bore' })
    expect(result.setter?.center_diameter).toBeCloseTo(19.96)
    expect(sim.status().offsets.g5x).toEqual([155, 108, -120])
  })

  it('finds the boss centre and measures its diameter from outside', () => {
    const { sim, cycle } = scene('boss')
    const result = cycle({ action: 'boss', diameter: 24 })
    expect(result.setter?.center_diameter).toBeCloseTo(24.04)
    expect(sim.status().offsets.g5x).toEqual([235, 170, -120])
  })

  it.each<[string, Exclude<ProbeDirection, 'z_top'>]>([['angle', 'y_plus'], ['angle-left', 'x_plus']])(
    'measures the skew of the %s fixture and applies it only on confirmation', (setup, direction) => {
      const { sim, cycle, send, result } = scene(setup)
      const measured = cycle({ action: 'angle', direction, spacing: 50 })
      expect(measured.setter?.edge_angle).toBeCloseTo(3)
      expect(sim.status().offsets.rotation_xy).toBe(0)
      const id = measured.setter?.edge_rotation?.measurement_id
      if (id === undefined) throw new Error('Missing rotation offer')
      const request = { type: 'fixed_setter', action: 'apply_angle', tool: 99, measurement_id: id } as const
      expect(result(send(request))?.outcome).toBe('accepted')
      expect(sim.status().offsets.rotation_xy).toBeCloseTo(3)
      expect(result(send(request))?.outcome).toBe('failed')
    })

  it('calibrates the ruby ball and updates the table read by the panel', () => {
    const { sim, out, cycle } = scene('ball')
    expect(cycle({ action: 'ball', gauge_width: 24 }).setter?.ball_diameter).toBeCloseTo(4.04)
    expect(sim.modal.tools.find(tool => tool.number === 99)?.diameter).toBe(4.04)
    const table = out.findLast(message => message.type === 'tool_table')
    expect(table?.type === 'tool_table' && table.tools.find(tool => tool.number === 99)?.diameter).toBe(4.04)
  })

  it('teaches from the already measured probe and reports the setter location', () => {
    const { cycle } = scene('setter')
    expect(cycle({ action: 'touch_off' }).setter).toMatchObject({ calibration: [55, 55, -150],
      measured_tools: [98, 99], measurement: { tool: 99, calibration: true, value: -150, contact: [55, 55, -102] } })
  })

  it('measures a cutter with the taught setter and applies its new length', () => {
    const { sim, cycle } = scene('measure')
    const result = cycle({ action: 'touch_off' })
    expect(result.setter?.measurement).toMatchObject({ tool: 1, calibration: false, previous_length: 62.5 })
    expect(result.setter?.measurement?.value).toBeCloseTo(62.43)
    expect(result.setter?.measured_tools).toEqual([1, 98, 99])
    expect(sim.status().offsets.tool[2]).toBeCloseTo(62.43)
  })

  it('rejects a miss and cancels an in-flight cycle without changing offsets', () => {
    const wrong = scene('left')
    expect(wrong.result(wrong.submit({ action: 'material', direction: 'z_top' }))?.outcome).toBe('failed')
    const p = scene()
    const before = p.sim.status().offsets
    const id = p.submit({ action: 'material', direction: 'z_top' })
    p.sim.advance(200)
    p.send({ type: 'abort' })
    p.sim.advance(4000)
    expect(p.result(id)).toMatchObject({ outcome: 'failed', reason: 'Probing was stopped.' })
    expect(p.sim.status().offsets).toEqual(before)
    expect(p.sim.status().probe_input).toBe(false)
  })
})
