<script setup lang="ts">
const directions = [
  ['y+', 'y', 1, 'ypos', 'up'],
  ['x-', 'x', -1, 'xneg', 'left'],
  ['x+', 'x', 1, 'xpos', 'right'],
  ['y-', 'y', -1, 'yneg', 'down'],
  ['z+', 'z', 1, 'zpos', 'up'],
  ['z-', 'z', -1, 'zneg', 'down'],
] as const
</script>

<template>
  <div class="motion-pane">
    <div class="pad">
      <slot
        v-for="[jog, axis, direction, area, icon] in directions"
        :key="jog" name="direction" :jog="jog" :axis="axis" :direction="direction" :area="area" :icon="icon"
      />
      <slot name="stop" />
    </div>
    <slot />
  </div>
</template>

<style scoped>
.motion-pane {
  display: grid;
  grid-template-columns: 300px minmax(0, 1fr);
  align-items: center;
  gap: 12px;
  min-height: var(--motion-content-height);
}
.pad {
  --pad-key: 68px;
  display: grid;
  justify-self: center;
  grid-template-areas:
    '. ypos . gap zpos'
    'xneg stop xpos gap .'
    '. yneg . gap zneg';
  grid-template-columns: repeat(3, var(--pad-key)) 12px var(--pad-key);
  grid-template-rows: repeat(3, var(--pad-key));
  gap: 4px;
}
.pad :slotted(.jogbtn) { flex-direction: column; gap: 0; padding: 0; font-size: 20px; }
.pad :slotted(.stopjog), .pad :slotted(.stopfeed) { grid-area: stop; font-size: 13px; }
@media (max-height: 760px) {
  .pad { --pad-key: 60px; }
}
</style>
