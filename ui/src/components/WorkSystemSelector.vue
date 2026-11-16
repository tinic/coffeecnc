<script setup lang="ts">
import { computed, useTemplateRef } from 'vue'
import { mdiBlock, panel, send } from '../machine'
import { visibleWorkSystems } from '../operator-settings'
import { vPopover } from '../popover'
import { vTooltip } from '../tooltip'
import { formatLength, wcsName } from '../units'
import AppIcon from './AppIcon.vue'

const menu = useTemplateRef<HTMLDetailsElement>('menu')
const systems = computed(() => visibleWorkSystems(panel.preferences.enabled_wcs, panel.status?.offsets.wcs))
const shift = computed(() => panel.status?.offsets.g92)
const shifted = computed(() => shift.value?.some((value) => value !== 0) ?? false)
function select(wcs: number): void {
  if (mdiBlock.value !== null) return
  if (menu.value) menu.value.open = false
  send({ type: 'select_wcs', wcs })
}
</script>

<template>
  <div class="work-system-controls">
    <div class="selector" aria-label="Work coordinate systems">
      <button v-for="wcs in systems.primary" :key="wcs" v-tooltip="mdiBlock ?? `Make ${wcsName(wcs)} active. The machine does not move.`" class="btn mono" :class="{ on: panel.status?.offsets.wcs === wcs }" type="button" :aria-pressed="panel.status?.offsets.wcs === wcs" :disabled="mdiBlock !== null" @click="select(wcs)">{{ wcsName(wcs) }}</button>
      <details v-if="systems.more.length" ref="menu" v-popover class="more">
        <summary class="btn" aria-label="More work systems">More<AppIcon name="up" /></summary>
        <div class="popover more-systems"><button v-for="wcs in systems.more" :key="wcs" class="btn mono" type="button" :disabled="mdiBlock !== null" @click="select(wcs)">{{ wcsName(wcs) }}</button></div>
      </details>
    </div>
    <details v-if="shifted" v-popover class="shift">
      <summary>G92 / G52 shift active<AppIcon name="info" /></summary>
      <div class="popover shift-details">
        <h3>Additional coordinate shift</h3>
        <div class="shift-values"><span v-for="(value, index) in shift" :key="index">{{ ['X', 'Y', 'Z'][index] }} <strong class="mono">{{ formatLength(value, panel.unit) }}</strong></span><span>{{ panel.unit === 'inch' ? 'in' : 'mm' }}</span></div>
        <p>This shift applies to every work coordinate system. G92 and G52 share these offset values in LinuxCNC.</p>
        <p>Zero and Half adjust the selected work offset and leave this shift in place. LinuxCNC may retain it after a restart.</p>
      </div>
    </details>
  </div>
</template>

<style scoped>
.work-system-controls { min-width: 0; }.selector { display: flex; gap: 6px; }.selector > .btn, .more { flex: 1; min-width: 0; }.selector .btn { min-height: 44px; padding: 8px 10px; font-size: 14px; }.more { position: relative; }.more summary { width: 100%; gap: 6px; }.more summary :deep(svg) { width: 14px; height: 14px; }
.popover { top: auto; bottom: calc(100% + 8px); }.more-systems { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; min-width: 220px; padding: 12px; }
.shift { position: relative; margin-top: 8px; }.shift summary { display: flex; align-items: center; justify-content: flex-start; gap: 8px; width: fit-content; min-height: 32px; padding: 4px 8px; border: 1px solid var(--warn); border-radius: 6px; color: var(--warn); font-size: 12px; }.shift summary :deep(svg) { width: 16px; height: 16px; }.shift-details { left: 0; right: auto; width: 370px; max-width: 100%; }.shift-details p { margin: 10px 0 0; font-size: 12px; line-height: 1.5; color: var(--text-dim); }.shift-values { display: flex; flex-wrap: wrap; gap: 8px; font-size: 12px; }.shift-values strong { font-weight: 400; }
</style>
