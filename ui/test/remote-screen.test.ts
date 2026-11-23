// The LAN viewer's screen: the program name and the latest notice above the DRO.

import { afterEach, describe, expect, it } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import RemoteScreen from '../src/components/RemoteScreen.vue'
import { panelLanguage, theme } from '../src/appearance'
import { operatorSettings, panel } from '../src/machine'
import type { Status } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'

let wrapper: VueWrapper | undefined
afterEach(() => {
  wrapper?.unmount(); wrapper = undefined; panel.notice = null
  operatorSettings.chooseTheme('light')
  operatorSettings.chooseLanguage('en')
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
