<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { panel } from '../machine'
import { applyNumericEntry, numericEntry, trackNumericInput } from '../numeric-input'
import NumberDialog from './NumberDialog.vue'

const { t } = useI18n()
const changed = ref(false)
onBeforeUnmount(trackNumericInput())
watch(numericEntry, () => { changed.value = false })
watch([(): unknown => panel.hello, (): string => panel.unit, (): string => panel.tab], () => { numericEntry.value = null }, { flush: 'sync' })
function confirm(value: number): void {
  if (!applyNumericEntry(value)) changed.value = true
}
</script>

<template>
  <NumberDialog
    v-if="numericEntry" :key="numericEntry.key" :title="numericEntry.label" :label="t('status.number.value')"
    :unit="numericEntry.unit ?? ''" :initial="numericEntry.initial" :integer="numericEntry.integer" :error="changed ? t('status.number.fieldChanged') : ''" expanded
    :effect="numericEntry.effect ?? t('status.number.effect')" :action="numericEntry.action ?? t('status.number.useValue')"
    @confirm="confirm" @close="numericEntry = null"
  />
</template>
