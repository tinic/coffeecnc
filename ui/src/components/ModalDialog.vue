<script setup lang="ts">
import { vTooltip } from '../tooltip'
// A dialog over the workspace. It never covers the status bar or the run bar: stop and E-stop
// stay reachable whatever is open.
import { onBeforeUnmount, onMounted } from 'vue'

import { openDialogs } from '../dialogs'
import { vFreshGesture } from '../numeric-input'
import AppIcon from './AppIcon.vue'

defineProps<{ title: string; size: 'small' | 'medium' | 'large'; contained?: boolean; expanded?: boolean; freshGesture?: boolean }>()
const emit = defineEmits<{ close: [] }>()

onMounted(() => {
  openDialogs.value += 1
})
onBeforeUnmount(() => {
  openDialogs.value -= 1
})
</script>

<template>
  <Teleport defer :to="expanded ? '#workspace-dialogs' : '#dialogs'">
    <div v-fresh-gesture="freshGesture" class="scrim" @pointerdown.self="emit('close')" @touchend.self.prevent="emit('close')" @keydown.esc="emit('close')">
      <section class="dialog" :class="size" role="dialog" :aria-label="title">
        <header>
          <h2>{{ title }}</h2>
          <button v-tooltip="'Close'" aria-label="Close" class="btn quiet square" type="button" @click="emit('close')">
            <AppIcon name="close" />
          </button>
        </header>
        <div class="body" :class="{ contained }">
          <slot />
        </div>
        <footer v-if="$slots['footer']">
          <slot name="footer" />
        </footer>
      </section>
    </div>
  </Teleport>
</template>

<style scoped>
.scrim {
  position: absolute;
  inset: 0;
  z-index: 20;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 16px;
  background: rgb(8 10 12 / 72%);
  pointer-events: auto;
}

.dialog {
  display: flex;
  flex-direction: column;
  max-width: 100%;
  max-height: 100%;
  border: 1px solid var(--line-strong);
  border-radius: 12px;
  background: var(--panel-raised);
  box-shadow: 0 18px 50px rgb(0 0 0 / 50%);
}

.dialog.small {
  width: 420px;
}

.dialog.medium {
  width: 620px;
}

.dialog.large {
  width: 860px;
  height: 100%;
}

header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 8px 8px 8px 16px;
  border-bottom: 1px solid var(--line);
}

h2 {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
}

.body {
  display: flex;
  flex-direction: column;
  gap: 12px;
  min-height: 0;
  padding: 16px;
  overflow: auto;
}

.body.contained { flex: 1; overflow: hidden; }

footer {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  padding: 12px 16px;
  border-top: 1px solid var(--line);
}
</style>
