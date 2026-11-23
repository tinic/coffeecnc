<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import { onResult, send } from '../machine'
import type { CommandBody, DirectoryEntry, ManageProgramFile } from '../protocol'
import ModalDialog from './ModalDialog.vue'

const { t } = useI18n()
const props = defineProps<{ directory: string; writable: boolean; entry: DirectoryEntry | null }>()
const emit = defineEmits<{ close: []; done: [operation: ManageProgramFile['operation']] }>()
const action = ref<'menu' | 'create_folder' | 'rename' | 'remove'>(props.entry ? 'menu' : 'create_folder')
const name = ref(props.entry?.name ?? '')
const validName = computed(() => !!name.value.trim() && !name.value.trim().startsWith('.') && !/[/\\]/.test(name.value))
const error = ref('')
const pending = ref(false)
let request: CommandBody | null = null

function begin(operation: 'rename' | 'remove'): void {
  error.value = ''
  action.value = operation
}

function apply(operation: ManageProgramFile['operation']): void {
  if (pending.value || ((operation === 'create_folder' || operation === 'rename') && !validName.value)) return
  const leaf = operation === 'create_folder' ? name.value.trim() : props.entry?.name
  if (!leaf) return
  const path = `${props.directory === '/' ? '' : props.directory}/${leaf}`
  request = operation === 'rename'
    ? { type: 'manage_program_file', operation, path, name: name.value.trim() }
    : { type: 'manage_program_file', operation, path }
  error.value = ''
  pending.value = true
  send(request)
}

onBeforeUnmount(onResult((command, result) => {
  if (command !== request || command.type !== 'manage_program_file') return
  request = null
  pending.value = false
  if (result.outcome === 'accepted') emit('done', command.operation)
  else error.value = result.reason ? translateMessage(result.reason) : t('common.unavailable')
}))
</script>

<template>
  <ModalDialog
    :title="action === 'create_folder' ? t('program.fileBrowser.newFolder') : entry?.name ?? ''" size="small"
    :close-disabled="pending" @close="!pending && emit('close')"
  >
    <div v-if="action === 'menu'" class="actions">
      <template v-if="writable">
        <button class="btn" type="button" :disabled="pending" @click="begin('rename')">{{ t('program.fileBrowser.rename') }}</button>
        <button class="btn" type="button" :disabled="pending" @click="begin('remove')">{{ t('common.delete') }}</button>
      </template>
      <button v-else class="btn primary" type="button" :disabled="pending" @click="apply('copy')">{{ t('program.fileBrowser.copyToPrograms') }}</button>
    </div>
    <form v-else-if="action === 'create_folder' || action === 'rename'" class="edit" @submit.prevent="apply(action)">
      <label>
        {{ t('program.fileBrowser.name') }}
        <input v-model="name" class="field" autocomplete="off" :disabled="pending" />
      </label>
      <button class="btn primary" type="submit" :disabled="pending || !validName || (action === 'rename' && name.trim() === entry?.name)">
        {{ action === 'rename' ? t('program.fileBrowser.rename') : t('program.fileBrowser.newFolder') }}
      </button>
    </form>
    <template v-else>
      <p>{{ t('program.fileBrowser.deleteConfirm', { name: entry?.name }) }}</p>
      <div class="actions">
        <button class="btn" type="button" :disabled="pending" @click="action = 'menu'">{{ t('common.back') }}</button>
        <button class="btn danger" type="button" :disabled="pending" @click="apply('remove')">{{ t('common.delete') }}</button>
      </div>
    </template>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
  </ModalDialog>
</template>

<style scoped>
.actions { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 8px; }
.edit, .edit label { display: grid; gap: 8px; }
.edit .btn { justify-self: end; }
p { margin: 0; overflow-wrap: anywhere; }
.error { color: var(--stop); }
</style>
