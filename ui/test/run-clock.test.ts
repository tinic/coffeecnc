import { nextTick } from 'vue'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import type { Status } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { reactive } = await import('vue')
  return { panel: reactive({ status: null as Status | null }) }
})

beforeEach(() => {
  vi.resetModules()
  vi.useFakeTimers()
})
afterEach(() => {
  vi.clearAllTimers()
  vi.useRealTimers()
})

it('counts the restarted cut once and preserves its time through an air cut', async () => {
  const { panel } = await import('../src/machine')
  const { runClock } = await import('../src/run-clock')
  const base = new SimMachine({ emit: (): void => undefined }).status()
  const publish = async (time: number, interp: Status['interp'], program: Partial<Status['program']>): Promise<void> => {
    vi.setSystemTime(time)
    panel.status = {
      ...base, link: 'live', machine: 'on', mode: 'auto', interp,
      program: { ...base.program, file: '/air/03-pocket.ngc', cycle: 'idle', ...program },
    }
    await nextTick()
  }

  await publish(1000, 'reading', { cycle: 'running' })
  await publish(2000, 'paused', { cycle: 'running' })
  await publish(3000, 'idle', {})
  // The preview has not confirmed a restart line yet.
  expect(runClock.parts).toBe(0)
  expect(runClock.lastSeconds).toBeNull()
  await publish(3100, 'idle', { restart_hint: { line: 25, tool: 1 } })
  await publish(4000, 'reading', { cycle: 'running' })
  await publish(9000, 'idle', { cycle: 'running' })
  expect(runClock.startedAt).toBe(4000)
  await publish(12000, 'idle', { cycle: 'completed' })
  await publish(13000, 'idle', { cycle: 'completed' })
  expect(runClock.parts).toBe(1)
  expect(runClock.lastSeconds).toBe(8)

  await publish(14000, 'reading', { air_z: 25, air_offset_owned: true })
  expect(runClock.startedAt).toBe(14000)
  await publish(20000, 'idle', { air_z: 25, air_offset_owned: true })
  await publish(21000, 'idle', {})
  expect(runClock.parts).toBe(1)
  expect(runClock.lastSeconds).toBe(8)
})
