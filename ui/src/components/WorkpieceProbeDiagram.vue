<script setup lang="ts">
import type { ProbeDirection } from '../protocol'


// `travel` is one face's probe travel; a corner lights the travels of both of its faces.
const props = defineProps<{ travel: ProbeDirection | null; corner?: readonly ProbeDirection[] | null }>()
const lit = (direction: ProbeDirection): boolean => props.travel === direction || (props.corner?.includes(direction) ?? false)
</script>

<template>
  <figure class="workpiece" aria-label="Workpiece viewed from above. The arrows point toward the edge as the probe moves.">
    <div class="approach back" :class="{ active: lit('y_minus') }">
      <span>Y+</span><i aria-hidden="true">↓</i>
    </div>
    <div class="approach left" :class="{ active: lit('x_plus') }">
      <span>X−</span><i aria-hidden="true">→</i>
    </div>
    <div class="part" :class="[travel, ...(corner ?? [])]">
      <span class="top-contact" aria-hidden="true"></span>
      <span>Z top</span>
    </div>
    <div class="approach right" :class="{ active: lit('x_minus') }">
      <i aria-hidden="true">←</i><span>X+</span>
    </div>
    <div class="approach front" :class="{ active: lit('y_plus') }">
      <i aria-hidden="true">↑</i><span>Y−</span>
    </div>
  </figure>
</template>

<style scoped>
.workpiece {
  display: grid;
  grid-template: 35px 66px 35px / 52px minmax(64px, 1fr) 52px;
  grid-template-areas: '. back .' 'left part right' '. front .';
  align-items: stretch;
  margin: 0;
  padding: 5px 7px;
  border: 1px solid var(--line);
  border-radius: 9px;
  background: var(--bg-sunken);
}
.approach { display: flex; align-items: center; justify-content: center; gap: 3px; font-size: 13px; font-weight: 650; color: var(--text); white-space: nowrap; }
.approach i { color: var(--accent); font-size: 20px; line-height: 17px; font-style: normal; }
.approach.active { color: var(--accent); }
.left { grid-area: left; justify-content: flex-end; padding-right: 4px; }
.right { grid-area: right; justify-content: flex-start; padding-left: 4px; }
.back { grid-area: back; flex-direction: column; gap: 0; }
.front { grid-area: front; flex-direction: column; gap: 0; }
.part {
  grid-area: part; display: flex; flex-direction: column; justify-content: center; align-items: center; gap: 4px;
  border: 2px solid color-mix(in srgb, var(--accent) 55%, var(--line));
  border-radius: 4px; background: var(--accent-soft); color: var(--text); font-size: 12px; font-weight: 600;
}
.part.x_plus { border-left-color: var(--accent); }
.part.x_minus { border-right-color: var(--accent); }
.part.y_plus { border-bottom-color: var(--accent); }
.part.y_minus { border-top-color: var(--accent); }
.top-contact { display: grid; place-items: center; width: 16px; height: 16px; border: 1px solid var(--accent); border-radius: 50%; }
.top-contact::after { content: ''; width: 5px; height: 5px; border-radius: 50%; background: var(--accent); }
.part.z_top .top-contact { box-shadow: 0 0 0 3px color-mix(in srgb, var(--accent) 32%, transparent); }
</style>
