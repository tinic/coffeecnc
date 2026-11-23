import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent } from 'vue'
import OffsetHistoryDialog from '../src/components/OffsetHistoryDialog.vue'
import ToolsTab from '../src/components/ToolsTab.vue'
import { panel, send } from '../src/machine'
import { restoreBlock } from '../src/offset-history'
import type { HistoryEntry, OffsetHistory, Tool } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import { maxHistoryEntries } from '../src/sim/offset-history'
import { rig } from './harness'

vi.mock('../src/machine', async () => {
  const { reactive, computed } = await import('vue')
  const panel = reactive({ hello: null, toolModels: null, simulated: true }) as typeof import('../src/machine').panel
  const linkBlock = computed(() => panel.link !== 'live' || panel.status?.link !== 'live' ? 'Connection unavailable' : null)
  return { panel, linkBlock,
    mdiBlock: computed(() => linkBlock.value ?? (panel.status?.machine !== 'on' ? 'Machine is off'
      : panel.status.interp !== 'idle' ? 'Machine is busy' : null)),
    findTool: (number: number): Tool | null => panel.toolTable?.tools.find((tool) => tool.number === number) ?? null,
    send: vi.fn(), onResult: (): (() => void) => () => undefined }
})

function history(after: OffsetHistory['entries']): OffsetHistory {
  return { type: 'offset_history', revision: 3, storage: 'saved', reason: '', entries: after }
}
const at = new Date(2026, 10, 21, 9, 30, 0).getTime()
const entries: HistoryEntry[] = [
  { id: 4, time: at + 3000, source: 'tool_table', target: 'tool', number: 3, restores: null,
    changes: [{ field: 'length', before: 48.2, after: 50 }] },
  { id: 3, time: at + 2000, source: 'outside', target: 'wcs', number: 1, restores: null,
    changes: [{ field: 'y', before: 20, after: -7 }] },
  { id: 2, time: at + 1000, source: 'zero', target: 'wcs', number: 1, restores: null,
    changes: [{ field: 'x', before: 12.5, after: 0 }] },
  { id: 1, time: at, source: 'tool_table', target: 'tool', number: 7, restores: null,
    changes: [{ field: 'length', before: null, after: 25.4 }, { field: 'diameter', before: null, after: 12.7 }] },
]

let wrapper: VueWrapper | null = null
function view(): VueWrapper { if (!wrapper) throw new Error('Missing wrapper'); return wrapper }
// The dialog alone, or the Tools tab that opens it, with the places dialogs teleport to.
const fixture = defineComponent({ components: { OffsetHistoryDialog, ToolsTab }, props: { tab: Boolean },
  template: '<div><ToolsTab v-if="tab"/><OffsetHistoryDialog v-else /><div id="dialogs"></div><div id="workspace-dialogs"></div></div>' })
function only<T>(value: T | undefined): T { if (value === undefined) throw new Error('Missing value'); return value }
function list(): VueWrapper { return mount(fixture, { attachTo: document.body }) }
function tab(): VueWrapper { return mount(fixture, { props: { tab: true }, attachTo: document.body }) }
async function press(text: string): Promise<void> {
  const button = view().findAll('button').find((item) => item.text() === text)
  if (!button) throw new Error(`Missing button: ${text}`)
  await button.trigger('click')
}

beforeEach(() => {
  panel.hello = { ...hello }; panel.link = 'live'; panel.unit = 'mm'
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.link = 'live'; panel.status.offsets.wcs = 1
  panel.workOffsets = { type: 'work_offsets', revision: 1, systems: Array.from({ length: 9 }, (_, index) => ({
    wcs: index + 1, available: true, offset: [0, -7, 3.5], rotation_xy: 0,
  })) }
  panel.toolTable = { type: 'tool_table', revision: 2, tools: [{ number: 3, pocket: 3, length: 50, diameter: 6, comment: '' }] }
  panel.offsetHistory = history(entries.map((entry) => ({ ...entry })))
  vi.mocked(send).mockClear()
})
afterEach(() => { wrapper?.unmount(); wrapper = null; document.body.replaceChildren() })

