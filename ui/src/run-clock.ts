// Times program runs while the panel is open, whatever workspace is shown: elapsed time of the
// run in progress, the length of the last run that reached its end, and how many did (parts).
// A run that stops early leaves a restart hint behind, so it is neither a part nor a cycle time.

import { computed, reactive, watch } from 'vue'

import { panel } from './machine'

export const runClock = reactive({
  /** Milliseconds since the epoch when the run in progress began; null when none is. */
  startedAt: null as number | null,
  now: Date.now(),
  /** Seconds the last completed run took; null before the first. */
  lastSeconds: null as number | null,
  /** Completed runs of `file` since it was loaded. */
  parts: 0,
  file: null as string | null,
})

const executing = computed(() => {
  const s = panel.status
  if (!s) return false
  return s.machine === 'on' && s.mode === 'auto' && (s.interp !== 'idle' || s.wait.kind !== 'none')
})

watch(() => panel.status?.program.file ?? null, (file) => {
  if (file === runClock.file) return
  runClock.file = file
  runClock.parts = 0
  runClock.lastSeconds = null
})

watch(executing, (running, was) => {
  const now = Date.now()
  runClock.now = now
  if (running && !was) {
    runClock.startedAt = now
  } else if (!running && was && runClock.startedAt !== null) {
    const completed = panel.status?.machine === 'on' && panel.status.program.restart_hint === undefined
    if (completed) {
      runClock.lastSeconds = (now - runClock.startedAt) / 1000
      runClock.parts += 1
    }
    runClock.startedAt = null
  }
})

setInterval(() => {
  if (runClock.startedAt !== null) runClock.now = Date.now()
}, 1000)

/** Seconds since the run in progress began, or null. */
export const elapsedSeconds = computed(() =>
  runClock.startedAt === null ? null : Math.max(0, (runClock.now - runClock.startedAt) / 1000))
