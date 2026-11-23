import { afterEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent } from 'vue'

import { alarmGroups, describeAlarmCommand } from '../src/alarm-log'
import LogDialog from '../src/components/LogDialog.vue'
import { panel } from '../src/machine'
import type { AlarmEntry, AlarmLog, FromMachine } from '../src/protocol'
import { SimAlarmLog } from '../src/sim/alarm-log'
import { createPanel } from '../src/store'

vi.mock('../src/machine', async () => {
  const { reactive } = await import('vue')
  const panel = reactive({ log: [], alarmLog: null, simulated: false }) as unknown as typeof import('../src/machine').panel
  return { panel, setSimulatorFault: vi.fn() }
})

const at = new Date(2026, 10, 21, 9, 30, 0).getTime()
function entry(session: number, time: number, text: string, extra: Partial<AlarmEntry> = {}): AlarmEntry {
  return { session, time, level: 'error', text, outcome: null, command: null, ...extra }
}
function log(entries: AlarmEntry[], change: Partial<AlarmLog> = {}): AlarmLog {
  return { type: 'alarm_log', storage: 'saved', reason: '', session: 3, skipped: 0, entries, ...change }
}

describe('alarm log groups', () => {
  it('groups by run, newest run and newest entry first', () => {
    const groups = alarmGroups(log([
      entry(1, at, 'a'), entry(1, at + 1, 'b'), entry(2, at + 2, 'c'), entry(3, at + 3, 'd'), entry(3, at + 4, 'e'),
    ]))
    expect(groups.map((group) => [group.session, group.current, group.since, group.entries.map((item) => item.text)])).toEqual([
      [3, true, at + 3, ['e', 'd']], [2, false, at + 2, ['c']], [1, false, at, ['b', 'a']],
    ])
    expect(alarmGroups(null)).toEqual([])
  })

  it('keeps the file order when the clock went back between runs', () => {
    const groups = alarmGroups(log([entry(1, at + 10_000, 'first'), entry(2, at, 'second')]))
    expect(groups.map((group) => group.entries[0]?.text)).toEqual(['second', 'first'])
  })

  it('describes a stored command, also one this panel cannot read', () => {
    expect(describeAlarmCommand({ type: 'home', axis: 'x' })).toBe('Home X')
    expect(describeAlarmCommand({ type: 'home' })).toBe('home')
    expect(describeAlarmCommand({ type: 'future_command' })).toBe('future_command')
    expect(describeAlarmCommand({})).toBe('')
    expect(describeAlarmCommand(null)).toBe('')
  })
})

describe('the store', () => {
  it('keeps the alarm log of the current hello only', () => {
    const listeners: ((message: FromMachine) => void)[] = []
    const store = createPanel({ transport: { simulated: false, send: (): void => undefined, listen: (listener): void => { listeners.push(listener) } } })
    const receive = (message: FromMachine): void => { for (const listener of listeners) listener(message) }
    const sent = log([entry(1, at, 'kept')])
    receive(sent)
    expect(store.state.alarmLog).toEqual(sent)
    receive({ type: 'hello', protocol: 1, machine_name: 'M', simulated: false, axes: ['x', 'y', 'z'], machine_units: 'mm',
      jog_increments: [1], max_jog_velocity: 100, max_spindle_rpm: 100, axis_max_jog_velocity: { x: 100, y: 100, z: 100 },
      homing_required: false, max_feed_override: 1, min_spindle_override: 1, max_spindle_override: 1, machine_id: null,
      tool_model_storage: 1, operator_settings_storage: 1 })
    expect(store.state.alarmLog).toBeNull()
  })
})

describe('the simulator log', () => {
  function storage(): Pick<Storage, 'getItem' | 'setItem'> & { data: Map<string, string> } {
    const data = new Map<string, string>()
    return { data, getItem: (key) => data.get(key) ?? null, setItem: (key, value): void => { data.set(key, value) } }
  }

  it('sends what earlier runs recorded, with the command of a refusal', () => {
    const disk = storage()
    const first = new SimAlarmLog(() => disk)
    expect(first.view()).toMatchObject({ session: 1, storage: 'saved', entries: [] })
    first.request({ type: 'home', axis: 'z', id: 4 })
    first.observe({ type: 'result', id: 4, outcome: 'rejected', reason: 'Machine is off' }, at)
    first.request({ type: 'pause', id: 5 })
    first.observe({ type: 'result', id: 5, outcome: 'accepted', reason: '' }, at)
    first.observe({ type: 'message', level: 'info', text: 'Program loaded', time: at + 1 }, at + 1)
    expect(first.view().entries).toEqual([])
    const second = new SimAlarmLog(() => disk)
    expect(second.view()).toMatchObject({ session: 2, entries: [
      { session: 1, time: at, text: 'Machine is off', outcome: 'rejected', command: { type: 'home', axis: 'z' } },
      { session: 1, time: at + 1, level: 'info', text: 'Program loaded', command: null },
    ] })
  })

  it('works without browser storage', () => {
    const broken = new SimAlarmLog(() => { throw new Error('private window') })
    broken.observe({ type: 'message', level: 'error', text: 'x', time: at }, at)
    expect(broken.view()).toMatchObject({ storage: 'session_only', session: 1, entries: [] })
  })
})

describe('the message log dialog', () => {
  let wrapper: VueWrapper | null = null
  afterEach(() => { wrapper?.unmount(); wrapper = null; document.body.replaceChildren() })
  const fixture = defineComponent({ components: { LogDialog }, template: '<div><LogDialog /><div id="dialogs"></div></div>' })

  it('shows this session, then each earlier one under a separator', () => {
    panel.log = [{ id: 1, level: 'error', text: 'Now', context: '', time: at + 50_000, count: 1 }]
    panel.alarmLog = log([
      entry(1, at, 'Joint 2 following error'),
      entry(2, at + 10_000, 'The machine is off.', { outcome: 'rejected', command: { type: 'home', axis: 'x' } }),
      entry(3, at + 20_000, 'Before reload', { level: 'info' }),
    ], { skipped: 2 })
    wrapper = mount(fixture, { attachTo: document.body })
    const text = document.body.textContent
    const order = ['This session', 'Now', 'This session, before the panel was reloaded', 'Before reload',
      'Earlier session', 'The machine is off.', 'Answer to: Home X', 'Earlier session', 'Joint 2 following error']
    let from = 0
    for (const part of order) {
      const found = text.indexOf(part, from)
      expect(found, part).toBeGreaterThanOrEqual(0)
      from = found + part.length
    }
    expect(text).toContain('2 damaged lines of the saved log were skipped.')
    expect(document.querySelectorAll('h3.separator')).toHaveLength(4)
  })

  it('says when the log is not saved, and is empty without anything', () => {
    panel.log = []
    panel.alarmLog = log([], { storage: 'session_only', reason: 'No machine identity configured', session: 1 })
    wrapper = mount(fixture, { attachTo: document.body })
    expect(document.body.textContent).toContain('No messages in this session.')
    expect(document.body.textContent).toContain('This log is not saved: No machine identity configured')
  })
})
