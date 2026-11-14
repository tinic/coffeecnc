<script setup lang="ts">
// Asks for one number, with an on-screen keypad for touch screens without a keyboard.
import { computed, onMounted, ref, useTemplateRef } from 'vue'

import { parseNumber } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{
  title: string
  /** What confirming will change, in words. */
  effect: string
  label: string
  unit: string
  initial: string
  /** Label of the confirming button. */
  action: string
}>()
const emit = defineEmits<{ confirm: [value: number]; close: [] }>()

const text = ref<string>(props.initial)
const fresh = ref<boolean>(true)
const input = useTemplateRef<HTMLInputElement>('input')

const value = computed(() => parseNumber(text.value))

const keys = ['7', '8', '9', '4', '5', '6', '1', '2', '3', '±', '0', '.'] as const

function press(key: (typeof keys)[number]): void {
  if (key === '±') {
    text.value = text.value.startsWith('-') ? text.value.slice(1) : `-${text.value}`
    fresh.value = false
    return
  }
  // The first key replaces the value offered, as typing over a selection would.
  const base = fresh.value ? '' : text.value
  fresh.value = false
  if (key === '.' && base.includes('.')) {
    text.value = base
    return
  }
  text.value = base + key
}

function backspace(): void {
  fresh.value = false
  text.value = text.value.slice(0, -1)
}

function confirm(): void {
  if (value.value !== null) {
    emit('confirm', value.value)
  }
}

onMounted(() => {
  input.value?.select()
})
</script>

<template>
  <ModalDialog :title="title" size="small" @close="emit('close')">
    <p class="effect">{{ effect }}</p>
    <label class="entry">
      <span class="label">{{ label }}</span>
      <span class="row">
        <input
          ref="input"
          v-model="text"
          class="field mono"
          type="text"
          inputmode="decimal"
          autocomplete="off"
          spellcheck="false"
          @input="fresh = false"
          @keydown.enter.prevent="confirm"
        />
        <span class="unit">{{ unit }}</span>
      </span>
    </label>
    <div class="keypad">
      <button v-for="key in keys" :key="key" class="btn mono" type="button" @click="press(key)">{{ key }}</button>
    </div>
    <div class="row">
      <button class="btn grow" type="button" title="Delete the last digit" @click="backspace">
        <AppIcon name="backspace" />
      </button>
      <button class="btn grow" type="button" @click="((text = '0'), (fresh = true))">Zero</button>
    </div>
    <template #footer>
      <button class="btn" type="button" @click="emit('close')">Cancel</button>
      <button class="btn primary" type="button" :disabled="value === null" @click="confirm">{{ action }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.effect {
  margin: 0;
  line-height: 1.45;
}

.entry {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.field {
  flex: 1;
  min-width: 0;
  font-size: 24px;
  text-align: right;
  min-height: 52px;
}

.unit {
  width: 40px;
  color: var(--text-dim);
}

.keypad {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 6px;
}

.keypad .btn {
  font-size: 18px;
}

.grow {
  flex: 1;
}
</style>
