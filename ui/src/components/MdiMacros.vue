<script setup lang="ts">
// Macros of the MDI card ([COFFEECNC] MACRO, #211): one button that opens the list. A macro runs
// as one MDI block after a confirmation that shows it, under the rules of a typed block, and its
// result goes to the MDI history. Nothing shows when the machine has no macros.
import { computed, shallowRef } from 'vue'

import { mdiBlock, panel, send } from '../machine'
import type { Macro } from '../protocol'
import { vTooltip } from '../tooltip'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const macros = computed(() => panel.hello?.macros ?? [])
const open = shallowRef(false)
const chosen = shallowRef<Macro | null>(null)

function close(): void {
  open.value = false
  chosen.value = null
}

function run(): void {
  const macro = chosen.value
  if (macro === null || mdiBlock.value !== null) return
  send({ type: 'mdi', text: macro.mdi })
  close()
}
</script>

<template>
  <template v-if="macros.length > 0">
    <button v-tooltip="'Run a configured MDI macro'" class="btn" type="button" aria-haspopup="dialog" @click="open = true">
      <AppIcon name="list" />Macros
    </button>
    <ModalDialog v-if="open && chosen === null" title="Macros" size="small" fresh-gesture @close="close">
      <div class="macros">
        <button
          v-for="macro in macros"
          :key="macro.label"
          v-tooltip="macro.mdi"
          class="btn macro"
          type="button"
          @click="chosen = macro"
        >
          {{ macro.label }}
        </button>
      </div>
    </ModalDialog>
    <ModalDialog v-if="open && chosen !== null" :title="`Run ${chosen.label}?`" size="small" fresh-gesture @close="close">
      <p class="block mono">{{ chosen.mdi }}</p>
      <p v-if="mdiBlock !== null" class="refusal" role="alert">{{ mdiBlock }}</p>
      <p v-else>Runs as one MDI command. The result shows in the MDI history.</p>
      <template #footer>
        <button class="btn wide" type="button" @click="close">Cancel</button>
        <button v-tooltip="mdiBlock ?? 'Run this macro'" class="btn primary wide" type="button" :aria-disabled="mdiBlock !== null" @click="run">Run</button>
      </template>
    </ModalDialog>
  </template>
</template>

<style scoped>
.macros { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
.macro { min-height: 48px; padding-inline: 10px; overflow: hidden; text-overflow: ellipsis; }
.block { margin: 0; padding: 10px 12px; border: 1px solid var(--line); border-radius: var(--radius); background: var(--bg-sunken); font-size: 16px; overflow-wrap: anywhere; }
p { margin: 0; }
.refusal { color: var(--stop); }
.wide { min-height: 48px; }
.btn[aria-disabled="true"] { opacity: .68; }
</style>
