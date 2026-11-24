// The LAN viewer's screen: the program name and the latest notice above the DRO.

import { afterEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import RemoteScreen from '../src/components/RemoteScreen.vue'
import RemoteProgramUpload from '../src/components/RemoteProgramUpload.vue'
import { panelLanguage, theme } from '../src/appearance'
import { setLanguage } from '../src/i18n'
import { operatorSettings, panel } from '../src/machine'
import type { Status } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'

let wrapper: VueWrapper | undefined
afterEach(() => {
  wrapper?.unmount(); wrapper = undefined; panel.notice = null
  operatorSettings.chooseTheme('light')
  operatorSettings.chooseLanguage('en')
  vi.unstubAllGlobals()
})

describe('LAN upload', () => {
  it.each([
    ['a keyed reply',
      { reason: 'The filename is in use.', localization: { key: 'backend.programEdit.exists', params: { name: 'Plate 2.ngc' } } }],
    ['a reply from an older backend',
      { reason: 'A program named Plate 2.ngc exists already.' }],
    ['a reply with invalid localization',
      { reason: 'A program named Plate 2.ngc exists already.', localization: { key: 'backend.programEdit.exists', params: { name: 2 } } }],
  ])('localizes %s', async (_name, payload) => {
    await setLanguage('de')
    vi.stubGlobal('fetch', vi.fn((): Promise<Response> =>
      Promise.resolve(Response.json(payload, { status: 409 }))))
    wrapper = mount(RemoteProgramUpload, { global: { stubs: { teleport: true } } })
    const input = wrapper.get('input[type="file"]')
    const file = new File(['G21\nM2\n'], 'Plate 2.ngc')
    Object.defineProperty(input.element, 'files', { value: { length: 1, item: () => file } })
    await input.trigger('change')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.get('[role="alert"]').text()).toBe('Ein Programm mit dem Namen Plate 2.ngc existiert bereits.')
  })
})

function screen(): VueWrapper {
  wrapper = mount(RemoteScreen, { global: { stubs: { DroPanel: true, ProgramTab: true, LogDialog: true, teleport: true } } })
  return wrapper
}

describe('LAN screen', () => {
  it('offers the supported themes and languages from the settings gear', async () => {
    const view = screen()
    await view.get('button[aria-haspopup="dialog"]').trigger('click')
    const [themeSetting, languageSetting] = view.findAll('select')
    expect(themeSetting?.findAll('option')).toHaveLength(4)
    expect(languageSetting?.findAll('option')).toHaveLength(7)
    await themeSetting?.setValue('harbor')
    await languageSetting?.setValue('de')
    expect([theme.value, panelLanguage.value]).toEqual(['harbor', 'de'])
  })
  it('shows notices with no program loaded', async () => {
    const status: Status = new SimMachine({ emit: (): void => undefined }).status()
    panel.status = { ...status, program: { ...status.program, file: '' } }
    const view = screen()
    panel.notice = { id: 1, level: 'error', text: 'Emergency stop is active.', context: '', time: 0, count: 1 }
    await nextTick()
    expect(view.find('h1').exists()).toBe(false)
    const notice = view.find('.notice')
    expect(notice.exists()).toBe(true)
    expect(notice.classes()).toContain('error')
    expect(notice.text()).toBe('Emergency stop is active.')
  })

  it('shows the program name with its notice', async () => {
    const status: Status = new SimMachine({ emit: (): void => undefined }).status()
    panel.status = { ...status, program: { ...status.program, file: '/progs/part.ngc' } }
    const view = screen()
    panel.notice = { id: 2, level: 'info', text: 'Program finished.', context: '', time: 0, count: 1 }
    await nextTick()
    expect(view.find('h1').text()).toBe('part.ngc')
    expect(view.find('.notice').text()).toBe('Program finished.')
  })
})
