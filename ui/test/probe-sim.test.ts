import { describe, expect, it } from 'vitest'
import type { CommandBody, CommandResult, FromMachine, ProbeDirection, Vec3 } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'
import { toWork } from '../src/sim/gcode'
import { wrapped } from '../src/sim/probe'
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

  it.each([
    ['inside-front-left', 'x_minus', 'y_minus', 160.02, 76.02],
    ['inside-front-right', 'x_plus', 'y_minus', 183.98, 76.02],
    ['inside-back-left', 'x_minus', 'y_plus', 160.02, 93.98],
    ['inside-back-right', 'x_plus', 'y_plus', 183.98, 93.98],
  ] as const)('sets both offsets at the %s pocket corner', (setup, x, y, ox, oy) => {
    const { sim, cycle } = scene(setup)
    expect(cycle({ action: 'inside_corner', x_direction: x, y_direction: y }).setter?.material?.axis).toBe('XY')
    const after = sim.status().offsets.g5x
    expect(after[0]).toBeCloseTo(ox)
    expect(after[1]).toBeCloseTo(oy)
    expect(after[2]).toBe(-120)
  })

  it('measures a rectangular pocket and sets both axes in the active WCS', () => {
    const { sim, cycle, send } = scene('pocket')
    const original = sim.status().offsets.g5x
    send({ type: 'select_wcs', wcs: 2 })
    const result = cycle({ action: 'pocket' })
    expect(result.setter?.feature_size?.x).toBeCloseTo(23.96)
    expect(result.setter?.feature_size?.y).toBeCloseTo(17.96)
    expect(sim.status().offsets.g5x.slice(0, 2)).toEqual([172, 85])
    expect(sim.modal.offsets[0]).toEqual(original)
  })

  it.each([
    ['slot-x', 'x', 'X', 125, 11.96], ['slot-y', 'y', 'Y', 110, 27.96],
    ['ridge-x', 'x', 'X', 343, 16.04], ['ridge-y', 'y', 'Y', 146, 32.04],
  ] as const)('measures %s and preserves the two unprobed axes', (setup, axis, name, centre, width) => {
    const { sim, cycle } = scene(setup)
    const before = sim.status()
    const index = axis === 'x' ? 0 : 1
    const result = cycle(setup.startsWith('slot') ? { action: 'slot', axis }
      : { action: 'ridge', axis, width: width - 0.04 })
    expect(result.setter?.material?.axis).toBe(name)
    expect(result.setter?.feature_size?.[axis]).toBeCloseTo(width)
    const after = sim.status()
    expect(after.offsets.g5x[index]).toBeCloseTo(centre)
    expect(after.offsets.g5x[1 - index]).toBe(before.offsets.g5x[1 - index])
    expect(after.offsets.g5x[2]).toBe(before.offsets.g5x[2])
    expect(after.axes[2]?.machine).toBeCloseTo(before.axes[2]?.machine ?? Infinity)
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

  it('aligns to the slanted block, measures no turn left and probes its corner in the rotated axes', () => {
    const { sim, cycle, send, result } = scene('angle')
    const move = (x: number, y: number, z: number): void => {
      const id = send({ type: 'mdi', text: `G53 G0 X${String(x)} Y${String(y)} Z${String(z)}` })
      sim.advanceUntil(() => result(id) !== undefined && sim.status().interp === 'idle' && sim.status().motion.in_position, 30_000)
    }
    const [x, y, z] = sim.status().axes.map(axis => axis.machine) as Vec3
    const measured = cycle({ action: 'angle', direction: 'y_plus', spacing: 50 })
    const id = measured.setter?.edge_rotation?.measurement_id
    if (id === undefined) throw new Error('Missing rotation offer')
    const applied = result(send({ type: 'fixed_setter', action: 'apply_angle', tool: 99, measurement_id: id }))
    expect(applied?.outcome).toBe('accepted')
    expect(applied?.setter?.edge_angle).toBeCloseTo(3)
    expect(sim.status().offsets.rotation_xy).toBeCloseTo(3)
    move(x, y, z)
    const again = cycle({ action: 'angle', direction: 'y_plus', spacing: 50 })
    expect(again.setter?.edge_angle).toBeCloseTo(0)
    expect(again.setter?.edge_rotation?.rotation).toBeCloseTo(3)
    // Over the slanted block 5 mm inside its front-left corner, above the top.
    const [centreX, centreY] = [255, 85]
    const turn = (u: number, v: number): [number, number] => {
      const a = 3 * Math.PI / 180
      return [centreX + u * Math.cos(a) - v * Math.sin(a), centreY + u * Math.sin(a) + v * Math.cos(a)]
    }
    const [sx, sy] = turn(-35, -25)
    move(sx, sy, z + 12)
    expect(cycle({ action: 'corner', x_direction: 'x_plus', y_direction: 'y_plus' }).setter?.material?.axis).toBe('XY')
    const [cx, cy] = turn(-40, -30)
    const work = toWork(sim.modal, [cx, cy, 0])
    expect(Math.abs(work[0])).toBeLessThan(0.05)
    expect(Math.abs(work[1])).toBeLessThan(0.05)
    expect(sim.status().offsets.rotation_xy).toBeCloseTo(3)
  })

  it.each([7.5, -30])('finds the bore and boss centres along work axes rotated %s°', (degrees) => {
    for (const [setup, centre, diameter] of [['bore', [155, 108], 19.96], ['boss', [235, 170], 24.04]] as const) {
      const { sim, cycle } = scene(setup)
      sim.modal.rotations[0] = degrees
      const measured = cycle(setup === 'bore' ? { action: 'bore' } : { action: 'boss', diameter: 24 })
      expect(measured.setter?.center_diameter).toBeCloseTo(diameter)
      expect(sim.status().offsets.g5x[0]).toBeCloseTo(centre[0])
      expect(sim.status().offsets.g5x[1]).toBeCloseTo(centre[1])
      expect(sim.status().offsets.rotation_xy).toBe(degrees)
    }
  })

  it.each([0, 7.5])('maps the stock top and the pocket floor, rotated %s°, without writing', (degrees) => {
    const { sim, cycle } = scene('pocket')
    sim.modal.rotations[0] = degrees
    const before = structuredClone(sim.status().offsets)
    // Over the stock, the pocket floor and the stock again: 154, 170 and 186 machine X, 16 mm apart.
    const work = toWork(sim.modal, [154, 83, 0])
    const map = cycle({ action: 'height_map', x_min: work[0], y_min: work[1], x_max: work[0] + 32, y_max: work[1] + 4,
      step: 16, clearance: 5 }).setter?.height_map
    expect(sim.status().offsets).toEqual(before)
    expect(map).toMatchObject({ wcs: 1, columns: 3, rows: 2 })
    expect(map?.points).toHaveLength(6)
    expect(map?.rotation).toBe(degrees)
    expect(map?.partial).toBeUndefined()
    for (const [index, point] of (map?.points ?? []).entries()) {
      expect(point?.work[0]).toBeCloseTo(work[0] + 16 * (index % 3))
      expect(point?.work[1]).toBeCloseTo(work[1] + 4 * Math.floor(index / 3))
    }
    // The middle column is over the pocket floor, the outer ones over the stock top.
    const heights = map?.points.map(point => Math.round((point?.work[2] ?? NaN) * 1000) / 1000) ?? []
    expect(heights[1]).toBeLessThan(heights[0] ?? 0)
    expect(heights[0]).toBeCloseTo(heights[2] ?? NaN)
  })

  it('refuses a height map beyond the travel before moving, as the controller does', () => {
    const { sim, submit, result } = scene('pocket')
    const work = toWork(sim.modal, [154, 83, 0])
    const before = sim.status().axes.map(axis => axis.machine)
    // Machine X 154 to 454: past X max 400.
    const wide = submit({ action: 'height_map', x_min: work[0], y_min: work[1], x_max: work[0] + 300, y_max: work[1], step: 100, clearance: 5 })
    expect(result(wide)?.reason).toMatch(/^Grid point X-?\d+\.\d{3} Y-?\d+\.\d{3} mm is outside the configured travel at the clearance or the search depth\.$/)
    // The clearance at machine Z -190: the search ends below Z min -200.
    const deep = submit({ action: 'height_map', x_min: work[0], y_min: work[1], x_max: work[0] + 10, y_max: work[1], step: 10, clearance: work[2] - 190 })
    expect(result(deep)?.reason).toContain('is outside the configured travel at the clearance or the search depth.')
    sim.advance(1000)
    expect(sim.status().axes.map(axis => axis.machine)).toEqual(before)
  })

  it('keeps the measured points of a height map that misses a point, labelled partial', () => {
    const { sim, submit, result } = scene('pocket')
    // Machine X 154 over the stock, then 204 between the stock and the slanted block: no top.
    const work = toWork(sim.modal, [154, 83, 0])
    const id = submit({ action: 'height_map', x_min: work[0], y_min: work[1], x_max: work[0] + 50, y_max: work[1], step: 50, clearance: 5 })
    sim.advanceUntil(() => result(id) !== undefined, 30_000)
    const reply = result(id)
    expect(reply?.outcome).toBe('failed')
    expect(reply?.reason).toMatch(/^Point X-?\d+\.\d{3} Y-?\d+\.\d{3} mm: No probe contact/)
    const map = reply?.setter?.height_map
    expect(map?.partial).toBe(reply?.reason)
    expect(map?.points[0]?.machine[0]).toBeCloseTo(154)
    expect(map?.points[1]).toBeNull()
  })

  it('refuses a height map with too many points before moving', () => {
    const { submit, result } = scene('top')
    const id = submit({ action: 'height_map', x_min: 0, y_min: 0, x_max: 100, y_max: 100, step: 5, clearance: 5 })
    expect(result(id)?.reason).toBe('Choose a rectangle and a step of 0.5 mm or more for 2 to 150 points.')
  })

  it.each([0, 7.5, -30])('measures the bore and boss only, then sets them on confirmation, rotated %s°', (degrees) => {
    for (const [setup, centre, diameter] of [['bore', [155, 108], 19.96], ['boss', [235, 170], 24.04]] as const) {
      const { sim, cycle, send, result } = scene(setup)
      sim.modal.rotations[0] = degrees
      const before = structuredClone(sim.status().offsets)
      const measured = cycle(setup === 'bore' ? { action: 'bore', measure_only: true } : { action: 'boss', diameter: 24, measure_only: true })
      expect(sim.status().offsets).toEqual(before)
      expect(measured.setter?.material).toBeUndefined()
      expect(measured.setter?.center_diameter).toBeCloseTo(diameter)
      const report = measured.setter?.measured
      const work = toWork(sim.modal, [centre[0], centre[1], 0])
      expect(report).toMatchObject({ wcs: 1, axes: 'XY' })
      expect(report?.faces.map(face => face.direction)).toEqual(['x_plus', 'x_minus', setup === 'bore' ? 'y_plus' : 'y_plus', 'y_minus'])
      expect(report?.work.x).toBeCloseTo(work[0])
      expect(report?.work.y).toBeCloseTo(work[1])
      expect(report?.work.z).toBeUndefined()
      expect(report?.machine?.[0]).toBeCloseTo(centre[0])
      expect(report?.machine?.[1]).toBeCloseTo(centre[1])
      const id = report?.measurement_id
      if (id === undefined) throw new Error('Missing measurement')
      const request = { type: 'fixed_setter', action: 'apply_measurement', tool: 99, measurement_id: id } as const
      expect(result(send(request))?.setter?.material).toEqual({ wcs: 1, axis: 'XY' })
      expect(sim.status().offsets.g5x[0]).toBeCloseTo(centre[0])
      expect(sim.status().offsets.g5x[1]).toBeCloseTo(centre[1])
      expect(sim.status().offsets.g5x[2]).toBe(before.g5x[2])
      expect(sim.status().offsets.rotation_xy).toBe(degrees)
      expect(result(send(request))?.reason).toBe('Probe the feature again before applying the measurement.')
    }
  })

  it('measures Z top and an edge without writing, and refuses a measurement from another work frame', () => {
    const { sim, cycle, send, result } = scene()
    const top = cycle({ action: 'material', direction: 'z_top', measure_only: true })
    expect(sim.status().offsets.g5x[2]).toBe(-118)
    expect(top.setter?.measured).toMatchObject({ axes: 'Z', faces: [{ direction: 'z_top' }] })
    expect(top.setter?.measured?.work.z).toBeCloseTo(-2)
    const id = top.setter?.measured?.measurement_id
    if (id === undefined) throw new Error('Missing measurement')
    expect(result(send({ type: 'fixed_setter', action: 'apply_measurement', tool: 99, measurement_id: id }))?.setter?.material)
      .toEqual({ wcs: 1, axis: 'Z' })
    expect(sim.status().offsets.g5x[2]).toBe(-120)

    const left = scene('left')
    const x = left.sim.status().offsets.g5x[0]
    const edge = left.cycle({ action: 'material', direction: 'x_plus', measure_only: true })
    expect(left.sim.status().offsets.g5x[0]).toBe(x)
    expect(edge.setter?.measured).toMatchObject({ axes: 'X', faces: [{ direction: 'x_plus' }] })
    expect(edge.setter?.measured?.machine?.[0]).toBeCloseTo(109.98)
    const stale = edge.setter?.measured?.measurement_id
    if (stale === undefined) throw new Error('Missing measurement')
    left.send({ type: 'select_wcs', wcs: 2 })
    expect(left.result(left.send({ type: 'fixed_setter', action: 'apply_measurement', tool: 99, measurement_id: stale }))?.reason)
      .toBe('The work frame or tool changed. Probe the feature again.')
    expect(left.sim.modal.offsets[0]?.[0]).toBe(x)
  })

  it.each(['ball', 'touch_off', 'check'] as const)('refuses %s as measure-only, as the controller does', (action) => {
    const { sim, send, result } = scene('top')
    const before = structuredClone(sim.status().offsets)
    const tools = JSON.stringify(sim.modal.tools)
    const [x, y, z] = sim.status().axes.map(axis => axis.machine) as Vec3
    const command = { type: 'fixed_setter', action, tool: 99, x, y, z, gauge_width: 24, measure_only: true }
    const reply = result(send(command as unknown as CommandBody))
    expect(reply).toMatchObject({ outcome: 'rejected', reason: 'This probe action always writes its result.',
      refusal: { key: 'messages.policy.probeAlwaysWrites' } })
    expect(sim.status().offsets).toEqual(before)
    expect(JSON.stringify(sim.modal.tools)).toBe(tools)
  })

  it('reports both edge-angle contacts in measure-only mode and offers no origin', () => {
    const { sim, cycle } = scene('angle')
    const measured = cycle({ action: 'angle', direction: 'y_plus', spacing: 50, measure_only: true })
    expect(measured.setter?.edge_angle).toBeCloseTo(3)
    expect(measured.setter?.measured).toMatchObject({ axes: '', work: {}, faces: [{ direction: 'y_plus' }, { direction: 'y_plus' }] })
    expect(measured.setter?.measured?.measurement_id).toBeUndefined()
    expect(sim.status().offsets.rotation_xy).toBe(0)
  })

  it('keeps an applied rotation inside -180..180° like the controller', () => {
    expect([363, -363, -180, 180, 540, 10.5].map(wrapped)).toEqual([3, -3, 180, 180, 180, 10.5])
    expect(wrapped(179 + 2.8624)).toBeCloseTo(-178.1376)
  })

  it('refuses a ball calibration in a rotated work system', () => {
    const { sim, submit, result } = scene('ball')
    sim.modal.rotations[0] = 7.5
    const id = submit({ action: 'ball', gauge_width: 24 })
    sim.advanceUntil(() => result(id) !== undefined, 30_000)
    expect(result(id)?.reason).toContain('without rotation')
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

  it('checks a cutter without changing the table or tool compensation', () => {
    const { sim, cycle, submit, result } = scene('measure')
    const row = sim.modal.tools.find(tool => tool.number === 1)
    if (!row) throw new Error('Missing demo cutter')
    const before = sim.status().offsets.tool
    const passed = cycle({ action: 'check' })
    expect(passed.setter?.measurement).toMatchObject({ tool: 1, previous_length: 62.5, check_tolerance_mm: 0.1 })
    expect(passed.setter?.measurement?.value).toBeCloseTo(62.43)
    expect(passed.setter?.measured_tools).toEqual([98, 99])
    expect(row.length).toBe(62.5)
    row.length = 62.83
    const id = submit({ action: 'check' })
    expect(result(id)).toBeUndefined()
    expect(sim.advanceUntil(() => result(id) !== undefined, 30_000)).toBe(true)
    expect(result(id)?.outcome).toBe('failed')
    expect(result(id)?.reason).toContain('0.4000 mm shorter')
    expect(sim.status()).toMatchObject({ interp: 'idle', probe_input: false })
    expect(row.length).toBe(62.83)
    expect(sim.status().offsets.tool).toEqual(before)
  })

  it('teaches with T98 and then measures the 6 mm end mill', () => {
    const p = scene('setter')
    function loadAtSetter(tool: number): void {
      p.send({ type: 'mdi', text: `T${String(tool)} M6` })
      expect(p.sim.advanceUntil(status => status.wait.kind === 'tool_change', 1000)).toBe(true)
      p.send({ type: 'tool_changed' })
      p.sim.advance(100)
      p.send({ type: 'mdi', text: 'G43' })
      p.sim.advance(100)
      p.send({ type: 'mdi', text: 'G53 G0 X55 Y55 Z-90' })
      expect(p.sim.advanceUntil(status => status.interp === 'idle', 3000)).toBe(true)
    }
    loadAtSetter(98)
    expect(p.cycle({ action: 'touch_off' }).setter?.measurement)
      .toMatchObject({ tool: 98, calibration: true, value: -150 })
    loadAtSetter(2)
    const result = p.cycle({ action: 'touch_off' }).setter
    expect(result?.measurement).toMatchObject({ tool: 2, calibration: false })
    expect(result?.measurement?.value).toBeCloseTo(48.2)
    expect(result?.measured_tools).toEqual([2, 98, 99])
  })

  it('rejects a miss and cancels an in-flight cycle without changing offsets', () => {
    const wrong = scene('left')
    expect(wrong.result(wrong.submit({ action: 'material', direction: 'z_top' }))?.outcome).toBe('failed')
    const p = scene()
    const before = p.sim.status().offsets
    const id = p.submit({ action: 'material', direction: 'z_top' })
    expect(p.sim.advanceUntil(status => status.probe_input === true, 5000)).toBe(true)
    p.send({ type: 'abort' })
    p.sim.advance(4000)
    expect(p.result(id)).toMatchObject({ outcome: 'failed', reason: 'Probing was stopped.' })
    expect(p.sim.status().offsets).toEqual(before)
    expect(p.sim.status().probe_input).toBe(false)
  })
})
