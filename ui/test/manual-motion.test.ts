import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'

import ManualTab from '../src/components/ManualTab.vue'
import { operatorSettings, panel, send } from '../src/machine'
import type { OperatorPreferences } from '../src/operator-settings'
import type { Status } from '../src/protocol'
import { touchInput, trackNumericInput } from '../src/numeric-input'
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
/** A tap on the touch screen: touchstart and pointerdown on press, the click on release. */
async function tap(target: Element | { element: Node }): Promise<void> {
  const element = target instanceof Element ? target : target.element as Element
  const touch = (type: string): Event => Object.assign(new Event(type, { bubbles: true, cancelable: true }), { touches: [], changedTouches: [] })
  element.dispatchEvent(touch('touchstart'))
  element.dispatchEvent(Object.assign(new Event('pointerdown', { bubbles: true, cancelable: true }), { pointerType: 'touch' }))
  element.dispatchEvent(Object.assign(new Event('pointerup', { bubbles: true }), { pointerType: 'touch' }))
  element.dispatchEvent(touch('touchend'))
  element.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }))
  await nextTick()
}
const nameField = (): HTMLInputElement | null => document.querySelector<HTMLInputElement>('input[aria-label="Position name"]')
async function typeName(name: string): Promise<void> {
  const field = nameField()
  if (field === null) throw new Error('No position name field')
  field.value = name
  field.dispatchEvent(new Event('input'))
  await nextTick()
}

