<script setup lang="ts">
// Inline icons: 24 × 24 outlines drawn here, so the panel loads nothing from anywhere.
import { computed } from 'vue'

import { icons, type IconName } from '../icons'

const props = defineProps<{ name: IconName }>()

const icon = computed(() => icons[props.name])
</script>

<template>
  <svg
    class="icon"
    viewBox="0 0 24 24"
    width="20"
    height="20"
    aria-hidden="true"
    :fill="icon.solid ? 'currentColor' : 'none'"
    :stroke="icon.solid ? 'none' : 'currentColor'"
    stroke-width="1.8"
    stroke-linecap="round"
    stroke-linejoin="round"
  >
    <path :d="icon.path" />
  </svg>
</template>

<style scoped>
.icon {
  flex: none;
}
</style>
