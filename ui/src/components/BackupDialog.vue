<script setup lang="ts">
// Backup and restore of the machine's data (#252). A backup goes to a folder chosen in the file
// browser, a USB stick for example. A restore is checked first: the dialog shows what would
// change, and only then offers to restore, which stops CoffeeCNC and LinuxCNC.
import { computed, onBeforeUnmount, ref, shallowRef } from 'vue'
import { useI18n } from 'vue-i18n'

import { translateMessage } from '../i18n/messages'
import { live, onResult, panel, send } from '../machine'
import type { BackupRole, CommandBody, RestoreSummary } from '../protocol'
import { formatBytes } from '../units'
import AppIcon from './AppIcon.vue'
import FileBrowser from './FileBrowser.vue'
import ModalDialog from './ModalDialog.vue'

const { t, d } = useI18n()
const emit = defineEmits<{ close: [] }>()

const browsing = ref<'folder' | 'file' | null>(null)
const request = shallowRef<CommandBody | null>(null)
const error = ref('')
const written = ref<string | null>(null)
const summary = shallowRef<RestoreSummary | null>(null)
const restored = ref(false)

// The policy's own refusal, shown before the operator asks: the backend checks again.
const restoreBlock = computed(() => {
  if (!live.value || panel.status === null) return t('status.backup.waitingLive')
  if (panel.status.machine === 'on') return translateMessage('Switch the machine off or press E-stop before a restore.')
  if (panel.status.interp !== 'idle') return translateMessage('Stop the program before a restore.')
  return null
})
const changes = computed(() => summary.value?.files.filter((file) => file.change !== 'same') ?? [])
const restartsLinuxcnc = computed(() => changes.value.some((file) => file.role !== 'data'))

function roleName(role: BackupRole): string {
  const names: Record<BackupRole, string> = {
    ini: t('status.backup.role.ini'), hal: t('status.backup.role.hal'), tool_table: t('status.backup.role.tool_table'),
    parameters: t('status.backup.role.parameters'), data: t('status.backup.role.data'),
  }
  return names[role]
}
function changeName(change: RestoreSummary['files'][number]['change']): string {
  return change === 'new' ? t('status.backup.change.new') : change === 'changed' ? t('status.backup.change.changed') : t('status.backup.change.same')
}

function run(command: CommandBody): void {
  error.value = ''
  // Install before sending: the browser simulator answers synchronously.
  request.value = command
  send(command)
}

function pick(path: string): void {
  const mode = browsing.value
  browsing.value = null
  if (mode === 'folder') {
    written.value = null
    run({ type: 'backup', directory: path })
  } else {
    summary.value = null
    panel.restoreSummary = null
    run({ type: 'check_restore', path })
  }
}

function restore(): void {
  const checked = summary.value
  if (checked === null || restoreBlock.value !== null) return
  run({ type: 'restore', path: checked.path, token: checked.token })
}

const removeResultListener = onResult((command, result) => {
  if (request.value !== command) return
  request.value = null
  if (result.outcome !== 'accepted') {
    error.value = result.reason
    return
  }
  if (command.type === 'backup') written.value = panel.backupWritten?.path ?? ''
  else if (command.type === 'check_restore') summary.value = panel.restoreSummary
  else if (command.type === 'restore') {
    restored.value = true
    if (result.reason !== '') error.value = result.reason
  }
})
onBeforeUnmount(removeResultListener)
</script>