beforeEach(() => {
  vi.mocked(send).mockClear(); vi.mocked(operatorSettings.save).mockClear()
  Object.defineProperty(HTMLElement.prototype, 'scrollTo', { configurable: true, value: vi.fn() })
  panel.hello = { ...hello, max_jog_velocity: 3000 }
  panel.status = statusAt({ x: 100, y: 50, z: -20 })
  panel.link = 'live'; panel.unit = 'mm'
  panel.motionStopRequest = 0
  panel.preferences = { ...panel.preferences, saved_positions: [], go_to_safe_z: null }
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
  it('holds the feed to the maximum velocity of the axis it moves', async () => {
    panel.hello = { ...hello, max_jog_velocity: 3000, axis_max_jog_velocity: { x: 3000, y: 3000, z: 750 } }
    const view = await mountManual()
    await button(view, 'Power feed').trigger('click')
    await button(view, 'To limit').trigger('click')
    await view.get('input[aria-label="Power feed rate"]').setValue('1000')
    await view.get('input[aria-label="Power feed rate"]').trigger('change')
    await button(view, 'Z−').trigger('click')
    expect(band(view)).toContain('The feed is above the machine maximum')
    expect(document.querySelector('[role="dialog"]')).toBeNull()
    await button(view, 'X+').trigger('click')
    expect(document.body.textContent).toContain('F 1000 mm/min')
    await button(view, 'Cancel').trigger('click')
    await view.get('input[aria-label="Power feed rate"]').setValue('200')
    await view.get('input[aria-label="Power feed rate"]').trigger('change')
    expect(send).not.toHaveBeenCalled()
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
    expect(nameField()?.placeholder).toBe('Position 1')
    expect(nameField()?.value).toBe('')
    await button(view, 'Save').trigger('click'); await nextTick()
    expect(operatorSettings.save).toHaveBeenCalledOnce()
    expect(vi.mocked(operatorSettings.save).mock.calls[0]?.[0].saved_positions).toEqual([{ name: 'Position 1', position: [100, 50, -20] }])
    expect(view.text()).toContain('Position 1')
    expect(send).not.toHaveBeenCalled()
  })
  it('allows the position name to receive native focus from a fresh touch', async () => {
    const stop = trackNumericInput()
    try {
      const view = await mountManual()
      await button(view, 'Positions').trigger('click')
      await tap(button(view, 'Save position'))
      const field = nameField()
      if (field === null) throw new Error('No position name field')
      const mouseDown = (): Event => Object.assign(new Event('pointerdown', { bubbles: true, cancelable: true }), { pointerType: 'mouse' })
      expect(field.dispatchEvent(mouseDown())).toBe(false)
      expect(field.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }))).toBe(false)
      field.dispatchEvent(new Event('touchstart', { bubbles: true }))
      field.dispatchEvent(new Event('touchend', { bubbles: true }))
      expect(field.dispatchEvent(mouseDown())).toBe(true)
      expect(field.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }))).toBe(true)
      await typeName('Vise jaw')
      await tap(button(view, 'Save'))
      expect(vi.mocked(operatorSettings.save).mock.calls[0]?.[0].saved_positions).toEqual([{ name: 'Vise jaw', position: [100, 50, -20] }])
      expect(view.text()).toContain('Vise jaw')
    } finally { stop(); touchInput.value = false }
  })
  it('saves an empty or blank name as the default, trims it, saves on Enter and refuses a duplicate', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Corner', position: [1, 2, -3] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Save position').trigger('click')
    expect(document.body.textContent).toContain('An empty name saves as Position 2')
    const field = nameField()
    if (field === null) throw new Error('No name field')
    field.value = '  Corner '; field.dispatchEvent(new Event('input')); await nextTick()
    expect(document.body.textContent).toContain('Another position is named Corner')
    field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' })); await nextTick()
    expect(operatorSettings.save).not.toHaveBeenCalled()
    field.value = 'x'.repeat(65); field.dispatchEvent(new Event('input')); await nextTick()
    expect(document.body.textContent).toContain('Enter a name of at most 64 bytes')
    field.value = '   '; field.dispatchEvent(new Event('input')); await nextTick()
    field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' })); await nextTick()
    expect(vi.mocked(operatorSettings.save).mock.calls[0]?.[0].saved_positions).toEqual([{ name: 'Corner', position: [1, 2, -3] }, { name: 'Position 2', position: [100, 50, -20] }])
    await button(view, 'Save position').trigger('click')
    const next = nameField()
    if (next === null) throw new Error('No name field')
    next.value = '  Fixture  '; next.dispatchEvent(new Event('input')); await nextTick()
    next.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', isComposing: true })); await nextTick()
    expect(nameField()).toBe(next)
    next.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter' })); await nextTick()
    expect(panel.preferences.saved_positions.map((entry) => entry.name)).toEqual(['Corner', 'Position 2', 'Fixture'])
  })
  it('renames a saved position by tapping its name, keeps it on Cancel, and saves the new name', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }, { name: 'Corner', position: [1, 2, -3] }] }
    const stop = trackNumericInput()
    try {
      const view = await mountManual()
      await button(view, 'Positions').trigger('click')
      await tap(view.get('button[aria-label="Rename Vise jaw"]').element)
      expect(document.body.textContent).toContain('Rename Vise jaw')
      expect(nameField()?.value).toBe('Vise jaw')
      await tap(button(view, 'Cancel'))
      expect(nameField()).toBeNull()
      expect(operatorSettings.save).not.toHaveBeenCalled()
      await tap(view.get('button[aria-label="Rename Vise jaw"]').element)
      await typeName('Corner')
      expect(document.body.textContent).toContain('Another position is named Corner')
      await tap(button(view, 'Save'))
      expect(operatorSettings.save).not.toHaveBeenCalled()
      await typeName('Left')
      await tap(button(view, 'Save'))
      expect(vi.mocked(operatorSettings.save).mock.calls[0]?.[0].saved_positions).toEqual([{ name: 'Left', position: [10, 20, -50] }, { name: 'Corner', position: [1, 2, -3] }])
      expect(view.find('button[aria-label="Rename Left"]').exists()).toBe(true)
      // Renaming to the same name saves nothing.
      await tap(view.get('button[aria-label="Rename Corner"]').element)
      await tap(button(view, 'Save'))
      expect(operatorSettings.save).toHaveBeenCalledOnce()
    } finally { stop(); touchInput.value = false }
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
  it('confirms Go to, then sends Z up, X Y, and Z down to the saved Z by default, one after another', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    expect([...document.querySelectorAll('.steps li')].map((step) => step.textContent.trim())).toEqual([
      'Lift Z to the top of travel (safe Z) -0.500', 'Move X Y to X +10.000  Y +20.000', 'Lower Z to the saved -50.000'])
    expect(button(view, 'Down to saved Z -50.000').attributes('aria-pressed')).toBe('true')
    // Left at the safe height, then Cancel: the next Go to again includes Z.
    const stay = button(view, 'Stays at safe height').element as HTMLButtonElement
    stay.click(); await nextTick()
    expect(document.body.textContent).not.toContain('Lower Z to the saved -50.000')
    await button(view, 'Cancel').trigger('click')
    expect(send).not.toHaveBeenCalled()

    await button(view, 'Go to').trigger('click')
    expect(document.body.textContent).toContain('Lower Z to the saved -50.000')
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
  it('lifts to the safe Z of the settings and names it in the steps', async () => {
    panel.preferences = { ...panel.preferences, go_to_safe_z: -15, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    expect([...document.querySelectorAll('.steps li')].map((step) => step.textContent.trim())).toEqual([
      'Lift Z to safe Z -15.000', 'Move X Y to X +10.000  Y +20.000', 'Lower Z to the saved -50.000'])
    const confirm = [...document.querySelectorAll('footer button')].find((candidate) => candidate.textContent.trim() === 'Go to') as HTMLButtonElement
    confirm.click(); await nextTick()
    expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-15.000'])
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
  it('disarms Go to immediately on a panel stop, even when the lift appears to arrive', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    await button(view, 'Go to').trigger('click')
    answerLast()
    panel.status = statusAt({ x: 100, y: 50, z: -1 }, { mode: 'mdi', interp: 'reading' })
    panel.motionStopRequest = 1
    panel.status = statusAt({ x: 100, y: 50, z: -0.625 }, { mode: 'mdi' })
    await nextTick()
    expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500'])
    expect(band(view)).toContain('Go to stopped')
  })
  it('describes an upward final move when the saved Z is higher than the configured safe Z', async () => {
    panel.preferences = { ...panel.preferences, go_to_safe_z: -15,
      saved_positions: [{ name: 'Vise jaw', position: [10, 20, -10] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    expect(document.body.textContent).toContain('Raise Z to the saved -10.000')
    expect(button(view, 'Up to saved Z -10.000').attributes('aria-pressed')).toBe('true')
  })
  it('stops a Go to that the operator leaves, and sends nothing further', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    const confirm = [...document.querySelectorAll('footer button')].find((candidate) => candidate.textContent.trim() === 'Go to') as HTMLButtonElement
    confirm.click(); await nextTick()
    answerLast()
    panel.status = statusAt({ x: 100, y: 50, z: -10 }, { mode: 'mdi', interp: 'reading' })
    await button(view, 'Power feed').trigger('click')
    expect(vi.mocked(send).mock.calls.map(([command]) => command.type)).toEqual(['mdi', 'abort'])
    panel.status = statusAt({ x: 100, y: 50, z: -0.5 }, { mode: 'mdi' })
    await button(view, 'Positions').trigger('click')
    panel.status = statusAt({ x: 100, y: 50, z: -0.5 }, { mode: 'mdi' })
    await nextTick()
    expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500'])
    expect(vi.mocked(send).mock.calls.map(([command]) => command.type)).toEqual(['mdi', 'abort'])
  })
  it('toggles Z at the end with a touch tap, and the planned moves follow', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const stop = trackNumericInput()
    try {
      const view = await mountManual()
      await button(view, 'Positions').trigger('click')
      await button(view, 'Go to').trigger('click')
      expect(touchInput.value).toBe(false)
      const choice = (text: string): HTMLButtonElement => button(view, text).element as HTMLButtonElement
      expect(choice('Down to saved Z -50.000').getAttribute('aria-pressed')).toBe('true')
      expect(document.body.textContent).toContain('Lower Z to the saved -50.000')
      // A finger on the touch screen: WebKitGTK sends touchstart, then the click on release.
      await tap(choice('Stays at safe height'))
      expect(touchInput.value).toBe(true)
      expect(choice('Stays at safe height').getAttribute('aria-pressed')).toBe('true')
      expect(choice('Down to saved Z -50.000').getAttribute('aria-pressed')).toBe('false')
      expect(document.body.textContent).not.toContain('Lower Z to the saved -50.000')
      await tap(choice('Down to saved Z -50.000'))
      expect(choice('Down to saved Z -50.000').getAttribute('aria-pressed')).toBe('true')
      expect(document.body.textContent).toContain('Lower Z to the saved -50.000')
      await tap(choice('Stays at safe height'))
      expect(choice('Stays at safe height').getAttribute('aria-pressed')).toBe('true')
      expect(document.body.textContent).not.toContain('Lower Z to the saved -50.000')
      await tap(choice('Down to saved Z -50.000'))
      const confirm = [...document.querySelectorAll('footer button')].find((candidate) => candidate.textContent.trim() === 'Go to') as HTMLButtonElement
      await tap(confirm)
      expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500'])
      answerLast(); panel.status = statusAt({ x: 100, y: 50, z: -0.5 }, { mode: 'mdi' })
      answerLast(); panel.status = statusAt({ x: 10, y: 20, z: -0.5 }, { mode: 'mdi' })
      expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500', 'G21 G90 G53 G0 X10.000 Y20.000', 'G21 G90 G53 G0 Z-50.000'])
    } finally { stop(); touchInput.value = false }
  })
  it('goes on when the measured position trails the command by the backlash of a scale', async () => {
    panel.preferences = { ...panel.preferences, saved_positions: [{ name: 'Vise jaw', position: [10, 20, -50] }] }
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Go to').trigger('click')
    const confirm = [...document.querySelectorAll('footer button')].find((candidate) => candidate.textContent.trim() === 'Go to') as HTMLButtonElement
    confirm.click(); await nextTick()
    answerLast()
    panel.status = statusAt({ x: 100, y: 50, z: -10 }, { mode: 'mdi', interp: 'reading' })
    // The G0759's Z scale reads 0.125 mm short after a reversal.
    panel.status = statusAt({ x: 100, y: 50, z: -0.625 }, { mode: 'mdi' })
    await nextTick()
    expect(sentMdi()).toEqual(['G21 G90 G53 G0 Z-0.500', 'G21 G90 G53 G0 X10.000 Y20.000'])
  })
  it('leaves the machine alone when the card closes with no Go to running', async () => {
    const view = await mountManual()
    await button(view, 'Positions').trigger('click')
    await button(view, 'Power feed').trigger('click')
    expect(send).not.toHaveBeenCalled()
  })
})
