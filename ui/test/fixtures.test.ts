// The wire fixtures shared with the C++ decoder tests. `examples` is checked against the protocol
// types by the compiler and must name every command; the JSON file must equal it exactly.

import { describe, expect, it } from 'vitest'

import fixtures from '../../protocol/fixtures/commands.json'
import type { ToMachine } from '../src/protocol'

type Examples = { [K in ToMachine['type']]: Extract<ToMachine, { type: K }> }

const examples: Examples = {
  ready: { id: 1, type: 'ready' },
  set_machine: { id: 2, type: 'set_machine', state: 'on' },
  home: { id: 3, type: 'home', axis: 'all' },
  jog: { id: 4, type: 'jog', axis: 'x', direction: -1, velocity: 1500, held: true },
  jog_step: { id: 5, type: 'jog_step', axis: 'z', distance: -0.1, velocity: 600 },
  jog_stop_all: { id: 6, type: 'jog_stop_all' },
  mdi: { id: 7, type: 'mdi', text: 'G0 X10 Y-5.5' },
  list_directory: { id: 8, type: 'list_directory', path: null },
  open_program: { id: 9, type: 'open_program', path: '/home/cnc/nc_files/part.ngc' },
  run: { id: 10, type: 'run' },
  pause: { id: 11, type: 'pause' },
  resume: { id: 12, type: 'resume' },
  step: { id: 13, type: 'step' },
  abort: { id: 14, type: 'abort' },
  set_override: { id: 15, type: 'set_override', which: 'feed', value: 0.85 },
  set_spindle: { id: 16, type: 'set_spindle', direction: 'forward', rpm: 6000 },
  set_coolant: { id: 17, type: 'set_coolant', flood: true, mist: false },
  set_program_option: { id: 18, type: 'set_program_option', option: 'optional_stop', on: true },
  select_wcs: { id: 19, type: 'select_wcs', wcs: 2 },
  touch_off: { id: 20, type: 'touch_off', axis: 'y', value: 0 },
  tool_touch_off: { id: 21, type: 'tool_touch_off', value: 25.4 },
  set_tool: {
    id: 22,
    type: 'set_tool',
    tool: { number: 3, pocket: 3, length: 48.2, diameter: 6, comment: 'End mill 6 mm' },
  },
  remove_tool: { id: 23, type: 'remove_tool', number: 3 },
  tool_changed: { id: 24, type: 'tool_changed' },
}

describe('protocol/fixtures/commands.json', () => {
  it('holds exactly one example of every command, as the panel serialises it', () => {
    const sent: unknown = JSON.parse(JSON.stringify(Object.values(examples)))
    expect(fixtures).toEqual(sent)
  })
})
