<script setup lang="ts">
// A short name with an on-screen keyboard. The mill's touch screen has no keyboard of its own:
// GNOME shows none for the panel's window, and a touch in a fresh-gesture dialog never focuses
// a text field. The keys are buttons, so a tap always types; a physical keyboard types into
// the field as usual.
import { computed, ref } from 'vue'

import { vTooltip } from '../tooltip'
import AppIcon from './AppIcon.vue'

const props = defineProps<{
  /** Read by screen readers and tests. */
  label: string
  /** Shown while the field is empty: what an empty name stands for. */
  placeholder: string
  maxLength: number
  /** The first key replaces the name offered, as typing over a selection would. */
  replaceFirst: boolean
}>()
const text = defineModel<string>({ required: true })
const emit = defineEmits<{ enter: [] }>()

const fresh = ref(props.replaceFirst)
const shift = ref(false)
const rows = [
  ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0'],
  ['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'],
  ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '-'],
  ['z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '_'],
] as const
const shown = (key: string): string => shift.value ? key.toUpperCase() : key

function put(value: string): void {
  const base = fresh.value ? '' : text.value
  fresh.value = false
  if (base.length >= props.maxLength) return
  text.value = base + value
}
function press(key: string): void {
  put(shown(key))
  shift.value = false
}
function backspace(): void {
  fresh.value = false
  text.value = text.value.slice(0, -1)
}
function clear(): void {
  fresh.value = false
  text.value = ''
}
const shiftLabel = computed(() => shift.value ? 'Next letter is a capital' : 'Capital for the next letter')
</script>

<template>
  <div class="name-entry">
    <input
      v-model="text"
      class="field"
      type="text"
      autocomplete="off"
      spellcheck="false"
      :aria-label="label"
      :placeholder="placeholder"
      :maxlength="maxLength"
      @input="fresh = false"
      @keydown.enter.prevent="emit('enter')"
    />
    <div class="keys" role="group" :aria-label="`Keyboard for ${label}`">
      <div v-for="(row, index) in rows" :key="index" class="row">
        <button v-for="key in row" :key="key" class="btn key" type="button" :aria-label="`Type ${shown(key)}`" @click="press(key)">{{ shown(key) }}</button>
      </div>
      <div class="row">
        <button v-tooltip="shiftLabel" class="btn key wide" type="button" :class="{ on: shift }" :aria-pressed="shift" aria-label="Shift" @click="shift = !shift">Shift</button>
        <button class="btn key space" type="button" aria-label="Type a space" @click="put(' ')">Space</button>
        <button v-tooltip="'Delete the last character'" class="btn key wide" type="button" aria-label="Delete the last character" @click="backspace"><AppIcon name="backspace" /></button>
        <button class="btn key wide" type="button" aria-label="Clear the name" @click="clear">Clear</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.name-entry { display: flex; flex-direction: column; gap: 8px; }
.field { min-height: 48px; font-size: 20px; }
.keys { display: flex; flex-direction: column; gap: 5px; }
.row { display: flex; gap: 5px; }
.key { flex: 1 1 0; min-width: 0; min-height: 44px; padding: 0; font-size: 18px; }
.key.wide { flex-grow: 1.5; font-size: 15px; }
.key.space { flex-grow: 4; font-size: 15px; }
</style>
