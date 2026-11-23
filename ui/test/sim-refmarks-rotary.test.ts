import { describe, expect, it } from 'vitest'

import { matchMessage } from '../src/i18n/messages'
import {
  crossMarkFirst,
  referenceMarks,
  rotaryClearShiftRefusal,
  rotaryPreviewRefusal,
} from '../src/sim/machine'
import { programDirectory } from '../src/sim/files'
import { applyScene } from '../src/sim/scenes'
import { rig, type Rig } from './harness'

// The browser simulator reports reference marks and a rotary A axis as the native backend
// does: panel_mapping.cpp and messages.cpp for the fields, command_policy.cpp for Home and
// ClearG92, preview_engine.cpp and Session::plan_restart for the preview refusal.

function scene(name: string): Rig {
  const r = rig()
  applyScene(r.sim, name)
  return r
}

describe('reference-mark homing', () => {
  it('sends no mark fields on a machine without reference marks', () => {
    const r = rig()
    for (const axis of r.status().axes) {
      expect(axis).not.toHaveProperty('reference_mark')
      expect(axis).not.toHaveProperty('at_reference_mark')
    }
  })

  it('reports each mark as not found until the axis crosses it', () => {
    const r = rig()
    r.sim.enableReferenceMarks()
    expect(r.status().axes.map((axis) => [axis.reference_mark, axis.at_reference_mark]))
      .toEqual([[false, false], [false, false], [false, false]])
  })

  it('refuses Home and Home all for an axis whose mark is not found, with the native text', () => {
    const r = rig()
    r.sim.enableReferenceMarks()
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    expect(r.send({ type: 'home', axis: 'y' })).toMatchObject({ outcome: 'rejected', reason: crossMarkFirst('Y') })
    expect(r.send({ type: 'home', axis: 'all' })).toMatchObject({ outcome: 'rejected', reason: crossMarkFirst('X') })
    expect(crossMarkFirst('Z')).toBe('Move Z across its reference mark first: homing cannot find it yet.')
    r.sim.advance(200)
    expect(r.status().axes.some((axis) => axis.homing || axis.homed)).toBe(false)
  })

  it('learns a mark when a jog crosses it, in either direction, and keeps it over power off', () => {
    const r = rig()
    r.sim.enableReferenceMarks()
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    // X starts 10 mm below its mark: a 5 mm jog does not reach it.
    expect(r.send({ type: 'jog_step', axis: 'x', distance: 5, velocity: 600 }).outcome).toBe('accepted')
    r.sim.advance(1000)
    expect(r.status().axes[0]?.reference_mark).toBe(false)
    expect(r.send({ type: 'jog_step', axis: 'x', distance: 10, velocity: 600 }).outcome).toBe('accepted')
    r.sim.advance(1500)
    expect(r.status().axes[0]).toMatchObject({ reference_mark: true, at_reference_mark: false })
    // Z starts 10 mm below its mark too: a continuous jog upwards crosses it.
    r.send({ type: 'jog', axis: 'z', direction: 1, velocity: 600, held: true })
    for (let t = 0; t < 1200; t += 100) {
      r.sim.advance(100)
      r.send({ type: 'jog', axis: 'z', direction: 1, velocity: 600, held: true })
    }
    r.send({ type: 'jog', axis: 'z', direction: 1, velocity: 600, held: false })
    r.sim.advance(200)
    expect(r.status().axes[2]?.reference_mark).toBe(true)
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    expect(r.status().axes.map((axis) => axis.reference_mark)).toEqual([true, false, true])
    expect(r.send({ type: 'home', axis: 'x' }).outcome).toBe('accepted')
    expect(r.send({ type: 'home', axis: 'all' })).toMatchObject({ outcome: 'rejected', reason: crossMarkFirst('Y') })
  })

  it('refmarks scene: X at its mark, Y found, Z to find', () => {
    const r = scene('refmarks')
    const [x, y, z] = r.status().axes
    expect(x).toMatchObject({ homed: false, reference_mark: true, at_reference_mark: true })
    expect(y).toMatchObject({ homed: false, reference_mark: true, at_reference_mark: false })
    expect(z).toMatchObject({ homed: false, reference_mark: false, at_reference_mark: false })
    expect(r.send({ type: 'home', axis: 'all' })).toMatchObject({ outcome: 'rejected', reason: crossMarkFirst('Z') })
    expect(r.send({ type: 'home', axis: 'z' })).toMatchObject({ outcome: 'rejected', reason: crossMarkFirst('Z') })
  })

  it('homes an axis whose mark is found onto the mark', () => {
    const r = scene('refmarks')
    expect(r.send({ type: 'home', axis: 'y' }).outcome).toBe('accepted')
    r.sim.advance(100)
    expect(r.status().axes[1]?.homing).toBe(true)
    expect(r.sim.advanceUntil((status) => status.axes[1]?.homed === true, 10_000)).toBe(true)
    expect(r.status().axes[1]).toMatchObject({ machine: referenceMarks[1], at_reference_mark: true })
  })

  it('gives a refusal the catalogs translate', () => {
    expect(matchMessage(crossMarkFirst('X'))).toMatchObject({ key: 'messages.policy.crossReferenceMark', params: { axis: 'X' } })
  })
})

