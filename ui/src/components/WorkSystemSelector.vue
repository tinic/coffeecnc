<script setup lang="ts">
import { computed, useTemplateRef } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import { mdiBlock, panel, send } from '../machine'
import { visibleWorkSystems } from '../operator-settings'
import { vPopover } from '../popover'
import { vTooltip } from '../tooltip'
import { formatLength, wcsName } from '../units'
import AppIcon from './AppIcon.vue'

const { t } = useI18n()
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
    <div class="selector" :aria-label="t('manual.workSystems.group')">
      <button v-for="wcs in systems.primary" :key="wcs" v-tooltip="mdiBlock !== null ? translateMessage(mdiBlock) : t('manual.workSystems.makeActive', { wcs: wcsName(wcs) })" class="btn mono" :class="{ on: panel.status?.offsets.wcs === wcs }" type="button" :aria-pressed="panel.status?.offsets.wcs === wcs" :disabled="mdiBlock !== null" @click="select(wcs)">{{ wcsName(wcs) }}</button>
      <details v-if="systems.more.length" ref="menu" v-popover class="more">
        <summary class="btn" :aria-label="t('manual.workSystems.more')">{{ t('common.more') }}<AppIcon name="up" /></summary>
        <div class="popover more-systems"><button v-for="wcs in systems.more" :key="wcs" class="btn mono" type="button" :disabled="mdiBlock !== null" @click="select(wcs)">{{ wcsName(wcs) }}</button></div>
      </details>
    </div>
    <details v-if="shifted" v-popover class="shift">
      <summary class="btn warning">{{ t('manual.workSystems.shiftActive') }}<AppIcon name="info" /></summary>
      <div class="popover shift-details">
        <h3>{{ t('manual.workSystems.shiftTitle') }}</h3>
        <div class="shift-values"><span v-for="(value, index) in shift" :key="index">{{ ['X', 'Y', 'Z'][index] }} <strong class="mono">{{ formatLength(value, panel.unit) }}</strong></span><span>{{ panel.unit === 'inch' ? 'in' : 'mm' }}</span></div>
        <p>{{ t('manual.workSystems.shiftAll') }}</p>
        <p>{{ t('manual.workSystems.shiftKept') }}</p>
      </div>
    </details>
  </div>
</template>

<style scoped>
.work-system-controls { min-width: 0; }.selector { display: flex; gap: 6px; }.selector > .btn, .more { flex: 1; min-width: 0; }.selector .btn { min-height: 44px; padding: 8px 10px; font-size: 14px; }.more { position: relative; }.more summary { width: 100%; gap: 6px; }.more summary :deep(svg) { width: 14px; height: 14px; }
.popover { top: auto; bottom: calc(100% + 8px); }.more-systems { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; min-width: 220px; padding: 12px; }
.shift { position: relative; margin-top: 8px; }.shift summary { min-height: 32px; padding: 4px 8px; font-size: var(--type-sm); }.shift summary :deep(svg) { width: 16px; height: 16px; }.shift-details { left: 0; right: auto; width: 370px; max-width: 100%; }.shift-details p { margin: 10px 0 0; font-size: 12px; line-height: 1.5; color: var(--text-dim); }.shift-values { display: flex; flex-wrap: wrap; gap: 8px; font-size: 12px; }.shift-values strong { font-weight: 400; }
</style>
