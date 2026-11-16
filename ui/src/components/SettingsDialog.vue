<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue'
import { panel } from '../machine'
import { vNumeric } from '../numeric-input'
import { parseNumericExpression } from '../numeric-expression'
import type { OperatorPreferences } from '../operator-settings'
import type { OperatorSettingsController } from '../operator-settings-state'
import { fromDisplay, toDisplay, velocityUnit, wcsNames } from '../units'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{ settings: OperatorSettingsController }>()
const emit = defineEmits<{ close: [] }>()
let closed = false
onBeforeUnmount(() => { closed = true })
const startupDisabled = computed(() => props.settings.state.saving || props.settings.state.storage === 'session_only')
const context = props.settings.context()
const unit = panel.unit
const startupUnits = ref<OperatorPreferences['startup_units']>(panel.preferences.startup_units)
const enabled = ref([...panel.preferences.enabled_wcs])
const spindle = ref(String(panel.preferences.default_spindle_rpm))
const initialJog = panel.preferences.default_jog_velocity
const initialJogText = String(Number(toDisplay(initialJog, unit).toFixed(6)))
const jog = ref(initialJogText)
const rpm = computed(() => parseNumericExpression(spindle.value, 'rpm'))
const velocity = computed(() => {
  if (jog.value === initialJogText) return initialJog
  const value = parseNumericExpression(jog.value, unit)
  return value === null ? null : fromDisplay(value, unit)
})
const invalid = computed(() => {
  if (enabled.value.length === 0) return 'Keep at least one work system shortcut.'
  if (rpm.value === null || rpm.value < 0 || rpm.value > 1e9) return 'Enter a spindle speed of zero or higher.'
  if (rpm.value > (panel.hello?.max_spindle_rpm ?? Infinity)) return `Spindle limit: ${String(panel.hello?.max_spindle_rpm)} rpm.`
  if (velocity.value === null || velocity.value <= 0 || velocity.value > 1e9) return 'Enter a jog speed above zero.'
  if (velocity.value > (panel.hello?.max_jog_velocity ?? Infinity)) return `Jog limit: ${String(Number(toDisplay((panel.hello?.max_jog_velocity ?? 0), unit).toFixed(3)))} ${velocityUnit(unit)}.`
  return null
})
const blocked = computed(() => props.settings.block(context))
const storageLabel = computed(() => {
  switch (props.settings.state.storage) {
    case 'available': return `Saved for ${panel.hello?.machine_id ?? 'this machine'}`
    case 'browser': return 'Saved in this browser'
    case 'session_only': return 'This session only'
    case 'loading': return 'Loading settings…'
    case 'unavailable': return 'Settings unavailable'
  }
})
async function save(): Promise<void> {
  if (blocked.value !== null || invalid.value !== null || rpm.value === null || velocity.value === null) return
  const result = await props.settings.save({ version: 1, startup_units: startupUnits.value, enabled_wcs: [...enabled.value],
    default_spindle_rpm: rpm.value, default_jog_velocity: velocity.value }, context)
  if (result.saved && !closed) emit('close')
}
</script>

