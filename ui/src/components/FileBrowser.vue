<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Browses the machine's program directory and opens a program.
import { computed, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'

import { panel, send } from '../machine'
import type { DirectoryEntry, ManageProgramFile } from '../protocol'
import { baseName, formatBytes } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'
import ProgramFileActions from './ProgramFileActions.vue'
import { translateMessage } from '../i18n/messages'

const { t } = useI18n()

const props = defineProps<{ pick?: 'folder' | 'file'; title?: string }>()
const emit = defineEmits<{ close: [] }>()

const directory = computed(() => panel.directory)
// Shortcuts the machine offers: its program directory, the home folder, USB media.
const places = computed(() => panel.hello?.program_places ?? [])
const programs = computed(() => places.value.find((place) => place.name === 'Programs')) // i18n-ignore: native shortcut identifier
const writable = computed(() => props.pick === undefined && directory.value?.writable === true)
const copyable = computed(() => props.pick === undefined && directory.value?.writable === false && programs.value !== undefined)
const action = ref<{ entry: DirectoryEntry | null; directory: string; writable: boolean } | null>(null)
const feedback = ref('')

function fileActions(entry: DirectoryEntry | null): void {
  if (directory.value === null) return
  action.value = { entry, directory: directory.value.path, writable: writable.value }
  feedback.value = ''
}

function changed(operation: ManageProgramFile['operation']): void {
  const previous = action.value
  action.value = null
  if (operation === 'copy')
    feedback.value = t('program.fileBrowser.copied', { name: previous?.entry?.name })
  else if (previous !== null)
    list(previous.directory)
}

function list(path: string | null): void {
  send({ type: 'list_directory', path })
}

function choose(entry: DirectoryEntry): void {
  const path = `${directory.value?.path ?? ''}/${entry.name}`
  if (entry.kind === 'directory') {
    list(path)
  } else {
    open(path)
  }
}

// Recently opened programs, newest first; the loaded one is already open.
const recent = computed(() => panel.preferences.recent_programs.filter((path) => path !== panel.status?.program.file).slice(0, 5))
function open(path: string): void {
  send({ type: 'open_program', path })
  emit('close')
}
function folder(path: string): string {
  return path.slice(0, path.lastIndexOf('/')) || '/'
}

function formatDate(time: number): string {
  return new Date(time).toISOString().slice(0, 10)
}

onMounted(() => {
  const file = panel.status?.program.file ?? null
  list(file === null ? null : file.slice(0, file.lastIndexOf('/')))
})
</script>

<template>
  <ModalDialog :title="t('program.fileBrowser.title')" :size="recent.length ? 'large' : 'medium'" :contained="recent.length > 0" @close="emit('close')">
    <div class="browser" :class="{ 'has-recent': recent.length > 0 }">
      <section v-if="recent.length > 0" class="recent" :aria-label="t('program.fileBrowser.recentPrograms')">
        <h3>{{ t('program.fileBrowser.recent') }}</h3>
        <button v-for="path in recent" :key="path" v-tooltip="path" class="entry list-row" type="button" @click="open(path)">
          <AppIcon name="file" />
          <span class="recent-path">
            <span class="name">{{ baseName(path) }}</span>
            <span class="meta mono">{{ folder(path) }}</span>
          </span>
        </button>
      </section>
      <section class="directory" :aria-label="t('program.fileBrowser.directory')">
        <div class="path">
          <button
            v-tooltip="t('program.fileBrowser.upTooltip')"
            class="btn"
            type="button"
            :disabled="directory === null || directory.parent === null"
            @click="list(directory?.parent ?? null)"
          >
            <AppIcon name="up" />{{ t('program.fileBrowser.up') }}
          </button>
          <span class="mono where">{{ directory?.path ?? t('program.fileBrowser.reading') }}</span>
          <button v-if="writable" class="btn new-folder" type="button" @click="fileActions(null)">
            <AppIcon name="plus" />{{ t('program.fileBrowser.newFolder') }}
          </button>
        </div>
        <div v-if="places.length > 0" class="places" :aria-label="t('program.fileBrowser.places')">
          <button
            v-for="place in places" :key="place.path" class="btn" type="button"
            :class="{ on: directory?.path === place.path }" @click="list(place.path)"
          >
            {{ translateMessage(place.name) }}
          </button>
        </div>
        <div class="entries">
          <div v-for="entry in directory?.entries ?? []" :key="entry.name" class="entry-row">
            <button class="entry list-row" type="button" @click="choose(entry)">
              <AppIcon :name="entry.kind === 'directory' ? 'folder' : 'file'" />
              <span class="name">{{ entry.name }}</span>
              <span v-if="entry.kind === 'file'" class="meta mono">{{ formatBytes(entry.bytes) }}</span>
              <span class="meta mono">{{ formatDate(entry.modified) }}</span>
            </button>
            <button
              v-if="writable || (copyable && entry.kind === 'file')"
              v-tooltip="t('program.fileBrowser.actions', { name: entry.name })"
              class="btn quiet square entry-actions"
              type="button" :aria-label="t('program.fileBrowser.actions', { name: entry.name })"
              @click="fileActions(entry)"
            >
              <span aria-hidden="true">…</span>
            </button>
          </div>
          <p v-if="directory !== null && directory.entries.length === 0" class="hint">{{ directory.truncated ? t('program.fileBrowser.partialEmpty') : t('program.fileBrowser.empty') }}</p>
        </div>
        <p v-if="directory?.truncated" class="hint" role="status">{{ t('program.fileBrowser.partial') }}</p>
        <p v-if="feedback" class="hint" role="status">{{ feedback }}</p>
        <p v-if="panel.status && panel.status.interp !== 'idle'" class="blocked">{{ t('program.fileBrowser.stopFirst') }}</p>
      </section>
    </div>
  </ModalDialog>
  <ProgramFileActions v-if="action" v-bind="action" @close="action = null" @done="changed" />
</template>

<style scoped>
.places { display: flex; flex-wrap: wrap; gap: 6px; margin: 8px 0 4px; }
.places .btn { min-height: 36px; font-size: 12px; }
.browser { display: grid; flex: 1; min-height: 0; min-width: 0; }
.has-recent { grid-template-columns: minmax(0, 2fr) minmax(0, 3fr); gap: 16px; }
.directory, .recent { display: flex; flex-direction: column; min-height: 0; min-width: 0; }
.directory { gap: 8px; }
.recent { padding-right: 16px; border-right: 1px solid var(--line); overflow-y: auto; }
.recent h3 { margin: 0 0 6px; font-size: 11px; font-weight: 600; letter-spacing: .6px; text-transform: uppercase; color: var(--text-dim); }
.recent-path { display: grid; flex: 1; min-width: 0; gap: 2px; }
.recent .meta { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 11px; }

.path {
  display: flex;
  align-items: center;
  gap: 10px;
  flex: none;
}

.new-folder { margin-left: auto; flex: none; }
.entry-row { display: flex; align-items: center; min-width: 0; }
.entry-row .entry { flex: 1; min-width: 0; }
.entry-actions { flex: none; font-size: 20px; }

.where {
  overflow: hidden;
  font-size: 13px;
  color: var(--text-dim);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.entries {
  display: flex;
  flex-direction: column;
  flex: 1;
  gap: 2px;
  min-height: 220px;
}

.has-recent .entries { min-height: 0; overflow-y: auto; }

.entry {
  display: flex;
  align-items: center;
  flex: none;
  gap: 10px;
  min-height: 48px;
  padding: 0 12px;
  text-align: left;
}

.name {
  flex: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.meta {
  font-size: 12px;
  color: var(--text-dim);
}

.blocked,
.hint {
  margin: 8px;
}

@media (max-width: 720px) {
  .has-recent { grid-template-columns: minmax(0, 1fr); grid-template-rows: minmax(0, 1fr) minmax(0, 1fr); }
  .recent { padding-right: 0; padding-bottom: 8px; border-right: 0; border-bottom: 1px solid var(--line); }
}
</style>
