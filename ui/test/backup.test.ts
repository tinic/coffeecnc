import { afterEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'

import BackupDialog from '../src/components/BackupDialog.vue'
import { panel, send } from '../src/machine'
import type { BackupWritten, CommandResult, FromMachine, RestoreSummary, ToMachine } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'
import { describeCommand } from '../src/store'

vi.mock('../src/machine', async () => {
  const { reactive, computed } = await import('vue')
  const panel = reactive({ hello: null, status: null, link: 'live', simulated: false, backupWritten: null, restoreSummary: null,
    directory: null, preferences: { recent_programs: [] } }) as unknown as typeof import('../src/machine').panel
  return { panel, live: computed(() => true), send: vi.fn(), onResult: (): (() => void) => () => undefined }
})

function rig(): { machine: SimMachine; run: (command: ToMachine) => FromMachine[] } {
  let out: FromMachine[] = []
  const machine = new SimMachine({ emit: (message): void => { out.push(message) } })
  return {
    machine,
    run: (command): FromMachine[] => {
      out = []
      machine.handle(command)
      return out
    },
  }
}
function result(messages: FromMachine[]): CommandResult {
  const found = messages.find((m): m is CommandResult => m.type === 'result')
  if (!found) throw new Error('No result')
  return found
}

describe('the simulator', () => {
  it('backs up, checks and restores the tool table and work offsets', () => {
    const { run } = rig()
    const written = run({ id: 1, type: 'backup', directory: '/home/sim/nc_files' })
    expect(result(written).outcome).toBe('accepted')
    const archive = written.find((m): m is BackupWritten => m.type === 'backup_written')
    expect(archive?.path).toMatch(/^\/home\/sim\/nc_files\/coffeecnc-sim-\d{8}-\d{6}\.tar\.gz$/)
    const path = archive?.path ?? ''
    // The machine changes after the backup.
    run({ id: 2, type: 'remove_tool', number: 1 })
    const checked = run({ id: 3, type: 'check_restore', path })
    expect(result(checked).outcome).toBe('accepted')
    const summary = checked.find((m): m is RestoreSummary => m.type === 'restore_summary')
    expect(summary?.files.map((file) => [file.role, file.change])).toEqual([
      ['ini', 'same'], ['hal', 'same'], ['tool_table', 'changed'], ['parameters', 'at_exit']])
    expect(result(run({ id: 4, type: 'restore', path, token: 'wrong' })).reason)
      .toBe('The backup file changed after it was checked. Check it again.')
    expect(result(run({ id: 5, type: 'restore', path, token: summary?.token ?? '' })).outcome).toBe('accepted')
    const again = run({ id: 6, type: 'check_restore', path }).find((m): m is RestoreSummary => m.type === 'restore_summary')
    // The work offsets are compared when LinuxCNC stops, so they are never "same" beforehand.
    expect(again?.files.every((file) => file.change === (file.role === 'parameters' ? 'at_exit' : 'same'))).toBe(true)
  })

  it('refuses a restore while the machine is on or the file is not a backup', () => {
    const { run } = rig()
    run({ id: 1, type: 'set_machine', state: 'off' })
    run({ id: 2, type: 'set_machine', state: 'on' })
    expect(result(run({ id: 3, type: 'check_restore', path: '/home/sim/nc_files/slot.ngc' })).reason)
      .toBe('Switch the machine off or press E-stop before a restore.')
    run({ id: 4, type: 'set_machine', state: 'estop' })
    expect(result(run({ id: 5, type: 'check_restore', path: '/home/sim/nc_files/slot.ngc' })).outcome).toBe('rejected')
    expect(result(run({ id: 6, type: 'backup', directory: '/nowhere' })).reason).toBe('Choose a folder for the backup.')
  })

  it('names the commands in the log', () => {
    expect(describeCommand({ type: 'backup', directory: '/x' })).toBe('Back up machine data')
    expect(describeCommand({ type: 'check_restore', path: '/x' })).toBe('Check backup')
    expect(describeCommand({ type: 'restore', path: '/x', token: 't' })).toBe('Restore backup')
  })
})

describe('the backup dialog', () => {
  let wrapper: VueWrapper | null = null
  afterEach(() => { wrapper?.unmount(); wrapper = null; document.body.replaceChildren(); vi.mocked(send).mockClear() })
  const fixture = defineComponent({ components: { BackupDialog }, template: '<div><BackupDialog /><div id="dialogs"></div></div>' })
  const button = (text: string): HTMLButtonElement | undefined =>
    [...document.querySelectorAll('button')].find((item) => item.textContent.includes(text))

  it('refuses a restore while the machine is on, and offers the backup', async () => {
    panel.status = new SimMachine({ emit: (): void => undefined }).status()
    panel.status.machine = 'on'
    wrapper = mount(fixture, { attachTo: document.body })
    await nextTick()
    expect(document.body.textContent).toContain('Switch the machine off or press E-stop before a restore.')
    expect(button('Choose backup')?.disabled).toBe(true)
    expect(button('Choose folder and back up')?.disabled).toBe(false)
  })
})
