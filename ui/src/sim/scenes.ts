// Development scenes: each fast-forwards a fresh simulator into one panel state, by sending the
// same commands an operator would and letting simulated time pass.
//
//   estop          as switched on (the default without a scene)
//   off            E-stop reset, machine off
//   unhomed        machine on, nothing homed
//   homing         homing in progress
//   manual         homed, moved to a position above the stock
//   loaded         homed, demo program loaded
//   running        demo program running, in the pocket
//   paused         as running, then feed hold
//   toolchange     demo program waiting for the second tool
//   optstop        demo program paused at its M1
//   dwell          a long G4 dwell from MDI, after part of the drilling program
//   failed-preview upstream sample loaded; its preview fails
//   error          upstream sample started; the interpreter error stopped it
//   limit          program stopped by a soft limit
//   stale          as running, then the link goes silent

import type { Status, ToMachine } from '../protocol'
import { programDirectory, samplesDirectory } from './files'
import type { SimMachine } from './machine'

export const sceneNames = [
  'estop',
  'off',
  'unhomed',
  'homing',
  'manual',
  'loaded',
  'running',
  'paused',
  'toolchange',
  'optstop',
  'dwell',
  'failed-preview',
  'error',
  'limit',
  'stale',
] as const

export type SceneName = (typeof sceneNames)[number]

const plate = `${programDirectory}/plate-face-pocket.ngc`
const drill = `${programDirectory}/drill-pattern-inch.ngc`
const upstream = `${samplesDirectory}/gmoccapy_2_tools_with_cutter_radius_compensation.ngc`

type Body = ToMachine extends infer C ? (C extends ToMachine ? Omit<C, 'id'> : never) : never

class Director {
  private id = 0

  constructor(private readonly sim: SimMachine) {}

  /** Scene commands use negative ids, which the panel never assigns. */
  send(body: Body): void {
    this.id -= 1
    this.sim.handle({ ...body, id: this.id })
  }

  /** Runs until `done`, confirming tool changes on the way unless `done` is about one. */
  until(done: (status: Status) => boolean, limitMs = 600_000): void {
    for (let t = 0; t < limitMs; t += 20) {
      const status = this.sim.status()
      if (done(status)) {
        return
      }
      if (status.wait.kind === 'tool_change') {
        this.send({ type: 'tool_changed' })
      }
      this.sim.tick()
    }
  }

  idle(): void {
    this.sim.advance(100)
    this.until((status) => status.interp === 'idle' && status.axes.every((axis) => !axis.homing))
  }

  homed(): void {
    this.send({ type: 'set_machine', state: 'off' })
    this.send({ type: 'set_machine', state: 'on' })
    this.send({ type: 'home', axis: 'all' })
    this.until((status) => status.axes.every((axis) => axis.homed))
  }

  open(path: string): void {
    this.send({ type: 'open_program', path })
    this.sim.advance(400)
  }

  run(path: string): void {
    this.homed()
    this.open(path)
    this.send({ type: 'run' })
    this.sim.advance(40)
  }
}

export function applyScene(sim: SimMachine, name: string): void {
  const scene = sceneNames.find((known) => known === name)
  if (scene === undefined) {
    console.warn(`unknown scene "${name}"; known: ${sceneNames.join(', ')}`)
    return
  }
  const d = new Director(sim)
  sim.quiet = true
  switch (scene) {
    case 'estop':
      break
    case 'off':
      d.send({ type: 'set_machine', state: 'off' })
      break
    case 'unhomed':
      d.send({ type: 'set_machine', state: 'off' })
      d.send({ type: 'set_machine', state: 'on' })
      break
    case 'homing':
      d.send({ type: 'set_machine', state: 'off' })
      d.send({ type: 'set_machine', state: 'on' })
      d.send({ type: 'home', axis: 'all' })
      sim.advance(2200)
      break
    case 'manual':
      d.homed()
      d.send({ type: 'mdi', text: 'G0 X-12.5 Y8 Z22.4' })
      d.idle()
      d.send({ type: 'jog_step', axis: 'x', distance: 0.1, velocity: 600 })
      d.idle()
      break
    case 'loaded':
      d.homed()
      d.open(plate)
      break
    case 'running':
    case 'paused':
    case 'stale':
      d.run(plate)
      d.until((status) => status.program.motion_line >= 41)
      sim.advance(700)
      if (scene === 'paused') {
        d.send({ type: 'pause' })
        sim.advance(60)
      }
      break
    case 'toolchange':
      d.run(plate)
      d.until((status) => status.wait.kind === 'tool_change' && status.wait.tool === 2)
      break
    case 'optstop':
      d.homed()
      d.open(plate)
      d.send({ type: 'set_program_option', option: 'optional_stop', on: true })
      d.send({ type: 'run' })
      sim.advance(40)
      d.until((status) => status.wait.kind === 'paused')
      break
    case 'dwell':
      // A long dwell from MDI: one in a program is over before anyone looks.
      d.run(drill)
      d.until((status) => status.program.motion_line >= 13)
      d.send({ type: 'abort' })
      d.send({ type: 'mdi', text: 'G4 P600' })
      sim.advance(120)
      break
    case 'failed-preview':
      d.homed()
      d.open(upstream)
      break
    case 'error':
      d.run(upstream)
      d.idle()
      break
    case 'limit':
      d.homed()
      d.open(plate)
      d.send({ type: 'mdi', text: 'G10 L2 P1 X20 Y20 Z-20' })
      d.idle()
      d.send({ type: 'run' })
      sim.advance(40)
      d.idle()
      break
  }
  sim.quiet = false
}
