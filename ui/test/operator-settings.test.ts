import { afterEach, describe, expect, it, vi, type Mock } from 'vitest'
import { nextTick, ref, type Ref } from 'vue'
import type { Listener } from '../src/bridge'
import type { FromMachine, ToMachine } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import { answerTimeoutMs, createPanel, type PanelState } from '../src/store'
import { decodeOperatorPreferences, defaultOperatorPreferences, visibleWorkSystems, withRecentProgram, type OperatorPreferences, type ProgramLayout, type PanelTheme } from '../src/operator-settings'
import { createOperatorSettingsState, type OperatorSettingsController } from '../src/operator-settings-state'

const disposals: (() => void)[] = []
afterEach(() => { for (const dispose of disposals.splice(0)) dispose() })
interface Rig {
  panel: PanelState
  theme: Ref<PanelTheme>
  settings: OperatorSettingsController
  sent: ToMachine[]
  storage: { getItem: Mock<() => string>; setItem: Mock<(key: string, value: string) => void> }
  storageAccess: Mock<() => Pick<Storage, 'getItem' | 'setItem'>>
  connect: (id?: string | null, capable?: boolean) => void
  snapshot: (revision?: number, document?: OperatorPreferences, id?: string) => void
  reply: (outcome?: 'accepted' | 'failed') => void
  receive: (message: FromMachine) => void
  pass: (ms: number) => void
}
function rig(simulated = false, saved: unknown = null,
  overrides: { layoutOverride?: ProgramLayout; themeOverride?: PanelTheme } = {}): Rig {
  let receive: Listener = () => undefined
  let timer: (() => void) | null = null
  let now = 0
  const sent: ToMachine[] = []
  const storage = { getItem: vi.fn(() => JSON.stringify(saved)), setItem: vi.fn<(key: string, value: string) => void>() }
  const storageAccess = vi.fn(() => storage)
  const panel = createPanel({ transport: { simulated, listen: (listener): void => { receive = listener },
    send: (command): void => { sent.push(command) } }, now: (): number => now,
  timers: { setInterval: (callback): number => { timer = callback; return 1 }, clearInterval: (): void => { timer = null } } })
  const theme = ref<PanelTheme>('light')
  const settings = createOperatorSettingsState(panel.state, panel, storageAccess, { theme, ...overrides })
  panel.start(); disposals.push(settings.dispose, panel.stop)
  const connect = (id: string | null = 'G0759', capable = true): void => {
    receive({ ...hello, machine_id: id, max_spindle_rpm: 3000, max_jog_velocity: 1000, ...(capable ? { operator_settings_storage: 1 } : {}) })
  }
  const snapshot = (revision = 0, document = defaultOperatorPreferences(), id = 'G0759'): void => {
    receive({ type: 'operator_settings', machine_id: id, revision, availability: 'available', reason: '', document })
  }
  const reply = (outcome: 'accepted' | 'failed' = 'accepted'): void => {
    const last = sent.at(-1)
    if (!last) throw new Error('Expected a save')
    receive({ type: 'result', id: last.id, outcome, reason: outcome === 'failed' ? 'Disk full' : '' })
  }
  return { panel: panel.state, theme, settings, sent, storage, storageAccess, connect, snapshot, reply,
    receive: (message: FromMachine): void => { receive(message) }, pass: (ms: number): void => { now += ms; timer?.() } }
}
const custom = (): OperatorPreferences => ({ ...defaultOperatorPreferences(), startup_units: 'inch',
  default_spindle_rpm: 1800, default_jog_velocity: 800, enabled_wcs: [1, 9], tool_reference_height: 6.35 })

