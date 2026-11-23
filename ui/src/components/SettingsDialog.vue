<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue'
import { panel } from '../machine'
import { theme, themePreview } from '../appearance'
import { limitMarginMm, safeZBlock } from '../manual-moves'
import { vNumeric } from '../numeric-input'
import { parseNumericExpression } from '../numeric-expression'
import { panelThemes, themeLabels, type OperatorPreferences, type PanelTheme } from '../operator-settings'
import type { OperatorSettingsController } from '../operator-settings-state'
import { fromDisplay, toDisplay, velocityUnit, wcsNames } from '../units'
import { vTooltip } from '../tooltip'
import ModalDialog from './ModalDialog.vue'
import ToolModelDialog from './ToolModelDialog.vue'

const props = defineProps<{ settings: OperatorSettingsController }>()
const emit = defineEmits<{ close: [] }>()
const templates = ref(false)
let closed = false
const selectedTheme = ref(theme.value)
let themePreviewed = false
function previewTheme(value: PanelTheme): void {
  selectedTheme.value = value
  themePreviewed = true
  themePreview.value = value
}
onBeforeUnmount(() => {
  closed = true
  themePreview.value = null
})
const startupDisabled = computed(() => props.settings.state.saving || props.settings.state.storage === 'session_only')
const context = props.settings.context()
const unit = panel.unit
const startupUnits = ref<OperatorPreferences['startup_units']>(panel.preferences.startup_units)
const enabled = ref([...panel.preferences.enabled_wcs])
const spindle = ref(String(panel.preferences.default_spindle_rpm))
const initialJog = panel.preferences.default_jog_velocity
const initialJogText = String(Number(toDisplay(initialJog, unit).toFixed(6)))
const jog = ref(initialJogText)
const initialReference = panel.preferences.tool_reference_height
const initialReferenceText = String(Number(toDisplay(initialReference, unit).toFixed(6)))
const reference = ref(initialReferenceText)
const referenceHeight = computed(() => {
  if (reference.value === initialReferenceText) return initialReference
  const value = parseNumericExpression(reference.value, unit)
  return value === null ? null : fromDisplay(value, unit)
})
// Safe Z for Go to: machine Z in display units; empty is the default, Z max less the margin.
const initialSafeZ = panel.preferences.go_to_safe_z
const lengthText = (mm: number): string => String(Number(toDisplay(mm, unit).toFixed(6)))
const initialSafeZText = initialSafeZ === null ? '' : lengthText(initialSafeZ)
const safeZText = ref(initialSafeZText)
const zAxis = computed(() => panel.status?.axes.find((axis) => axis.axis === 'z'))
const safeZ = computed((): number | null | undefined => {
  if (safeZText.value === initialSafeZText) return initialSafeZ
  if (safeZText.value.trim() === '') return null
  const value = parseNumericExpression(safeZText.value, unit)
  return value === null ? undefined : fromDisplay(value, unit)
})
const defaultSafeZ = computed(() => zAxis.value === undefined ? 'the top of Z travel'
  : `${lengthText(zAxis.value.max_limit - limitMarginMm)} ${unit === 'inch' ? 'in' : 'mm'}`)
