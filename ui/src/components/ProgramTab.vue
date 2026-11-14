<script setup lang="ts">
// Program workspace: the NC text and the path view as peers. Either can take the whole space; a
// program without a usable preview is still a program.
import { computed, ref, useTemplateRef } from 'vue'

import { startDialog } from '../dialogs'
import { live, panel, send } from '../machine'
import type { ProgramLayout } from '../store'
import { baseName } from '../units'
import AppIcon from './AppIcon.vue'
import FileBrowser from './FileBrowser.vue'
import NcText from './NcText.vue'
import PathView from './PathView.vue'

const browsing = ref<boolean>(startDialog === 'open')
const text = useTemplateRef<InstanceType<typeof NcText>>('text')

const status = computed(() => panel.status)
const program = computed(() => panel.program)
const file = computed(() => status.value?.program.file ?? null)

/** The text shown belongs to the program the machine has loaded. */
const textCurrent = computed(
  () => program.value !== null && status.value !== null && program.value.revision === status.value.program.revision,
)

const preview = computed(() => panel.preview)
const previewCurrent = computed(
  () => preview.value !== null && status.value !== null && preview.value.context === status.value.preview_context,
)
const failure = computed(() =>
  preview.value?.state === 'failed' && previewCurrent.value ? preview.value.error : null,
)

const layouts: readonly (readonly [ProgramLayout, string])[] = [
  ['split', 'Split'],
  ['text', 'Text'],
  ['path', 'Path'],
]

function option(which: 'optional_stop' | 'block_delete'): void {
  const now = status.value?.program[which] ?? false
  send({ type: 'set_program_option', option: which, on: !now })
}
</script>

<template>
  <div class="program">
    <div class="toolbar">
      <button class="btn" type="button" :disabled="!live" @click="browsing = true"><AppIcon name="folder" />Open</button>
      <div class="file">
        <span class="name" :title="file ?? ''">{{ file === null ? 'No program loaded' : baseName(file) }}</span>
        <span class="hint">
          <template v-if="program !== null && textCurrent">{{ program.lines.length }} lines · {{ program.file }}</template>
          <template v-else-if="file !== null">Program text not received</template>
          <template v-else>Open a program from the machine's program directory.</template>
        </span>
      </div>
      <button
        class="btn"
        type="button"
        :class="{ on: status?.program.optional_stop }"
        :disabled="!live"
        title="Pause at every M1"
        @click="option('optional_stop')"
      >
        Optional stop <span class="mono">M1</span>
      </button>
      <button
        class="btn"
        type="button"
        :class="{ on: status?.program.block_delete }"
        :disabled="!live"
        title="Skip the blocks that begin with /"
        @click="option('block_delete')"
      >
        Block delete <span class="mono">/</span>
      </button>
      <div class="segmented">
        <button
          v-for="[layout, label] in layouts"
          :key="layout"
          class="btn"
          type="button"
          :class="{ on: panel.programLayout === layout }"
          @click="panel.programLayout = layout"
        >
          {{ label }}
        </button>
      </div>
    </div>

    <div v-if="failure" class="failure">
      <AppIcon name="warning" />
      <span class="what">
        <strong>The interpreter stopped the preview.</strong>
        <span class="mono">{{ failure.text }}</span>
      </span>
      <button class="btn" type="button" @click="text?.goTo(failure.line)">Show line {{ failure.line }}</button>
    </div>

    <div class="views" :class="panel.programLayout">
      <template v-if="panel.programLayout !== 'path'">
        <NcText
          v-if="program !== null && textCurrent"
          ref="text"
          :lines="program.lines"
          :motion-line="live ? (status?.program.motion_line ?? 0) : 0"
          :current-line="live ? (status?.program.current_line ?? 0) : 0"
          :error-line="failure?.line ?? 0"
        />
        <div v-else class="empty">
          <span>{{ file === null ? 'No program loaded' : 'Waiting for the program text' }}</span>
        </div>
      </template>
      <PathView v-if="panel.programLayout !== 'text'" />
    </div>

    <FileBrowser v-if="browsing" @close="browsing = false" />
  </div>
</template>

<style scoped>
.program {
  display: flex;
  flex-direction: column;
  gap: 10px;
  height: 100%;
  min-height: 0;
}

.toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
}

.file {
  display: flex;
  flex: 1;
  flex-direction: column;
  min-width: 0;
  padding: 0 6px;
}

.name {
  overflow: hidden;
  font-size: 16px;
  font-weight: 600;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.file .hint {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.toolbar .btn {
  padding: 0 12px;
}

.failure {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 8px 8px 8px 12px;
  border: 1px solid var(--stop);
  border-radius: var(--radius);
  background: var(--stop-soft);
  color: var(--stop);
}

.what {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
  color: var(--text);
}

.what .mono {
  font-size: 13px;
  user-select: text;
  -webkit-user-select: text;
}

.failure .btn {
  color: var(--text);
}

.views {
  display: grid;
  flex: 1;
  gap: 10px;
  min-height: 0;
}

.views.split {
  grid-template-columns: minmax(0, 5fr) minmax(0, 6fr);
}

.views.text,
.views.path {
  grid-template-columns: minmax(0, 1fr);
}

.empty {
  display: flex;
  align-items: center;
  justify-content: center;
  border: 1px dashed var(--line-strong);
  border-radius: var(--radius);
  color: var(--text-faint);
}
</style>
