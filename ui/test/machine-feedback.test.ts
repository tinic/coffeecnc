import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import MachineFeedback from '../src/components/MachineFeedback.vue'
import { panel } from '../src/machine'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { reactive } = await import('vue')
  return { panel: reactive({}) as typeof import('../src/machine').panel }
})

let wrapper: VueWrapper | null = null
beforeEach(() => {
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.link = 'live'
  panel.status.spindle = { direction: 'forward', commanded_rpm: 9000, override: 2, actual_rpm: 6500, at_speed: true }
  panel.status.probe_input = true
})
afterEach(() => { wrapper?.unmount() })

describe('machine feedback presentation', () => {
  it.each(['null', 'absent'])('shows %s feedback as unavailable, never from the spindle command', (kind) => {
    if (panel.status === null) throw new Error('Missing status')
    if (kind === 'null') {
      panel.status.spindle.actual_rpm = null; panel.status.spindle.at_speed = null; panel.status.probe_input = null
    } else {
      delete panel.status.spindle.actual_rpm; delete panel.status.spindle.at_speed; delete panel.status.probe_input
    }
    wrapper = mount(MachineFeedback, { props: { showProbe: true } })
    expect(wrapper.findAll('dd').map((value) => value.text())).toEqual(['Unavailable', 'Unavailable', 'Unavailable'])
    expect(wrapper.text()).not.toContain('9000')
    expect(wrapper.text()).not.toContain('18000')
  })

  it('shows real zero/false readings and updates to measured feedback', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.spindle.actual_rpm = 0; panel.status.spindle.at_speed = false; panel.status.probe_input = false
    wrapper = mount(MachineFeedback, { props: { showProbe: true } })
    expect(wrapper.findAll('dd').map((value) => value.text())).toEqual(['0 rpm', 'No', 'Clear'])
    panel.status.spindle.actual_rpm = 6587.5; panel.status.spindle.at_speed = true; panel.status.probe_input = true
    await nextTick()
    expect(wrapper.findAll('dd').map((value) => value.text())).toEqual(['6588 rpm', 'Yes', 'Tripped'])
    expect(wrapper.get('dd.tripped').text()).toBe('Tripped')
  })

  it.each(['stale', 'silent'] as const)('labels %s readings as history without a live probe indication', (link) => {
    panel.link = link
    wrapper = mount(MachineFeedback, { props: { showProbe: true } })
    expect(wrapper.findAll('dd').map((value) => value.text())).toEqual(['6500 rpm · stale', 'Yes · stale', 'Tripped · stale'])
    expect(wrapper.find('dd.tripped').exists()).toBe(false)
  })

  it.each(['down', 'incompatible', 'connecting'] as const)('discards %s placeholders', (link) => {
    panel.link = link
    wrapper = mount(MachineFeedback, { props: { showProbe: true } })
    expect(wrapper.findAll('dd').map((value) => value.text())).toEqual(['Unavailable', 'Unavailable', 'Unavailable'])
    expect(wrapper.find('dd.tripped').exists()).toBe(false)
  })

  it('also respects the native stale/down link when the panel link has not caught up', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.link = 'stale'
    wrapper = mount(MachineFeedback)
    expect(wrapper.text()).toContain('6500 rpm · stale')
    panel.status.link = 'down'
    await nextTick()
    expect(wrapper.findAll('dd').map((value) => value.text())).toEqual(['Unavailable', 'Unavailable'])
    expect(wrapper.text()).not.toContain('Probe input')
  })

  it.each([NaN, Infinity, -1])('does not display an invalid measured RPM value (%s)', (rpm) => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.spindle.actual_rpm = rpm
    wrapper = mount(MachineFeedback)
    expect(wrapper.get('dd').text()).toBe('Unavailable')
  })
})
