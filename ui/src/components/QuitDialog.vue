<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, shallowRef, useTemplateRef } from 'vue'
import { live, onResult, panel, send } from '../machine'
import type { CommandBody } from '../protocol'
import ModalDialog from './ModalDialog.vue'

const emit = defineEmits<{ close: [] }>()
const openedHello = panel.hello
const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
const cancel = useTemplateRef<HTMLButtonElement>('cancel')
const request = shallowRef<CommandBody | null>(null)
const accepted = ref(false)
const error = ref('')
const blocked = computed(() => {
  if (panel.hello !== openedHello) return 'The connection changed. Close this dialog and try again.'
  if (panel.hello?.quit_supported !== true) return 'This backend does not support quitting from the panel.'
  if (!live.value || panel.status === null) return 'Waiting for a live machine connection.'
  if (panel.status.interp !== 'idle') return 'Stop the program before quitting.'
  return null
})
function close(): void {
  if (request.value === null) emit('close')
}
function confirm(): void {
  if (blocked.value || request.value || accepted.value) return
  error.value = ''
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
    error.value = result.reason || 'Quit was not confirmed. CoffeeCNC may still be running.'
  }
})
onMounted(() => { cancel.value?.focus({ preventScroll: true }) })
onBeforeUnmount(() => {
  removeResultListener()
  previousFocus?.focus({ preventScroll: true })
})
</script>

<template>
  <ModalDialog title="Quit CoffeeCNC?" size="small" fresh-gesture @close="close">
    <p class="effect">LinuxCNC shuts down; the machine is switched off.</p>
    <p v-if="panel.simulated" class="note">Browser simulator: the simulated machine switches off. This browser stays open.</p>
    <p v-if="error" class="failure" role="alert">{{ error }}</p>
    <p v-if="accepted" class="feedback" role="status">{{ panel.simulated ? 'Simulated quit completed. This browser stays open.' : 'Quit request accepted. Waiting for CoffeeCNC to close…' }}</p>
    <p v-else-if="request" class="feedback" role="status">Waiting for the quit request to be confirmed…</p>
    <p v-else-if="blocked" class="note" role="status">{{ blocked }}</p>
    <template #footer>
      <button ref="cancel" class="btn" type="button" :disabled="request !== null" @click="close">{{ accepted ? 'Dismiss' : 'Cancel' }}</button>
      <button class="btn danger" type="button" :disabled="blocked !== null || request !== null || accepted" @click="confirm">Quit CoffeeCNC</button>
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
