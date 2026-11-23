<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, shallowRef, useTemplateRef } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import { live, onResult, panel, send } from '../machine'
import type { CommandBody, CommandResult } from '../protocol'
import ModalDialog from './ModalDialog.vue'

const { t } = useI18n()
const emit = defineEmits<{ close: [] }>()
const openedHello = panel.hello
const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
const cancel = useTemplateRef<HTMLButtonElement>('cancel')
const request = shallowRef<CommandBody | null>(null)
const accepted = ref(false)
const failure = shallowRef<CommandResult | null>(null)
const blocked = computed(() => {
  if (panel.hello !== openedHello) return t('status.quit.connectionChanged')
  if (panel.hello?.quit_supported !== true) return t('status.quit.unsupported')
  if (!live.value || panel.status === null) return t('status.quit.waitingLive')
  if (panel.status.interp !== 'idle') return t('status.quit.stopProgram')
  return null
})
function close(): void {
  if (request.value === null) emit('close')
}
function confirm(): void {
  if (blocked.value || request.value || accepted.value) return
  failure.value = null
  // Install before sending: the browser simulator answers synchronously.
  const command: CommandBody = { type: 'quit' }
  request.value = command
  send(command)
}
const removeResultListener = onResult((command, result) => {
  if (request.value !== command) return
  request.value = null
  if (result.outcome === 'accepted') {
    accepted.value = true
  } else {
    failure.value = result
  }
})
onMounted(() => { cancel.value?.focus({ preventScroll: true }) })
onBeforeUnmount(() => {
  removeResultListener()
  previousFocus?.focus({ preventScroll: true })
})
</script>

<template>
  <ModalDialog :title="t('status.quit.title')" size="small" fresh-gesture @close="close">
    <p class="effect">{{ t('status.quit.effect') }}</p>
    <p v-if="panel.simulated" class="note">{{ t('status.quit.simulatorNote') }}</p>
    <p v-if="failure" class="failure" role="alert">{{ translateMessage(failure.reason, failure.refusal) || t('status.quit.notConfirmed') }}</p>
    <p v-if="accepted" class="feedback" role="status">{{ panel.simulated ? t('status.quit.simulatedDone') : t('status.quit.accepted') }}</p>
    <p v-else-if="request" class="feedback" role="status">{{ t('status.quit.waiting') }}</p>
    <p v-else-if="blocked" class="note" role="status">{{ blocked }}</p>
    <template #footer>
      <button ref="cancel" class="btn" type="button" :disabled="request !== null" @click="close">{{ accepted ? t('status.quit.dismiss') : t('common.cancel') }}</button>
      <button class="btn danger" type="button" :disabled="blocked !== null || request !== null || accepted" @click="confirm">{{ t('status.quit.quit') }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.5; }
.effect { font-size: 16px; }
.note, .feedback { color: var(--text-dim); font-size: 13px; }
.failure { color: var(--stop); font-size: 13px; }
.btn { min-height: 48px; }
</style>
