<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from 'vue'
import { panel } from '../machine'
import { applyNumericEntry, numericEntry, trackNumericInput } from '../numeric-input'
import NumberDialog from './NumberDialog.vue'

const error = ref('')
onBeforeUnmount(trackNumericInput())
watch(numericEntry, () => { error.value = '' })
watch([(): unknown => panel.hello, (): string => panel.unit, (): string => panel.tab], () => { numericEntry.value = null }, { flush: 'sync' })
function confirm(value: number): void {
  if (!applyNumericEntry(value)) error.value = 'The field changed or is no longer available. Cancel and reopen it.'
}
</script>

<template>
  <NumberDialog
    v-if="numericEntry" :key="numericEntry.key" :title="numericEntry.label" label="Value"
    :unit="numericEntry.unit ?? ''" :initial="numericEntry.initial" :integer="numericEntry.integer" :error="error" expanded
    :effect="numericEntry.effect ?? 'Updates this field only. Apply or run its action separately.'" :action="numericEntry.action ?? 'Use value'"
    @confirm="confirm" @close="numericEntry = null"
  />
</template>
