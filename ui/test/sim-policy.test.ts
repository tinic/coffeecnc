import { describe, expect, it } from 'vitest'
import { applyScene } from '../src/sim/scenes'
import { inspectProbeCode, probeCodeBlock, probeCodeUnknown, probeLoadedReason } from '../src/sim/probe-code'
import { rig, type Rig } from './harness'

// The browser simulator follows the native command policy (command_policy.cpp) and the native
// touch-probe check (Session::probe_block) for the commands below.

function probeRig(): Rig {
  const r = rig()
  applyScene(r.sim, 'probe', 'top')
  return r
}

describe('touch probe G-code check', () => {
  it('reads blocks as the native inspect_probe_code does', () => {
    expect(probeCodeBlock('G0 X10 Y5 (move) ; note')).toBe('')
    expect(probeCodeBlock('T1 M6 G43')).toBe('')
    expect(probeCodeBlock('S6000 M3')).toBe(probeLoadedReason)
    expect(probeCodeBlock('m 4')).toBe(probeLoadedReason)
    expect(probeCodeBlock('M+19')).toBe(probeLoadedReason)
    expect(probeCodeBlock('G84 Z-5 R1')).toBe(probeLoadedReason)
    expect(probeCodeBlock('G33.1 Z-5 K1')).toBe(probeLoadedReason)
    expect(probeCodeBlock('M100')).toBe(probeCodeUnknown)
    expect(probeCodeBlock('M[3]')).toBe(probeCodeUnknown)
    expect(probeCodeBlock('O100 call')).toBe(probeCodeUnknown)
    expect(probeCodeBlock('G0 X#1')).toBe(probeCodeUnknown)
    expect(probeCodeBlock('(py,print(1))')).toBe(probeCodeUnknown)
    expect(probeCodeBlock('G0 X1 ;py,print(1)')).toBe(probeCodeUnknown)
    expect(probeCodeBlock('(unclosed')).toBe(probeCodeUnknown)
    expect(probeCodeBlock('M61 Q1')).toBe(probeLoadedReason)
    expect(inspectProbeCode('M61 Q99 S200 M3')).toMatchObject({ relabelsTool: true, refusal: probeLoadedReason })
  })

  it('refuses MDI that starts the spindle while the probe is loaded', () => {
    const r = probeRig()
    expect(r.status().tool.in_spindle).toBe(99)
    expect(r.send({ type: 'mdi', text: 'M3 S6000' })).toMatchObject({ outcome: 'rejected', reason: probeLoadedReason })
    expect(r.send({ type: 'mdi', text: 'M61 Q99 S200 M3' })).toMatchObject({
      outcome: 'rejected', reason: 'Separate M61 from tool changes and spindle commands.' })
    expect(r.status().spindle.direction).toBe('stopped')
    expect(r.send({ type: 'mdi', text: 'G91 G0 Z1' }).outcome).toBe('accepted')
  })

  it('refuses to run or step a program that starts the spindle while the probe is loaded', () => {
    const r = probeRig()
    r.load('G0 Z5\nS6000 M3\nG0 X10\nM2\n')
    expect(r.send({ type: 'run' })).toMatchObject({ outcome: 'rejected', reason: probeLoadedReason })
    expect(r.send({ type: 'step' })).toMatchObject({ outcome: 'rejected', reason: probeLoadedReason })
    expect(r.status().interp).toBe('idle')
    r.load('G91 G0 Z1\nM2\n')
    expect(r.send({ type: 'run' }).outcome).toBe('accepted')
  })

  it('leaves programs alone when a cutting tool is loaded', () => {
    const r = rig()
    r.home()
    r.load('S1000 M3\nG4 P0.1\nM5\nM2\n')
    expect(r.send({ type: 'run' }).outcome).toBe('accepted')
  })
})

describe('commands that only stop', () => {
  it('stops the spindle and coolant in E-stop, machine off and while a program runs', () => {
    const r = rig()
    r.home()
    r.load('S1000 M3 M8\nG4 P30\nM2\n')
    expect(r.send({ type: 'run' }).outcome).toBe('accepted')
    r.sim.advance(2000)
    expect(r.status().spindle.direction).toBe('forward')
    expect(r.send({ type: 'set_spindle', direction: 'stopped', rpm: 0 }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_coolant', flood: false, mist: false }).outcome).toBe('accepted')
    expect(r.status().spindle.direction).toBe('stopped')
    expect(r.send({ type: 'set_spindle', direction: 'forward', rpm: 1000 }).outcome).toBe('rejected')
    r.send({ type: 'set_machine', state: 'estop' })
    expect(r.send({ type: 'set_spindle', direction: 'stopped', rpm: 0 }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_coolant', flood: false, mist: false }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_coolant', flood: true, mist: false }).outcome).toBe('rejected')
  })
})

describe('program options', () => {
  it('refuses to change block delete while a program runs', () => {
    const r = rig()
    r.home()
    r.load('G4 P30\nM2\n')
    expect(r.send({ type: 'set_program_option', option: 'block_delete', on: true }).outcome).toBe('accepted')
    expect(r.send({ type: 'run' }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_program_option', option: 'block_delete', on: false })).toMatchObject({
      outcome: 'rejected', reason: 'Stop execution before changing block delete.' })
    expect(r.send({ type: 'set_program_option', option: 'optional_stop', on: true }).outcome).toBe('accepted')
  })
})

describe('program lines', () => {
  it('keeps the motion line of the last motion during a dwell, as the native status does', () => {
    const r = rig()
    r.home()
    r.load('G91 G1 X1 F600\nG4 P5\nG1 X1\nM2\n')
    expect(r.send({ type: 'run' }).outcome).toBe('accepted')
    r.sim.advanceUntil((status) => status.program.current_line === 2, 10_000)
    r.sim.advance(1000)
    expect(r.status().program).toMatchObject({ current_line: 2, motion_line: 1 })
  })
})

describe('tool length offset codes', () => {
  it('reports G43 for an active offset of zero length', () => {
    const r = rig()
    r.home()
    r.send({ type: 'mdi', text: 'G43 H0' })
    r.finish()
    expect(r.status().active_codes).toContain('G43')
    r.send({ type: 'mdi', text: 'G49' })
    r.finish()
    expect(r.status().active_codes).toContain('G49')
  })
})

describe('native refusals', () => {
  it('refuses an MDI block that native refuses, before the machine state', () => {
    const r = rig()
    const refusal = { outcome: 'rejected', reason: 'MDI requires one nonempty block of at most 254 bytes.' }
    for (const text of ['', ' \t', 'G0 X1\nG0 X2', 'G0 X1\0', `(${'x'.repeat(253)})`, `(${'ä'.repeat(127)})`])
      expect(r.send({ type: 'mdi', text })).toMatchObject(refusal)
    r.home()
    expect(r.send({ type: 'mdi', text: `G0 X1 (${'x'.repeat(246)})` }).outcome).toBe('accepted')
  })

  it('refuses to save operator settings as native does without its storage', () => {
    const r = rig()
    expect(r.send({ type: 'set_operator_settings', machine_id: 'sim', base_revision: 0, document: {} as never })).toMatchObject({
      outcome: 'rejected', reason: 'Operator settings storage is not part of this session.' })
  })

  it('refuses to start the spindle with the touch probe loaded, as native', () => {
    expect(probeRig().send({ type: 'set_spindle', direction: 'forward', rpm: 6000 }))
      .toMatchObject({ outcome: 'rejected', reason: probeLoadedReason })
  })
})