describe('rotary A axis', () => {
  it('sends no rotary_axes on the three-axis mill', () => {
    expect(rig().status()).not.toHaveProperty('rotary_axes')
  })

  it('reports A unhomed from where it was switched on, and homes it with Home all', () => {
    const r = rig()
    r.sim.enableRotary()
    expect(r.status().rotary_axes).toEqual([
      { axis: 'a', homed: false, homing: false, machine: 0, work: 0, dtg: 0, on_hard_limit: false },
    ])
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    expect(r.send({ type: 'home', axis: 'all' }).outcome).toBe('accepted')
    expect(r.sim.advanceUntil((status) => status.axes.every((axis) => axis.homed), 60_000)).toBe(true)
    // The linear joints are homed; A homes after them, and MDI waits for it.
    r.sim.advance(100)
    expect(r.status().rotary_axes?.[0]).toMatchObject({ homed: false, homing: true })
    expect(r.send({ type: 'mdi', text: 'G0 X10' }).outcome).toBe('rejected')
    expect(r.sim.advanceUntil((status) => status.rotary_axes?.[0]?.homed === true, 10_000)).toBe(true)
    expect(r.status().rotary_axes?.[0]).toMatchObject({ homing: false, machine: 0, work: 0 })
    expect(r.send({ type: 'mdi', text: 'G0 X10' }).outcome).toBe('accepted')
  })

  it('needs A homed before commands that require homing', () => {
    const r = rig()
    r.sim.enableRotary()
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    for (const axis of ['z', 'x', 'y'] as const) {
      r.send({ type: 'home', axis })
      r.sim.advanceUntil((status) => status.axes.every((a) => !a.homing) && status.motion.in_position, 10_000)
    }
    expect(r.status().axes.every((axis) => axis.homed)).toBe(true)
    expect(r.send({ type: 'mdi', text: 'G0 X10' })).toMatchObject({ outcome: 'rejected', reason: "Can't issue MDI command when not homed" })
  })

  it('refuses the preview with or without a program, and restart with the same text', () => {
    const r = scene('rotary')
    expect(r.status().program.file).toBe(`${programDirectory}/plate-face-pocket.ngc`)
    r.sim.advance(400)
    const preview = r.preview()
    expect(preview).toMatchObject({ state: 'unavailable', error: { line: 0, text: rotaryPreviewRefusal }, segments: [] })
    expect(r.send({ type: 'plan_restart', line: 20, preview_context: preview.context }))
      .toMatchObject({ outcome: 'rejected', reason: rotaryPreviewRefusal })
    const bare = rig()
    bare.sim.enableRotary()
    expect(bare.preview()).toMatchObject({ state: 'unavailable', error: { line: 0, text: rotaryPreviewRefusal } })
    expect(matchMessage(rotaryPreviewRefusal)?.key).toBe('backend.previewEngine.rotary')
  })

  it('refuses to clear the G92/G52 shift, as natively', () => {
    const r = scene('rotary')
    expect(r.send({ type: 'clear_g92' })).toMatchObject({ outcome: 'rejected', reason: rotaryClearShiftRefusal })
    expect(matchMessage(rotaryClearShiftRefusal)?.key).toBe('messages.policy.clearShiftRotary')
  })

  it('rotary scene: everything homed, A at 0 degrees', () => {
    const r = scene('rotary')
    expect(r.status().axes.every((axis) => axis.homed)).toBe(true)
    expect(r.status().rotary_axes).toEqual([
      { axis: 'a', homed: true, homing: false, machine: 0, work: 0, dtg: 0, on_hard_limit: false },
    ])
  })
})