<template>
  <ModalDialog :title="t('status.backup.title')" size="large" @close="request === null && emit('close')">
    <div class="sections">
      <section>
        <h3>{{ t('status.backup.backupTitle') }}</h3>
        <p class="note">{{ t('status.backup.backupWhat') }}</p>
        <button class="btn" type="button" :disabled="request !== null" @click="browsing = 'folder'">
          <AppIcon name="folder" />{{ t('status.backup.backupAction') }}
        </button>
        <p v-if="written !== null" class="feedback" role="status">
          {{ t('status.backup.written', { path: written, files: panel.backupWritten?.files ?? 0, size: formatBytes(panel.backupWritten?.bytes ?? 0) }) }}
        </p>
        <ul v-if="written !== null && panel.backupWritten?.not_included.length" class="left-out">
          <li v-for="item in panel.backupWritten.not_included" :key="item.path" class="note">
            {{ t('status.backup.notIncluded', { path: item.path, reason: translateMessage(item.reason) }) }}
          </li>
        </ul>
      </section>
      <section>
        <h3>{{ t('status.backup.restoreTitle') }}</h3>
        <p class="note">{{ t('status.backup.restoreWhat') }}</p>
        <button class="btn" type="button" :disabled="request !== null || restoreBlock !== null || restored" @click="browsing = 'file'">
          <AppIcon name="file" />{{ t('status.backup.chooseBackup') }}
        </button>
        <p v-if="restoreBlock && !restored" class="note" role="status">{{ restoreBlock }}</p>
        <template v-if="summary">
          <p class="mono where">{{ summary.path }}</p>
          <p class="note">
            {{ summary.created > 0 ? t('status.backup.created', { time: d(summary.created, 'short') }) : '' }}
            {{ summary.machine_id ? t('status.backup.machine', { machine: summary.machine_id }) : '' }}
          </p>
          <table class="files">
            <tbody>
              <tr v-for="file in summary.files" :key="file.name" :class="file.change">
                <td>{{ roleName(file.role) }}</td>
                <td class="mono target">{{ file.target }}</td>
                <td class="change">{{ changeName(file.change) }}</td>
              </tr>
            </tbody>
          </table>
          <p v-if="changes.length === 0" class="note">{{ t('status.backup.nothingChanges') }}</p>
          <p v-else class="warning">
            {{ t('status.backup.restoreEffect') }}
            <template v-if="restartsLinuxcnc"> {{ t('status.backup.restartNeeded') }}</template>
          </p>
        </template>
        <p v-if="restored" class="feedback" role="status">{{ panel.simulated ? t('status.backup.simulatedDone') : t('status.backup.restoring') }}</p>
      </section>
    </div>
    <p v-if="error" class="failure" role="alert">{{ translateMessage(error) }}</p>
    <template #footer>
      <button class="btn" type="button" :disabled="request !== null" @click="emit('close')">{{ t('common.close') }}</button>
      <button
        class="btn danger" type="button"
        :disabled="summary === null || changes.length === 0 || restoreBlock !== null || request !== null || restored"
        @click="restore"
      >
        {{ changes.length > 0 ? t('status.backup.restoreAction', changes.length) : t('status.backup.restoreTitle') }}
      </button>
    </template>
  </ModalDialog>
  <FileBrowser
    v-if="browsing" :pick="browsing"
    :title="browsing === 'folder' ? t('status.backup.chooseFolder') : t('status.backup.chooseBackup')"
    @pick="pick" @close="browsing = null"
  />
</template>

<style scoped>
.sections { display: grid; grid-template-columns: 1fr 1fr; gap: 20px; }
section { display: flex; flex-direction: column; align-items: flex-start; gap: 10px; min-width: 0; }
h3 { margin: 0; font-size: 15px; }
p { margin: 0; line-height: 1.5; }
.note, .feedback { color: var(--text-dim); font-size: 13px; }
.warning { color: var(--warn); font-size: 13px; }
.failure { margin-top: 12px; color: var(--stop); font-size: 13px; }
.where { font-size: 12px; overflow-wrap: anywhere; }
.left-out { margin: 0; padding-left: 18px; }
.btn { min-height: 48px; }
.files { width: 100%; border-collapse: collapse; font-size: 13px; }
.files td { padding: 6px 4px; border-bottom: 1px solid var(--line); vertical-align: top; }
.target { overflow-wrap: anywhere; color: var(--text-dim); font-size: 12px; }
.change { white-space: nowrap; text-align: right; }
.changed .change, .new .change { color: var(--warn); font-weight: 600; }
@media (max-width: 900px) { .sections { grid-template-columns: 1fr; } }
</style>