const safeZInvalid = computed(() => {
  const value = safeZ.value
  if (value === undefined) return 'Enter a safe Z for Go to, or leave it empty.'
  return value === null || value === initialSafeZ ? null : safeZBlock(panel.status, value, unit)
})
function usePresentZ(): void {
  const z = zAxis.value
  if (z?.homed === true) safeZText.value = lengthText(z.machine)
}
const rpm = computed(() => parseNumericExpression(spindle.value, 'rpm'))
const velocity = computed(() => {
  if (jog.value === initialJogText) return initialJog
  const value = parseNumericExpression(jog.value, unit)
  return value === null ? null : fromDisplay(value, unit)
})
const invalid = computed(() => {
  if (referenceHeight.value === null || referenceHeight.value < 0 || referenceHeight.value > 1e9) return 'Enter a tool reference height of zero or higher.'
  if (safeZInvalid.value !== null) return safeZInvalid.value
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
  if (blocked.value !== null || invalid.value !== null || rpm.value === null || velocity.value === null || referenceHeight.value === null || safeZ.value === undefined) return
  const result = await props.settings.save({ ...panel.preferences,
    theme: themePreviewed ? selectedTheme.value : panel.preferences.theme, startup_units: startupUnits.value, enabled_wcs: [...enabled.value],
    default_spindle_rpm: rpm.value, default_jog_velocity: velocity.value, tool_reference_height: referenceHeight.value, go_to_safe_z: safeZ.value }, context)
  if (!result.saved) return
  if (themePreviewed) props.settings.chooseTheme(selectedTheme.value)
  if (!closed) emit('close')
}
</script>

<template>
  <ToolModelDialog v-if="templates" :tool="null" @close="templates = false" />
  <ModalDialog v-else title="Settings" size="large" expanded @close="emit('close')">
    <div class="settings-grid">
      <section class="startup">
        <fieldset class="theme-picker" :disabled="settings.state.saving || settings.state.storage === 'loading'">
          <legend>Theme</legend>
          <div class="theme-options">
            <label v-for="option in panelThemes" :key="option" class="theme-option" :class="{ selected: selectedTheme === option }">
              <input type="radio" name="panel-theme" :checked="selectedTheme === option" @change="previewTheme(option)" />
              <span class="theme-sample" :data-theme="option" aria-hidden="true"></span>
              <span>{{ themeLabels[option] }}</span>
            </label>
          </div>
        </fieldset>
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
        <div class="tool-reference">
          <label class="speed-label" for="settings-reference">Tool reference height</label>
          <div class="speed-entry"><input id="settings-reference" v-model="reference" v-numeric :disabled="settings.state.saving" class="field mono" type="text" inputmode="decimal" :data-numeric-unit="unit" data-numeric-effect="Contact height above work Z zero for tool length touch-off." /><span>{{ unit === 'inch' ? 'in' : 'mm' }}</span></div>
          <p class="hint">Gauge block or setter contact height above work Z zero. Prefills Touch off length; does not move the machine.</p>
        </div>
        <div class="safe-z">
          <label class="speed-label" for="settings-safe-z">Safe Z for Go to</label>
          <div class="speed-entry">
            <input id="settings-safe-z" v-model="safeZText" v-numeric :disabled="settings.state.saving" class="field mono" type="text" inputmode="decimal" :placeholder="`Default ${defaultSafeZ}`" :data-numeric-unit="unit" data-numeric-effect="Machine Z that Go to lifts to before X and Y move." /><span>{{ unit === 'inch' ? 'in' : 'mm' }}</span>
          </div>
          <div class="safe-z-actions">
            <button v-tooltip="zAxis?.homed === true ? 'Set safe Z to the present machine Z' : 'Home Z first'" class="btn" type="button" :disabled="settings.state.saving || zAxis?.homed !== true" @click="usePresentZ">Use the present Z</button>
            <button class="btn quiet" type="button" :disabled="settings.state.saving || safeZText === ''" @click="safeZText = ''">Default</button>
          </div>
          <p class="hint">Machine coordinates. Go to lifts Z here, or stays higher, before X and Y move. Empty: {{ defaultSafeZ }}, the top of travel less {{ limitMarginMm }} mm.</p>
        </div>
        <details class="shift-note"><summary>About G92 / G52 shifts</summary><p>G92/G52 adds an offset to every work system. CoffeeCNC shows it whenever applied. Work system shortcuts do not hide or clear it.</p></details>
      </section>
    </div>
    <template #footer>
      <button class="btn templates" type="button" :disabled="settings.state.saving" @click="templates = true">Tool shape templates</button>
      <div class="save-state"><span>{{ storageLabel }}</span><p v-if="settings.state.error || blocked || invalid" role="status">{{ settings.state.error || blocked || invalid }}</p><p v-else-if="settings.state.storage === 'session_only'">{{ settings.state.reason }}</p></div>
      <button class="btn" type="button" @click="emit('close')">Cancel</button>
      <button class="btn go" type="button" :disabled="blocked !== null || invalid !== null" @click="save">{{ settings.state.saving ? 'Saving…' : settings.state.storage === 'session_only' ? 'Apply' : 'Save settings' }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.settings-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 28px; }
.templates { min-height: 44px; }
h3 { margin: 0 0 4px; font-size: 18px; font-weight: 600; }
.hint { margin: 2px 0 4px; font-size: 12px; line-height: 1.35; color: var(--text-dim); }
fieldset { min-width: 0; margin: 0; padding: 0; border: 0; }
.theme-picker { padding-bottom: 14px; margin-bottom: 14px; border-bottom: 1px solid var(--line); }
.theme-picker legend { font-size: 18px; font-weight: 600; }
.theme-options { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 6px; }
.theme-option { display: flex; align-items: center; gap: 7px; min-height: 44px; padding: 5px 8px; border: 1px solid var(--line); border-radius: 8px; cursor: pointer; background: var(--control); }
.theme-option.selected { border-color: var(--accent); background: var(--accent-soft); }
.theme-option input { flex: none; width: 16px; height: 16px; margin: 0; accent-color: var(--accent); }
.theme-sample { flex: none; position: relative; width: 27px; height: 24px; border: 1px solid var(--line-strong); border-radius: 5px; }
.theme-sample::before { content: ''; position: absolute; inset: 4px 4px 9px; background: var(--panel); border-radius: 2px; }
.theme-sample::after { content: ''; position: absolute; right: 4px; bottom: 4px; width: 8px; height: 3px; background: var(--accent); border-radius: 2px; }
legend, .speed-label { display: block; margin: 0 0 4px; font-size: 14px; font-weight: 500; }
.units { display: flex; width: 100%; margin-bottom: 12px; }.units .btn { min-height: 44px; flex: 1; white-space: nowrap; }.units .btn:first-child { flex: 1.8; }
.safe-z { margin-top: 12px; }.safe-z-actions { display: flex; gap: 8px; margin-top: 6px; }.safe-z-actions .btn { min-height: 44px; }
.speed-entry { display: flex; align-items: center; gap: 12px; }.speed-entry input { width: 100%; min-width: 0; height: 44px; font-size: 20px; }.speed-entry > span { min-width: 48px; font-size: 13px; color: var(--text-dim); }
.wcs-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 6px; }.wcs-grid label { display: flex; align-items: center; justify-content: center; gap: 4px; min-height: 44px; border: 1px solid var(--line); border-radius: 7px; cursor: pointer; font-size: 13px; }.wcs-grid label.selected { border-color: var(--accent); background: var(--accent-soft); }.wcs-grid input { width: 16px; height: 16px; margin: 0; accent-color: var(--accent); }
.shift-note { border-top: 1px solid var(--line); padding-top: 4px; font-size: 13px; }.shift-note summary { font-weight: 500; min-height: 28px; padding-block: 0; }.shift-note p { margin: 6px 0 0; color: var(--text-dim); font-size: 12px; line-height: 1.5; }
.tool-reference { margin-top: 6px; padding-top: 6px; border-top: 1px solid var(--line); }
.save-state { flex: 1; min-width: 0; align-self: center; color: var(--text-dim); font-size: 12px; }.save-state p { margin: 4px 0 0; color: var(--warn); font-size: 12px; }
@media (max-width: 760px) { .settings-grid { grid-template-columns: 1fr; } }
</style>
