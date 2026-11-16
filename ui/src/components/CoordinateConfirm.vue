<script setup lang="ts">
import { onBeforeUnmount, onMounted, useTemplateRef } from 'vue'
import type { AxisLetter, LengthUnit } from '../protocol'
import { formatLength } from '../units'
import ModalDialog from './ModalDialog.vue'

defineProps<{ axis: AxisLetter; wcs: string; current: number; target: number; unit: LengthUnit; operation: 'Zero' | 'Half' }>()
const emit = defineEmits<{ confirm: []; close: [] }>()
const cancel = useTemplateRef<HTMLButtonElement>('cancel')
const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
onMounted(() => { cancel.value?.focus({ preventScroll: true }) })
onBeforeUnmount(() => { previousFocus?.focus({ preventScroll: true }) })
</script>

<template>
  <ModalDialog :title="`${operation} ${axis.toUpperCase()} in ${wcs}?`" size="small" fresh-gesture @close="emit('close')">
    <p class="coordinate mono">{{ formatLength(current, unit) }} → {{ formatLength(target, unit) }} {{ unit === 'inch' ? 'in' : 'mm' }}</p>
    <p>Changes the {{ wcs }} work offset at the present position. The machine does not move.</p>
    <template #footer>
      <button ref="cancel" class="btn" type="button" @click="emit('close')">Cancel</button>
      <button class="btn primary" type="button" @click="emit('confirm')">Set {{ axis.toUpperCase() }} to {{ operation.toLowerCase() }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; }.coordinate { font-size: 20px; }.btn { min-height: 48px; }
</style>
