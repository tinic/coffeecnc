import { describe, expect, it } from 'vitest'
import { axisJogVelocity, requiresHoming } from '../src/machine-capabilities'
import type { CommandBody, FromMachine, Hello } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'

describe('machine capabilities', () => {
  it('keeps older v1 peers restrictive and clamps an asymmetric axis independently', () => {
    const legacy: Hello = { ...hello }
    delete legacy.axis_max_jog_velocity
    delete legacy.homing_required
    expect(requiresHoming(legacy)).toBe(true)
    expect(requiresHoming(null)).toBe(true)
    expect(requiresHoming({ ...legacy, homing_required: false })).toBe(false)
    expect(axisJogVelocity(legacy, 'z', 2500)).toBe(2500)
    const asymmetric = { ...hello, max_jog_velocity: 1500, axis_max_jog_velocity: { x: 1500, y: 1500, z: 750 } }
    expect(axisJogVelocity(asymmetric, 'x', 3000)).toBe(1500)
    expect(axisJogVelocity(asymmetric, 'z', 1500)).toBe(750)
    expect(axisJogVelocity(asymmetric, 'z', 600)).toBe(600)
  })

  it('does not promote missing, malformed or nonconfigured axis limits to the global limit', () => {
    for (const map of [{ x: 1500 }, { z: 0 }, { z: NaN }, { z: Infinity }, { z: -1 }]) {
      expect(axisJogVelocity({ ...hello, axis_max_jog_velocity: map }, 'z', 1000)).toBe(0)
    }
    expect(axisJogVelocity({ ...hello, axes: ['x', 'y'] }, 'z', 1000)).toBe(0)
    expect(axisJogVelocity(null, 'z', 1000)).toBe(0)
    expect(axisJogVelocity(hello, 'x', NaN)).toBe(0)
  })

  it('applies axis rates to actual simulated continuous and incremental jog motion', () => {
    const out: FromMachine[] = []
    const sim = new SimMachine({ emit: (message): void => { out.push(message) }, capabilities: {
      max_jog_velocity: 1500, axis_max_jog_velocity: { x: 1500, y: 1500, z: 750 },
    } })
    let id = 0
    const send = (body: CommandBody): void => {
      sim.handle({ ...body, id: ++id })
      expect(out.findLast((message) => message.type === 'result')).toMatchObject({ outcome: 'accepted' })
    }
    send({ type: 'set_machine', state: 'off' }); send({ type: 'set_machine', state: 'on' })
    send({ type: 'jog', axis: 'z', direction: 1, held: true, velocity: 1500 })
    sim.advance(100)
    expect(sim.status().axes.find((axis) => axis.axis === 'z')?.machine).toBeCloseTo(1.25)
    expect(sim.status().motion.velocity).toBeCloseTo(750)
    send({ type: 'jog', axis: 'z', direction: 1, held: false, velocity: 1500 })
    sim.advance(20)
    send({ type: 'jog_step', axis: 'z', distance: 5, velocity: 1500 })
    sim.advance(100)
    expect(sim.status().axes.find((axis) => axis.axis === 'z')?.machine).toBeCloseTo(2.5)
    send({ type: 'jog_stop_all' })
    send({ type: 'jog', axis: 'x', direction: 1, held: true, velocity: 1500 })
    sim.advance(100)
    expect(sim.status().axes.find((axis) => axis.axis === 'x')?.machine).toBeCloseTo(2.5)
  })

  it('changes MDI/program homing policy only when configured and leaves feedback unknown', () => {
    for (const required of [undefined, true, false]) {
      const out: FromMachine[] = []
      const sim = new SimMachine({ emit: (message): void => { out.push(message) },
        files: new Map([['/sim/policy.ngc', 'G4 P0.1\nM2']]),
        capabilities: required === undefined ? {} : { homing_required: required } })
      sim.handle({ type: 'set_machine', state: 'off', id: 1 })
      sim.handle({ type: 'set_machine', state: 'on', id: 2 })
      sim.handle({ type: 'mdi', text: 'G4 P0.1', id: 3 })
      expect(out.findLast((message) => message.type === 'result')).toMatchObject({ outcome: required === false ? 'accepted' : 'rejected' })
      expect(sim.status().axes.every((axis) => !axis.homed)).toBe(true)
      expect(sim.status().spindle.actual_rpm).toBeNull()
      expect(sim.status().spindle.at_speed).toBeNull()
      expect(sim.status().probe_input).toBeNull()
      sim.advance(200)
      sim.handle({ type: 'open_program', path: '/sim/policy.ngc', id: 4 })
      sim.handle({ type: 'run', id: 5 })
      expect(out.findLast((message) => message.type === 'result')).toMatchObject({ outcome: required === false ? 'accepted' : 'rejected' })
      sim.advance(500)
      sim.handle({ type: 'home', axis: 'all', id: 6 })
      sim.handle({ type: 'mdi', text: 'G4 P0.1', id: 7 })
      expect(out.findLast((message) => message.type === 'result')).toMatchObject({ outcome: 'rejected' })
    }
  })
})