describe('offset history dialog', () => {
  it('lists entries newest first with their source and filters by kind', async () => {
    wrapper = list()
    const rows = view().findAll('.entry')
    expect(rows.map((row) => row.find('strong').text())).toEqual(['T03', 'G54', 'G54', 'T07'])
    expect(rows[1]?.text()).toContain('Outside the panel')
    expect(rows[1]?.text()).toContain('20.000 → -7.000')
    expect(rows[2]?.text()).toContain('Zero')
    expect(rows[3]?.text()).toContain('— → 25.400')
    await press('Tools')
    expect(view().findAll('.entry').map((row) => row.find('strong').text())).toEqual(['T03', 'T07'])
    await press('Work offsets')
    expect(view().findAll('.entry')).toHaveLength(2)
  })

  it('offers Restore only where an earlier value is known', () => {
    wrapper = list()
    const buttons = view().findAll('.restore')
    expect(buttons.map((button) => button.attributes('disabled') !== undefined)).toEqual([false, false, false, true])
    expect(restoreBlock(only(entries[3]))).toBe('No earlier value is known')
    expect(restoreBlock({ ...only(entries[0]), changes: [{ field: 'length', before: 48.2, after: null }] })).toBe('The tool was removed')
  })

  it('reviews the values before and after, then sends the restore of that entry', async () => {
    wrapper = list()
    await view().get('[aria-label^="Restore G54 from"]').trigger('click')
    expect(view().get('[role="dialog"]').attributes('aria-label')).toBe('Restore G54 Y?')
    const cells = view().findAll('.changes td').map((cell) => cell.text())
    expect(cells).toEqual(['-7.000 mm', '20.000 mm'])
    expect(view().text()).toContain('G10 L2')
    expect(send).not.toHaveBeenCalled()
    await press('Restore G54')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'restore_offset', entry: 3 })
    expect(view().get('[role="dialog"]').attributes('aria-label')).toBe('Offset history')
  })

  it('shows a tool restore in the display unit and refuses it while the machine cannot take it', async () => {
    panel.unit = 'inch'
    wrapper = list()
    await view().get('[aria-label^="Restore T03 from"]').trigger('click')
    expect(view().findAll('.changes td').map((cell) => cell.text())).toEqual(['1.9685 in', '1.8976 in'])
    expect(view().text()).toContain('G10 L1')
    only(panel.status ?? undefined).machine = 'off'
    await view().vm.$nextTick()
    expect(view().get('[role="status"]').text()).toBe('Machine is off')
    await press('Restore T03')
    expect(send).not.toHaveBeenCalled()
    await press('Back')
    expect(view().findAll('.restore').every((button) => button.attributes('disabled') !== undefined)).toBe(true)
  })

  it('says when the history is not kept on disk', () => {
    panel.offsetHistory = { ...history([]), storage: 'session_only', reason: 'No machine identity configured' }
    wrapper = list()
    expect(view().text()).toContain('Kept for this session only: No machine identity configured')
    expect(view().text()).toContain('No changes recorded yet.')
  })

  it('opens from the Tools tab only when the machine keeps a history', async () => {
    panel.offsetHistory = null
    wrapper = tab()
    expect(view().findAll('button').some((button) => button.text() === 'History')).toBe(false)
    wrapper.unmount(); wrapper = null; document.body.replaceChildren()
    panel.offsetHistory = history(entries)
    wrapper = tab()
    await press('History')
    expect(view().get('[role="dialog"]').attributes('aria-label')).toBe('Offset history')
    await view().get('[aria-label="Close"]').trigger('click')
    await view().get('[aria-label="Tool history"]').trigger('click')
    expect(view().findAll('.entry')).toHaveLength(2)
  })
})

