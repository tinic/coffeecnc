<script setup lang="ts">
import { onBeforeUnmount, onMounted, useTemplateRef } from 'vue'
import { useI18n } from 'vue-i18n'
import type { AxisLetter, LengthUnit } from '../protocol'
import { formatLength } from '../units'
import ModalDialog from './ModalDialog.vue'

defineProps<{ axis: AxisLetter; wcs: string; current: number; target: number; unit: LengthUnit; operation: 'zero' | 'half' }>()
const emit = defineEmits<{ confirm: []; close: [] }>()
const { t } = useI18n()
const cancel = useTemplateRef<HTMLButtonElement>('cancel')
const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
onMounted(() => { cancel.value?.focus({ preventScroll: true }) })
onBeforeUnmount(() => { previousFocus?.focus({ preventScroll: true }) })
</script>

<template>
  <ModalDialog :title="operation === 'half' ? t('manual.coordinateConfirm.halfTitle', { axis: axis.toUpperCase(), wcs }) : t('manual.coordinateConfirm.zeroTitle', { axis: axis.toUpperCase(), wcs })" size="small" fresh-gesture @close="emit('close')">
    <p class="coordinate mono">{{ formatLength(current, unit) }} → {{ formatLength(target, unit) }} {{ unit === 'inch' ? 'in' : 'mm' }}</p>
    <p>{{ t('manual.coordinateConfirm.effect', { wcs }) }}</p>
    <template #footer>
      <button ref="cancel" class="btn" type="button" @click="emit('close')">{{ t('common.cancel') }}</button>
      <button class="btn primary" type="button" @click="emit('confirm')">{{ operation === 'half' ? t('manual.coordinateConfirm.setHalf', { axis: axis.toUpperCase() }) : t('manual.coordinateConfirm.setZero', { axis: axis.toUpperCase() }) }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; }.coordinate { font-size: 20px; }.btn { min-height: 48px; }
</style>
