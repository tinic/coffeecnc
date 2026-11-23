<script setup lang="ts">
import { ref, shallowRef } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import ModalDialog from './ModalDialog.vue'

const { t } = useI18n()
const emit = defineEmits<{ close: [] }>()
const file = shallowRef<File | null>(null)
const replace = ref(false)
const uploading = ref(false)
const saved = ref('')
const problem = ref('')

function select(files: FileList | null): void {
  if (uploading.value || files?.length !== 1) return
  file.value = files.item(0)
  replace.value = false
  saved.value = ''
  problem.value = ''
}

function choose(event: Event): void {
  const input = event.target as HTMLInputElement
  select(input.files)
  input.value = ''
}

async function send(): Promise<void> {
  const selected = file.value
  if (uploading.value || selected === null) return
  uploading.value = true
  problem.value = ''
  try {
    const response = await fetch('/program-upload', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'X-CoffeeCNC-Filename': encodeURIComponent(selected.name),
        'X-CoffeeCNC-Replace': replace.value ? '1' : '0',
      },
      body: selected,
    })
    if (!response.ok) {
      const result: unknown = await response.json().catch(() => null)
      problem.value = result !== null && typeof result === 'object' && 'reason' in result &&
        typeof result.reason === 'string' && result.reason
        ? translateMessage(result.reason)
        : t('program.upload.failed')
      return
    }
    saved.value = selected.name
    file.value = null
    replace.value = false
  } catch {
    problem.value = t('program.upload.failed')
  } finally {
    uploading.value = false
  }
}
</script>

<template>
  <ModalDialog :title="t('program.upload.title')" size="small" :close-disabled="uploading" @close="!uploading && emit('close')">
    <form class="upload" @submit.prevent="send">
      <label class="drop" @dragover.prevent @drop.prevent="select($event.dataTransfer?.files ?? null)">
        <span>{{ t('program.upload.choose') }}</span>
        <strong v-if="file" class="filename">{{ file.name }}</strong>
        <input type="file" :aria-label="t('program.upload.choose')" :disabled="uploading" @change="choose" />
      </label>
      <label class="replace">
        <input v-model="replace" type="checkbox" :disabled="uploading || file === null" />
        {{ t('program.upload.replace') }}
      </label>
      <p v-if="problem" role="alert">{{ problem }}</p>
      <p v-else-if="saved" role="status">{{ t('program.upload.saved', { name: saved }) }}</p>
      <button class="btn primary" type="submit" :disabled="file === null || uploading">
        {{ uploading ? t('program.upload.uploading') : t('program.upload.title') }}
      </button>
    </form>
  </ModalDialog>
</template>

<style scoped>
.upload { display: grid; gap: 12px; }
.drop { position: relative; display: grid; gap: 4px; min-height: 52px; padding: 16px; border: 1px dashed var(--line-strong); border-radius: var(--radius); background: var(--bg-sunken); cursor: pointer; }
.drop:focus-within { outline: 2px solid var(--accent); }
.drop input { position: absolute; inset: 0; width: 100%; opacity: 0; cursor: inherit; }
.filename { overflow-wrap: anywhere; }
.replace { display: flex; align-items: center; gap: 8px; min-height: 44px; }
p { margin: 0; }
[role="alert"] { color: var(--stop); }
.btn { justify-self: end; }
</style>