describe('operator settings', () => {
  it('upgrades an earlier settings document without losing saved defaults', () => {
    const legacy: Record<string, unknown> = { ...custom() }
    delete legacy['tool_reference_height']
    expect(decodeOperatorPreferences(legacy)).toEqual({ ...custom(), tool_reference_height: 0 })
    for (const height of [-1, Infinity, NaN, '6.35', null]) {
      expect(decodeOperatorPreferences({ ...custom(), tool_reference_height: height })).toBeNull()
    }
  })
  it('applies startup values once, clamps lowered limits, and never sends controller commands', () => {
    const r = rig(); r.connect()
    r.snapshot(2, { ...custom(), default_spindle_rpm: 5000, default_jog_velocity: 2000 })
    expect([r.panel.unit, r.panel.jogVelocity, r.panel.startupSpindleRpm]).toEqual(['inch', 1000, 3000])
    expect(r.panel.preferences.default_spindle_rpm).toBe(5000)
    r.panel.unit = 'mm'; r.panel.jogVelocity = 250
    r.snapshot(3, custom()); r.connect(); r.snapshot(3, custom())
    expect([r.panel.unit, r.panel.jogVelocity, r.panel.startupSpindleRpm]).toEqual(['mm', 250, 3000])
    expect(r.sent).toEqual([]); expect(r.storageAccess).not.toHaveBeenCalled()
  })
  it('preserves live edits made before a delayed initial snapshot', () => {
    const r = rig(); r.connect(); r.panel.unit = 'inch'; r.panel.jogVelocity = 200
    r.snapshot()
    expect(r.panel.unit).toBe('inch'); expect(r.panel.jogVelocity).toBe(200)
  })
  it('isolates native settings from browser storage, including native simulation and old peers', async () => {
    const r = rig(false, custom()); r.connect(null, false)
    expect(r.settings.state.storage).toBe('session_only')
    expect((await r.settings.save(custom(), r.settings.context())).saved).toBe(true)
    expect(r.panel.preferences).toEqual(custom())
    expect(r.panel.unit).toBe('mm'); expect(r.panel.jogVelocity).toBe(600)
    expect(r.sent).toEqual([]); expect(r.storageAccess).not.toHaveBeenCalled()
  })
  it('restores browser defaults on startup and keeps changes after save for the next startup', async () => {
    const r = rig(true, custom()); r.connect()
    expect(r.panel.unit).toBe('inch'); expect(r.panel.jogVelocity).toBe(800)
    expect((await r.settings.save(defaultOperatorPreferences(), r.settings.context())).saved).toBe(true)
    expect(r.panel.preferences.startup_units).toBe('machine')
    expect(r.panel.unit).toBe('inch'); expect(r.panel.jogVelocity).toBe(800)
    expect(r.storage.setItem).toHaveBeenCalledOnce(); expect(r.sent).toEqual([])
    r.storage.setItem.mockImplementation(() => { throw new Error('Quota') })
    expect((await r.settings.save(custom(), r.settings.context())).saved).toBe(false)
    expect(r.panel.preferences.startup_units).toBe('machine')
  })
  it('restarts the browser with the operator’s view and theme, with URL choices taking precedence', () => {
    const first = rig(true, custom())
    first.connect()
    first.settings.chooseProgramLayout('text')
    first.settings.chooseTheme('dark')
    const saved: unknown = JSON.parse(first.storage.setItem.mock.lastCall?.[1] ?? 'null')
    const restarted = rig(true, saved)
    expect([restarted.panel.programLayout, restarted.theme.value]).toEqual(['text', 'dark'])
    const overridden = rig(true, saved, { layoutOverride: 'split', themeOverride: 'light' })
    expect([overridden.panel.programLayout, overridden.theme.value]).toEqual(['split', 'light'])

    // The automatic jump to a failing line is a view action with no preference write.
    restarted.panel.programLayout = 'split'
    expect(restarted.storage.setItem).not.toHaveBeenCalled()
    expect(overridden.storage.setItem).not.toHaveBeenCalled()
    expect(restarted.panel.preferences).toEqual({ ...custom(), program_layout: 'text', theme: 'dark' })
  })
  it('remembers the programs the machine loads, newest first, and keeps them for the next start', async () => {
    const r = rig(true, custom()); r.connect()
    const base = new SimMachine({ emit: (): void => undefined, clock: (): number => 0 }).status()
    const load = async (file: string): Promise<void> => {
      r.receive({ ...base, program: { ...base.program, file } }); await nextTick()
    }
    await load('/nc/a.ngc'); await load('/nc/b.ngc'); await load('/nc/a.ngc')
    const saved = JSON.parse(r.storage.setItem.mock.lastCall?.[1] ?? 'null') as OperatorPreferences
    expect(saved.recent_programs).toEqual(['/nc/a.ngc', '/nc/b.ngc'])
    expect(rig(true, saved).panel.preferences.recent_programs).toEqual(['/nc/a.ngc', '/nc/b.ngc'])
    expect(r.sent).toEqual([])
  })
  it('keeps saved positions in the browser demo across a restart', async () => {
    const r = rig(true, custom()); r.connect()
    const positions = [{ name: 'Vise jaw', position: [-120.5, 33.25, -10] as [number, number, number] }]
    expect(await r.settings.save({ ...r.panel.preferences, saved_positions: positions }, r.settings.context())).toEqual({ saved: true, reason: '' })
    const saved = JSON.parse(r.storage.setItem.mock.lastCall?.[1] ?? 'null') as OperatorPreferences
    expect(saved.saved_positions).toEqual(positions)
    expect(rig(true, saved).panel.preferences).toEqual({ ...custom(), saved_positions: positions })
    expect(r.sent).toEqual([])
  })
  it('saves positions per machine through the native settings and refuses a damaged list instead of dropping it', async () => {
    const r = rig(); r.connect(); r.snapshot(2, custom())
    const positions = [{ name: 'Position 1', position: [1, 2, 3] as [number, number, number] }]
    const saving = r.settings.save({ ...r.panel.preferences, saved_positions: positions }, r.settings.context())
    expect(r.sent[0]).toMatchObject({ type: 'set_operator_settings', machine_id: 'G0759', base_revision: 2,
      document: { ...custom(), saved_positions: positions } })
    r.snapshot(3, { ...custom(), saved_positions: positions }); r.reply()
    expect(await saving).toEqual({ saved: true, reason: '' })
    expect(r.panel.preferences.saved_positions).toEqual(positions)
    for (const bad of [[{ name: ' ', position: [1, 2, 3] }], [{ name: 'A', position: [1, 2, NaN] }], [{ name: 'a'.repeat(65), position: [1, 2, 3] }]]) {
      const refused = await r.settings.save({ ...r.panel.preferences, saved_positions: bad as typeof positions }, r.settings.context())
      expect(refused).toEqual({ saved: false, reason: 'Check the saved position names and coordinates.' })
    }
    expect(r.sent).toHaveLength(1)
    const many = Array.from({ length: 17 }, () => ({ name: 'P', position: [0, 0, 0] }))
    for (const bad of [many, [{ name: 'A\n', position: [1, 2, 3] }], [{ name: 'A', position: [1, 2] }], 'x']) {
      const decoded = decodeOperatorPreferences({ ...custom(), saved_positions: bad })
      expect([decoded?.saved_positions, decoded?.default_spindle_rpm]).toEqual([[], 1800])
    }
  })
  it('keeps at most eight recent programs and drops a damaged list without losing other settings', () => {
    const nine = Array.from({ length: 9 }, (_, index) => `/nc/${String(index)}.ngc`)
    expect(nine.reduce<string[]>((list, path) => withRecentProgram(list, path), [])).toEqual(nine.slice(1).reverse())
    for (const bad of [nine, ['relative.ngc'], [3], 'x']) {
      const decoded = decodeOperatorPreferences({ ...custom(), recent_programs: bad })
      expect([decoded?.recent_programs, decoded?.default_spindle_rpm]).toEqual([[], 1800])
    }
  })
  it('falls back from invalid stored presentation values without resetting other defaults', () => {
    const invalid = rig(true, { ...custom(), program_layout: 'both', theme: null })
    invalid.connect()
    expect([invalid.panel.programLayout, invalid.theme.value, invalid.panel.startupSpindleRpm])
      .toEqual(['path', 'light', 1800])
    expect(invalid.storage.setItem).not.toHaveBeenCalled()
  })
  it('restores native presentation and discards choices queued for a previous machine', async () => {
    const r = rig()
    r.connect(); r.snapshot(2, { ...custom(), program_layout: 'split', theme: 'dark' })
    expect([r.panel.programLayout, r.theme.value]).toEqual(['split', 'dark'])
    expect(r.sent).toEqual([])
    r.settings.chooseProgramLayout('text')
    r.settings.chooseTheme('light')
    r.connect('SecondMill')
    r.snapshot(5, { ...custom(), program_layout: 'path', theme: 'dark' }, 'SecondMill')
    await nextTick()
    expect(r.sent).toHaveLength(1)
    expect([r.panel.programLayout, r.theme.value]).toEqual(['path', 'dark'])
  })
  it('queues rapid native choices on arrival of settings and rebases later choices onto the latest revision', async () => {
    const r = rig()
    r.connect()
    r.settings.chooseProgramLayout('text')
    r.settings.chooseTheme('dark')
    expect(r.sent).toHaveLength(0)
    r.snapshot(2, { ...custom(), program_layout: 'split' })
    await nextTick()
    const first = { ...custom(), program_layout: 'text', theme: 'dark' } as const
    expect(r.sent[0]).toMatchObject({ type: 'set_operator_settings', base_revision: 2, document: first })
    expect([r.panel.programLayout, r.theme.value]).toEqual(['text', 'dark'])

    r.settings.chooseProgramLayout('path')
    expect(r.sent).toHaveLength(1)
    r.snapshot(3, first)
    r.reply()
    await nextTick()
    const second = { ...first, program_layout: 'path' } as const
    expect(r.sent[1]).toMatchObject({ type: 'set_operator_settings', base_revision: 3, document: second })
    r.snapshot(4, second)
    r.reply()
    const restarted = rig()
    restarted.connect()
    restarted.snapshot(4, second)
    expect([restarted.panel.programLayout, restarted.theme.value]).toEqual(['path', 'dark'])
    expect(restarted.sent).toHaveLength(0)
  })
  it('honors native startup overrides without saving them', () => {
    const r = rig(false, null, { layoutOverride: 'text', themeOverride: 'dark' })
    r.connect()
    r.snapshot(1, { ...custom(), program_layout: 'split', theme: 'light' })
    expect([r.panel.programLayout, r.theme.value]).toEqual(['text', 'dark'])
    expect(r.sent).toEqual([])
  })
  it('requires updated revision and accepted result; saving never changes current unit or speed', async () => {
    const r = rig(); r.connect(); r.snapshot(2)
    const edit = r.settings.context()
    const saving = r.settings.save(custom(), edit)
    expect(r.sent[0]).toEqual({ type: 'set_operator_settings', id: 1, machine_id: 'G0759', base_revision: 2, document: custom() })
    expect(r.panel.preferences.enabled_wcs).toHaveLength(9)
    r.snapshot(3, custom()); expect(r.settings.state.saving).toBe(true)
    r.reply(); expect((await saving).saved).toBe(true)
    expect(r.panel.preferences.enabled_wcs).toEqual([1, 9])
    expect([r.panel.unit, r.panel.jogVelocity, r.panel.startupSpindleRpm]).toEqual(['mm', 600, 0])
    expect((await r.settings.save(custom(), edit)).saved).toBe(false)
    expect(r.sent).toHaveLength(1)
  })
  it('does not report a rejected, missing-snapshot, or timed-out write as saved', async () => {
    const r = rig(); r.connect(); r.snapshot(2)
    const failed = r.settings.save(custom(), r.settings.context()); r.reply('failed')
    expect(await failed).toEqual({ saved: false, reason: 'Disk full' })
    const missing = r.settings.save(custom(), r.settings.context()); r.reply()
    expect((await missing).saved).toBe(false); expect(r.settings.state.uncertain).toBe(true)
    r.snapshot(2)
    const timeout = r.settings.save(custom(), r.settings.context()); r.pass(answerTimeoutMs + 200)
    expect((await timeout).reason).toContain('may still finish')
    expect((await r.settings.save(custom(), r.settings.context())).saved).toBe(false)
    r.snapshot(3, custom()); expect(r.settings.state.uncertain).toBe(false)
  })
  it('rejects invalid settings, machine limits, corrupt storage, and unrelated snapshots', async () => {
    const r = rig(); r.connect(); r.snapshot()
    for (const document of [{ ...custom(), enabled_wcs: [] }, { ...custom(), enabled_wcs: [1, 1] },
      { ...custom(), default_spindle_rpm: 3001 }, { ...custom(), default_jog_velocity: 1001 }]) {
      expect((await r.settings.save(document, r.settings.context())).saved).toBe(false)
    }
    r.snapshot(9, custom(), 'OtherMill'); expect(r.settings.state.revision).toBe(0)
    r.snapshot(1, { ...custom(), default_jog_velocity: NaN })
    expect(r.settings.state.storage).toBe('unavailable')
    expect((await r.settings.save(custom(), r.settings.context())).saved).toBe(false)
    expect(r.sent).toEqual([])
    expect(decodeOperatorPreferences({ ...custom(), extra: true })).toBeNull()
  })
  it('discards a pending old-machine save and applies the next machine’s defaults', async () => {
    const r = rig(); r.connect(); r.snapshot()
    const saving = r.settings.save(custom(), r.settings.context())
    r.connect('OtherMill'); expect((await saving).saved).toBe(false)
    r.snapshot(1, custom()); r.reply(); expect(r.settings.state.revision).toBeNull()
    r.snapshot(0, custom(), 'OtherMill')
    expect(r.panel.unit).toBe('inch'); expect(r.settings.state.revision).toBe(0)
  })
})

describe('work-system overflow', () => {
  it('keeps an active extended system visible and every other enabled shortcut reachable', () => {
    const result = visibleWorkSystems(defaultOperatorPreferences().enabled_wcs, 9)
    expect(result.primary).toEqual([1, 2, 3, 4, 9]); expect(result.more).toEqual([5, 6, 7, 8])
  })
  it('shows an active system even when its shortcut is disabled, without showing other disabled systems', () => {
    expect(visibleWorkSystems([1, 2], 8)).toEqual({ primary: [1, 2, 8], more: [] })
  })
})