<template>
  <ModalDialog title="Settings" size="large" expanded @close="emit('close')">
    <div class="settings-grid">
      <section class="startup">
        <h3>At startup</h3>
        <p class="hint">{{ settings.state.storage === 'session_only' ? 'Startup defaults require saved settings.' : 'Applied next time CoffeeCNC opens.' }}</p>
        <fieldset :disabled="startupDisabled">
          <legend>Display units</legend>
          <div class="units segmented" aria-label="Startup units">
            <button v-for="option in (['machine', 'mm', 'inch'] as const)" :key="option" class="btn" :class="{ on: startupUnits === option }" type="button" :aria-pressed="startupUnits === option" @click="startupUnits = option">{{ option === 'machine' ? 'Machine default' : option === 'inch' ? 'Inch' : 'mm' }}</button>
          </div>
          <label class="speed-label" for="settings-spindle">Default spindle speed</label>
          <div class="speed-entry"><input id="settings-spindle" v-model="spindle" v-numeric :disabled="startupDisabled" class="field mono" type="text" inputmode="decimal" data-numeric-unit="rpm" data-numeric-effect="Startup preset only. Does not start the spindle." /><span>rpm</span></div>
          <p class="hint">Zero leaves the speed unset; never starts the spindle.</p>
          <label class="speed-label" for="settings-jog">Default jog speed</label>
          <div class="speed-entry"><input id="settings-jog" v-model="jog" v-numeric :disabled="startupDisabled" class="field mono" type="text" inputmode="decimal" :data-numeric-unit="unit" data-numeric-effect="Jog speed per minute at startup." /><span>{{ velocityUnit(unit) }}</span></div>
          <p class="hint">Formulas work here too: <span class="mono">600/2</span> or <span class="mono">10in</span> per minute.</p>
        </fieldset>
      </section>
      <section class="coordinates">
        <h3>Work system shortcuts</h3>
        <p class="hint">Five shortcuts on the main row. Others appear under More.</p>
        <div class="wcs-grid">
          <label v-for="(name, index) in wcsNames" :key="name" :class="{ selected: enabled.includes(index + 1) }"><input v-model="enabled" type="checkbox" :value="index + 1" :disabled="settings.state.saving" /><span class="mono">{{ name }}</span></label>
        </div>
        <p class="hint">Programs can use any system; the active one stays visible.</p>
        <details class="shift-note"><summary>About G92 / G52 shifts</summary><p>G92/G52 adds an offset to every work system. CoffeeCNC shows it whenever applied. Work system shortcuts do not hide or clear it.</p></details>
      </section>
    </div>
    <template #footer>
      <div class="save-state"><span>{{ storageLabel }}</span><p v-if="settings.state.error || blocked || invalid" role="status">{{ settings.state.error || blocked || invalid }}</p><p v-else-if="settings.state.storage === 'session_only'">{{ settings.state.reason }}</p></div>
      <button class="btn" type="button" @click="emit('close')">Cancel</button>
      <button class="btn go" type="button" :disabled="blocked !== null || invalid !== null" @click="save">{{ settings.state.saving ? 'Saving…' : settings.state.storage === 'session_only' ? 'Apply' : 'Save settings' }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.settings-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 28px; }
h3 { margin: 0 0 4px; font-size: 18px; font-weight: 600; }
.hint { margin: 2px 0 6px; font-size: 12px; line-height: 1.35; color: var(--text-dim); }
fieldset { min-width: 0; margin: 0; padding: 0; border: 0; }
legend, .speed-label { display: block; margin: 0 0 4px; font-size: 14px; font-weight: 500; }
.units { display: flex; width: 100%; margin-bottom: 12px; }.units .btn { min-height: 44px; flex: 1; white-space: nowrap; }.units .btn:first-child { flex: 1.8; }
.speed-entry { display: flex; align-items: center; gap: 12px; }.speed-entry input { width: 100%; min-width: 0; height: 44px; font-size: 20px; }.speed-entry > span { min-width: 48px; font-size: 13px; color: var(--text-dim); }
.wcs-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }.wcs-grid label { display: flex; align-items: center; justify-content: center; gap: 8px; min-height: 44px; border: 1px solid var(--line); border-radius: 7px; cursor: pointer; font-size: 15px; }.wcs-grid label.selected { border-color: var(--accent); background: var(--accent-soft); }.wcs-grid input { width: 18px; height: 18px; accent-color: var(--accent); }
.shift-note { border-top: 1px solid var(--line); padding-top: 12px; font-size: 13px; }.shift-note summary { font-weight: 500; min-height: 32px; padding-block: 4px; }.shift-note p { margin: 6px 0 0; color: var(--text-dim); font-size: 12px; line-height: 1.5; }
.save-state { flex: 1; min-width: 0; align-self: center; color: var(--text-dim); font-size: 12px; }.save-state p { margin: 4px 0 0; color: var(--warn); font-size: 12px; }
@media (max-width: 760px) { .settings-grid { grid-template-columns: 1fr; } }
</style>
