import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'

import ManualTab from '../src/components/ManualTab.vue'
import { operatorSettings, panel, send } from '../src/machine'
import type { OperatorPreferences } from '../src/operator-settings'
import type { Status } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import type { ResultListener } from '../src/store'

const results = vi.hoisted(() => new Set<(command: unknown, result: unknown) => void>())

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const { defaultOperatorPreferences } = await import('../src/operator-settings')
  const { requiresHoming } = await import('../src/machine-capabilities')
  const { machineBlock } = await import('../src/machine-block')
  const panel = reactive({ preferences: defaultOperatorPreferences(), startupSpindleRpm: 0, hello: null, status: null, link: 'live', unit: 'mm', mdi: [], jogVelocity: 1500, jogIncrement: null, tab: 'manual', notice: null, log: [] }) as unknown as typeof import('../src/machine').panel
  const allHomed = computed(() => panel.status?.axes.every((axis) => axis.homed) ?? false)
  const manualBlock = computed(() => machineBlock(panel))
  return { panel, send: vi.fn(), dismissNotice: vi.fn(),
    onResult: (listener: ResultListener): (() => void) => {
      const entry = listener as (command: unknown, result: unknown) => void
      results.add(entry); return (): void => { results.delete(entry) }
    },
    operatorSettings: {
      save: vi.fn((document: OperatorPreferences) => { panel.preferences = document; return Promise.resolve({ saved: true, reason: '' }) }),
      context: vi.fn(() => ({ hello: panel.hello, revision: 1 })),
    },
    axes: computed(() => panel.status?.axes ?? []), reported: computed(() => panel.status),
    live: computed(() => panel.link === 'live'),
    manualBlock, linkBlock: computed(() => null), allHomed,
    mdiBlock: computed(() => manualBlock.value ?? (allHomed.value ? null : 'Axes are not homed')),
    homingActive: computed(() => false),
    homingRequired: computed(() => requiresHoming(panel.hello)),
  }
})

let wrapper: VueWrapper | null = null
function statusAt(at: { x: number; y: number; z: number }, change: Partial<Status> = {}): Status {
  const base = new SimMachine({ emit: (): void => undefined, clock: (): number => 0 }).status()
  return { ...base, machine: 'on', interp: 'idle', link: 'live', program_units: 'mm',
    axes: base.axes.map((axis) => ({ ...axis, homed: true, machine: at[axis.axis] })), ...change }
}
function sentMdi(): string[] {
  return vi.mocked(send).mock.calls.map(([command]) => command).filter((command) => command.type === 'mdi').map((command) => command.text)
}
function answerLast(): void {
  const command = vi.mocked(send).mock.calls.at(-1)?.[0]
  if (command === undefined) throw new Error('Nothing sent')
  for (const listener of results) listener(command, { type: 'result', id: 1, outcome: 'accepted', reason: '' })
}
async function mountManual(): Promise<VueWrapper> {
  wrapper = mount(defineComponent({
    components: { ManualTab },
    template: '<div><ManualTab/><div id="dialogs"></div><div id="workspace-dialogs"></div></div>',
  }), { attachTo: document.body })
  await nextTick()
  return wrapper
}
const button = (view: VueWrapper, text: string): ReturnType<VueWrapper['get']> => {
  const found = view.findAll('button').filter((candidate) => candidate.text() === text)
  const last = found.at(-1)
  if (last === undefined) throw new Error(`No button ${text}`)
  return last
}
const band = (view: VueWrapper): string => view.get('.jog-status').text()

beforeEach(() => {
  vi.mocked(send).mockClear(); vi.mocked(operatorSettings.save).mockClear()
  Object.defineProperty(HTMLElement.prototype, 'scrollTo', { configurable: true, value: vi.fn() })
  panel.hello = { ...hello, max_jog_velocity: 3000 }
  panel.status = statusAt({ x: 100, y: 50, z: -20 })
  panel.link = 'live'; panel.unit = 'mm'
  panel.preferences = { ...panel.preferences, saved_positions: [] }
})
afterEach(() => {
  const element = wrapper?.element as Element | undefined
  wrapper?.unmount(); element?.remove(); wrapper = null
})

