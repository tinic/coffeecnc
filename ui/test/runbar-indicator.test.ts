// The … button in the run bar shows which execution options are away from their defaults, as the
// controller reports them (#254).
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import RunBar from '../src/components/RunBar.vue'
import { panel, send } from '../src/machine'
import type { Status } from '../src/protocol'
import { hello } from '../src/sim/machine'
import { rig } from './harness'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const { defaultOperatorPreferences } = await import('../src/operator-settings')
  const { connectionBlock } = await import('../src/machine-block')
  const { requiresHoming } = await import('../src/machine-capabilities')
  const panel = reactive({ preferences: defaultOperatorPreferences(), hello: null, status: null, link: 'live', unit: 'mm', tab: 'program', notice: null, log: [] }) as unknown as typeof import('../src/machine').panel
  return { panel, send: vi.fn(), onResult: (): (() => void) => (): void => undefined,
    reported: computed(() => panel.status), live: computed(() => panel.link === 'live'),
    linkBlock: computed(() => connectionBlock(panel.link)),
    allHomed: computed(() => panel.status?.axes.every((axis) => axis.homed) ?? false),
    homingRequired: computed(() => requiresHoming(panel.hello)) }
})

let wrapper: VueWrapper | null = null
const fresh = (): Status => rig().status()

beforeEach(() => {
  vi.mocked(send).mockClear()
  panel.hello = { ...hello }
  panel.status = fresh()
  panel.link = 'live'; panel.tab = 'program'; panel.preview = null
})
afterEach(() => { wrapper?.unmount(); wrapper = null })

const badges = (): string[] => wrapper?.findAll('.active-options small').map((b) => b.text()) ?? []
const label = (): string | undefined => wrapper?.get('.more-button').attributes('aria-label')

interface Options { rapid?: number; velocity?: number; m1?: boolean; slash?: boolean }
function apply(o: Options): void {
  const s = panel.status
  if (!s) throw new Error('Missing status')
  if (o.rapid !== undefined) s.motion.rapid_override = o.rapid
  if (o.velocity !== undefined) s.motion.velocity_limit = o.velocity
  if (o.m1 !== undefined) s.program.optional_stop = o.m1
  if (o.slash !== undefined) s.program.block_delete = o.slash
}

describe('run bar execution-option indicator', () => {
  it('shows nothing while every option is at its default', () => {
    wrapper = mount(RunBar)
    expect(wrapper.find('.active-options').exists()).toBe(false)
    expect(wrapper.get('.more-button').classes()).not.toContain('active')
    expect(label()).toBe('More execution controls')
  })

  it.each<[string, Options, string[], string]>([
    ['rapid below 100 %', { rapid: 0.5 }, ['R50'], 'Rapid override 50 %'],
    ['rapid at 0 %', { rapid: 0 }, ['R0'], 'Rapid override 0 %'],
    ['rapid just below 100 % never reads 100', { rapid: 0.996 }, ['R99'], 'Rapid override 99 %'],
    ['velocity cap below 100 %', { velocity: 0.4 }, ['V40'], 'Maximum velocity 40 %'],
    ['optional stop', { m1: true }, ['M1'], 'Optional stop M1'],
    ['block delete', { slash: true }, ['/'], 'Block delete /'],
  ])('shows %s', async (_name, options, expected, spoken) => {
    wrapper = mount(RunBar)
    apply(options)
    await nextTick()
    expect(badges()).toEqual(expected)
    expect(wrapper.get('.more-button').classes()).toContain('active')
    expect(label()).toBe(`More execution controls · ${spoken}`)
  })

  it.each<[Options, string[]]>([
    [{ rapid: 0.5, m1: true }, ['R50', 'M1']],
    [{ m1: true, slash: true }, ['M1', '/']],
    [{ rapid: 0.25, slash: true }, ['R25', '/']],
    [{ velocity: 0.8, m1: true, slash: true }, ['V80', 'M1', '/']],
    [{ rapid: 0.5, velocity: 0.4, m1: true, slash: true }, ['R50', 'V40', 'M1', '/']],
  ])('combines %o in a fixed order', async (options, expected) => {
    wrapper = mount(RunBar)
    apply(options)
    await nextTick()
    expect(badges()).toEqual(expected)
  })

  it('reads the full state into the accessible name', async () => {
    wrapper = mount(RunBar)
    apply({ rapid: 0.5, velocity: 0.4, m1: true, slash: true })
    await nextTick()
    expect(label()).toBe('More execution controls · Rapid override 50 % · Maximum velocity 40 % · Optional stop M1 · Block delete /')
  })

  it('goes away when the controller reports the defaults again', async () => {
    wrapper = mount(RunBar)
    apply({ rapid: 0.5, velocity: 0.4, m1: true, slash: true })
    await nextTick()
    apply({ rapid: 1, velocity: 1, m1: false, slash: false })
    await nextTick()
    expect(wrapper.find('.active-options').exists()).toBe(false)
    expect(label()).toBe('More execution controls')
  })

  it('ignores a controller that does not report a velocity cap', async () => {
    wrapper = mount(RunBar)
    delete panel.status?.motion.velocity_limit
    await nextTick()
    expect(wrapper.find('.active-options').exists()).toBe(false)
  })

  it('follows reported status, not the tap: a toggle without a status change shows nothing', async () => {
    wrapper = mount(RunBar)
    const m1 = wrapper.get('.popover').findAll('button').find((b) => b.text() === 'Optional stop M1')
    await m1?.trigger('click')
    expect(send).toHaveBeenCalledWith({ type: 'set_program_option', option: 'optional_stop', on: true })
    expect(wrapper.find('.active-options').exists()).toBe(false)
  })

  it('opens the same popover when tapped', async () => {
    wrapper = mount(RunBar, { attachTo: document.body })
    apply({ m1: true })
    await nextTick()
    const details = wrapper.get('details.more').element as HTMLDetailsElement
    expect(details.open).toBe(false)
    await wrapper.get('.more-button').trigger('click')
    expect(details.open).toBe(true)
    expect(wrapper.get('.popover h3').text()).toBe('Execution controls')
  })

  it('is hidden with the popover in the touch-off workspace', async () => {
    panel.tab = 'probe'
    wrapper = mount(RunBar)
    apply({ m1: true })
    await nextTick()
    expect(wrapper.find('.more-button').exists()).toBe(false)
  })
})

describe('simulator parity', () => {
  it('reports every option the indicator reads, and a fresh panel shows them after reload', () => {
    const r = rig()
    r.home()
    expect(r.send({ type: 'set_override', which: 'rapid', value: 0.5 }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_override', which: 'velocity_limit', value: 0.4 }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_program_option', option: 'optional_stop', on: true }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_program_option', option: 'block_delete', on: true }).outcome).toBe('accepted')
    // A reload mounts a new panel; only the controller's status carries the state across.
    panel.status = r.status()
    wrapper = mount(RunBar)
    expect(badges()).toEqual(['R50', 'V40', 'M1', '/'])
    wrapper.unmount()
    r.send({ type: 'set_override', which: 'rapid', value: 1 })
    r.send({ type: 'set_override', which: 'velocity_limit', value: 1 })
    r.send({ type: 'set_program_option', option: 'optional_stop', on: false })
    r.send({ type: 'set_program_option', option: 'block_delete', on: false })
    panel.status = r.status()
    wrapper = mount(RunBar)
    expect(wrapper.find('.active-options').exists()).toBe(false)
  })
})
