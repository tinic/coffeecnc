<script setup lang="ts">
// Browses the machine's program directory and opens a program.
import { computed, onMounted } from 'vue'

import { panel, send } from '../machine'
import type { DirectoryEntry } from '../protocol'
import { formatBytes } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const emit = defineEmits<{ close: [] }>()

const directory = computed(() => panel.directory)

function list(path: string | null): void {
  send({ type: 'list_directory', path })
}

function choose(entry: DirectoryEntry): void {
  const path = `${directory.value?.path ?? ''}/${entry.name}`
  if (entry.kind === 'directory') {
    list(path)
  } else {
    send({ type: 'open_program', path })
    emit('close')
  }
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
  <ModalDialog title="Open program" size="medium" @close="emit('close')">
    <div class="path">
      <button
        class="btn"
        type="button"
        :disabled="directory === null || directory.parent === null"
        title="Up one directory"
        @click="list(directory?.parent ?? null)"
      >
        <AppIcon name="up" />Up
      </button>
      <span class="mono where">{{ directory?.path ?? 'Reading directory' }}</span>
    </div>
    <div class="entries">
      <button v-for="entry in directory?.entries ?? []" :key="entry.name" class="entry" type="button" @click="choose(entry)">
        <AppIcon :name="entry.kind === 'directory' ? 'folder' : 'file'" />
        <span class="name">{{ entry.name }}</span>
        <span v-if="entry.kind === 'file'" class="meta mono">{{ formatBytes(entry.bytes) }}</span>
        <span class="meta mono">{{ formatDate(entry.modified) }}</span>
      </button>
      <p v-if="directory !== null && directory.entries.length === 0" class="hint">This directory is empty.</p>
    </div>
    <p v-if="panel.status?.interp !== 'idle'" class="blocked">A program is running: the machine will refuse to open another.</p>
  </ModalDialog>
</template>

<style scoped>
.path {
  display: flex;
  align-items: center;
  gap: 10px;
}

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
  gap: 2px;
  min-height: 220px;
}

.entry {
  display: flex;
  align-items: center;
  gap: 10px;
  min-height: 48px;
  padding: 0 12px;
  border: 1px solid transparent;
  border-radius: var(--radius);
  background: none;
  text-align: left;
  cursor: pointer;
}

.entry:hover {
  border-color: var(--line-strong);
  background: var(--control);
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
</style>