describe('power feed in Manual', () => {
  it('shows the target and feed, and sends one MDI move only on Start feed', async () => {
    const view = await mountManual()
    await button(view, 'Power feed').trigger('click')
    await button(view, 'X+').trigger('click')
    expect(document.body.textContent).toContain('X 100.000 → 399.500 mm')
    expect(document.body.textContent).toContain('F 200 mm/min')
    expect(send).not.toHaveBeenCalled()
    await button(view, 'Cancel').trigger('click')
    expect(send).not.toHaveBeenCalled()
    await button(view, 'X+').trigger('click')
    await button(view, 'Start feed').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'mdi', text: 'G21 G90 G94 G53 G1 X399.500 F200' })
  })
  it('feeds a set distance in inches when the panel shows inches', async () => {
    panel.unit = 'inch'
    const view = await mountManual()
    await button(view, 'Power feed').trigger('click')
    await button(view, 'Distance').trigger('click')
    await view.get('input[aria-label="Power feed distance"]').setValue('1')
    await view.get('input[aria-label="Power feed distance"]').trigger('change')
    await view.get('input[aria-label="Power feed rate"]').setValue('10')
    await view.get('input[aria-label="Power feed rate"]').trigger('change')
    await button(view, 'Y−').trigger('click')
    expect(document.body.textContent).toContain('Y 1.9685 → 0.9685 in')
    await button(view, 'Start feed').trigger('click')
    expect(sentMdi()).toEqual(['G21 G90 G94 G53 G1 Y24.600 F254'])
  })
  it('refuses with the reason when unhomed, off, running or with the probe loaded', async () => {
    const view = await mountManual()
    await button(view, 'Power feed').trigger('click')
    if (panel.status === null) throw new Error('No status')
    panel.hello = { ...hello, max_jog_velocity: 3000, touch_probe_tool: 99 }
    panel.status = { ...panel.status, tool: { in_spindle: 99, prepared: null } }
    await nextTick()
    await button(view, 'Z−').trigger('click')
    expect(band(view)).toContain('A touch probe is loaded')
    panel.status = statusAt({ x: 100, y: 50, z: -20 }, { machine: 'off' })
    await nextTick(); await button(view, 'Z−').trigger('click')
    expect(band(view)).toContain('Machine is off')
    panel.status = statusAt({ x: 100, y: 50, z: -20 }, { mode: 'auto', interp: 'reading' })
    await nextTick(); await button(view, 'Z−').trigger('click')
    expect(band(view)).toContain('Program running')
    const unhomed = statusAt({ x: 100, y: 50, z: -20 })
    panel.status = { ...unhomed, axes: unhomed.axes.map((axis) => ({ ...axis, homed: false })) }
    await nextTick(); await button(view, 'Z−').trigger('click')
    expect(band(view)).toContain('Axes are not homed')
    expect(document.querySelector('[role="dialog"]')).toBeNull()
    expect(send).not.toHaveBeenCalled()
  })
})

describe('saved positions in Manual', () => {
  it('saves the machine position under a default name in the operator settings', async () => {
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Save position').trigger('click')
    expect(document.querySelector<HTMLInputElement>('input[aria-label="Position name"]')?.value).toBe('Position 1')
    await button(view, 'Save').trigger('click'); await nextTick()
    expect(operatorSettings.save).toHaveBeenCalledOnce()
    expect(vi.mocked(operatorSettings.save).mock.calls[0]?.[0].saved_positions).toEqual([{ name: 'Position 1', position: [100, 50, -20] }])
    expect(view.text()).toContain('Position 1')
    expect(send).not.toHaveBeenCalled()
  })
  it('deletes a position after confirmation', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }, { name: 'Corner', position: [1, 2, -3] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await view.get('button[aria-label="Delete Vise jaw"]').trigger('click')
    await button(view, 'Cancel').trigger('click')
    expect(operatorSettings.save).not.toHaveBeenCalled()
    await view.get('button[aria-label="Delete Vise jaw"]').trigger('click')
    await button(view, 'Delete').trigger('click'); await nextTick()
    expect(panel.preferences.saved_positions).toEqual([{ name: 'Corner', position: [1, 2, -3] }])
  })
  it('confirms Go to, then sends Z up, X Y, and Z down only when included, one after another', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    expect(document.body.textContent).toContain('Z -20.000 → -0.500')
    expect(document.body.textContent).not.toContain('Z -0.500 → -50.000')
    await button(view, 'Cancel').trigger('click')
    expect(send).not.toHaveBeenCalled()

    await button(view, 'Go to').trigger('click')
    document.querySelector<HTMLInputElement>('.check input')?.click(); await nextTick()
    expect(document.body.textContent).toContain('Z -0.500 → -50.000')
    const confirm = [...document.querySelectorAll('footer button')].find((candidate) => candidate.textContent.trim() === 'Go to') as HTMLButtonElement
    confirm.click(); await nextTick()
    expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500'])
    answerLast()
    panel.status = statusAt({ x: 100, y: 50, z: -10 }, { mode: 'mdi', interp: 'reading' })
    panel.status = statusAt({ x: 100, y: 50, z: -0.5 }, { mode: 'mdi' })
    expect(sentMdi()).toHaveLength(2)
    answerLast(); panel.status = statusAt({ x: 10, y: 20, z: -0.5 }, { mode: 'mdi' })
    answerLast(); panel.status = statusAt({ x: 10, y: 20, z: -50 }, { mode: 'mdi' })
    await nextTick()
    expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500', 'G21 G90 G53 G0 X10.000 Y20.000', 'G21 G90 G53 G0 Z-50.000'])
    expect(band(view)).toContain('At Vise jaw')
  })
  it('sends nothing further after Stop ends a Go to move short', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    const confirm = [...document.querySelectorAll('footer button')].find((candidate) => candidate.textContent.trim() === 'Go to') as HTMLButtonElement
    confirm.click(); await nextTick()
    answerLast()
    panel.status = statusAt({ x: 100, y: 50, z: -10 }, { mode: 'mdi', interp: 'reading' })
    panel.status = statusAt({ x: 100, y: 50, z: -8 }, { mode: 'mdi' })
    await nextTick()
    expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500'])
    expect(band(view)).toContain('Go to stopped before the position was reached')
  })
})