describe('simulated offset history', () => {
  function latest(out: ReturnType<typeof rig>['out']): OffsetHistory {
    const found = out.findLast((message) => message.type === 'offset_history')
    if (found?.type !== 'offset_history') throw new Error('No offset history')
    return found
  }

  it('attributes touch-offs, MDI, tool edits and restores, and restores the earlier value', () => {
    const r = rig()
    r.home()
    r.sim.tick()
    const g54 = r.status().offsets.g5x[0]
    expect(r.send({ type: 'touch_off', axis: 'x', value: 0, origin: 'zero' }).outcome).toBe('accepted')
    const zero = only(latest(r.out).entries[0])
    expect(zero).toMatchObject({ source: 'zero', target: 'wcs', number: 1, restores: null })
    expect(zero.changes[0]?.before).toBe(g54)
    expect(r.send({ type: 'mdi', text: 'G10 L2 P2 Y-7' }).outcome).toBe('accepted')
    r.finish()
    expect(latest(r.out).entries[0]).toMatchObject({ source: 'mdi', number: 2, changes: [{ field: 'y', after: -7 }] })
    expect(r.send({ type: 'set_tool', tool: { number: 2, pocket: 2, length: 50, diameter: 6, comment: '' } }).outcome).toBe('accepted')
    expect(latest(r.out).entries[0]).toMatchObject({ source: 'tool_table', target: 'tool', number: 2 })
    expect(r.send({ type: 'restore_offset', entry: zero.id }).outcome).toBe('accepted')
    expect(r.status().offsets.g5x[0]).toBe(g54)
    expect(latest(r.out).entries[0]).toMatchObject({ source: 'restore', restores: zero.id })
    expect(r.send({ type: 'restore_offset', entry: 999 })).toMatchObject({ outcome: 'rejected', reason: 'That history entry is no longer kept.' })
    expect(latest(r.out).storage).toBe('session_only')
  })

  it('applies a restored length of the loaded tool again when its length is in effect', () => {
    const r = rig()
    r.home()
    r.send({ type: 'mdi', text: 'T2 M6' })
    r.sim.advance(40)
    r.send({ type: 'tool_changed' })
    r.finish()
    r.send({ type: 'mdi', text: 'G43 H2' })
    r.finish()
    const applied = r.status().offsets.tool[2]
    r.send({ type: 'set_tool', tool: { number: 2, pocket: 2, length: applied + 3, diameter: 6, comment: '' } })
    const edited = only(latest(r.out).entries[0])
    r.send({ type: 'mdi', text: 'G43 H2' })
    r.finish()
    expect(r.status().offsets.tool[2]).toBe(applied + 3)
    expect(r.send({ type: 'restore_offset', entry: edited.id }).outcome).toBe('accepted')
    expect(r.status().offsets.tool[2]).toBe(applied)
    // Under G49 nothing is applied.
    r.send({ type: 'mdi', text: 'G49' })
    r.finish()
    expect(r.send({ type: 'restore_offset', entry: only(latest(r.out).entries[0]).id }).outcome).toBe('accepted')
    expect(r.status().offsets.tool[2]).toBe(0)
  })

  it('refuses a restore of a tool that was added, and a restore with the machine off', () => {
    const r = rig()
    r.home()
    r.sim.tick()
    r.send({ type: 'set_tool', tool: { number: 9, pocket: 9, length: 30, diameter: 3, comment: '' } })
    const added = only(latest(r.out).entries[0])
    expect(r.send({ type: 'restore_offset', entry: added.id }).reason).toBe('This entry has no earlier value to restore.')
    r.send({ type: 'touch_off', axis: 'z', value: 1 })
    r.send({ type: 'set_machine', state: 'off' })
    expect(r.send({ type: 'restore_offset', entry: only(latest(r.out).entries[0]).id }).outcome).toBe('rejected')
  })

  it('keeps at most 200 entries, newest first', () => {
    const r = rig()
    r.home()
    r.sim.tick()
    for (let i = 1; i <= maxHistoryEntries + 20; i += 1) r.send({ type: 'touch_off', axis: 'x', value: i })
    const shown = latest(r.out)
    expect(shown.entries).toHaveLength(maxHistoryEntries)
    expect(only(shown.entries[0]).id).toBe(maxHistoryEntries + 20)
    expect(only(shown.entries.at(-1)).id).toBe(21)
  })
})
