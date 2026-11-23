// Optional page-local display attachments. Nothing in this module sends a controller command.
import { computed, markRaw, reactive, watch } from 'vue'
import { isRemoteScreen } from './bridge'
import { live, panel } from './machine'
import { t } from './i18n'
import { modelBlock } from './step-block'
import { frameOf, importTimeoutMs, maxStepBytes, type ImportReply, type ModelBinding, type ModelGeometry, type Placement } from './step-model'

interface Attachment { name: string; note: string; geometry: ModelGeometry; binding: ModelBinding }
interface PartState {
  attachment: Attachment | null; loading: boolean; error: string; visible: boolean; opacity: number
  placement: Placement; placed: boolean
}
export const part = reactive<PartState>({
  attachment: null, loading: false, error: '', visible: true, opacity: 0.8,
  placement: { translation: [0, 0, 0], rotation: [0, 0, 0] }, placed: false,
})
export const partBlock = computed(() => part.attachment === null ? '' : modelBlock(part.attachment.binding, panel.status, panel.preview, live.value))
export const showPart = computed(() => part.attachment !== null && part.visible && partBlock.value === '')
// Notes are kept in English as identifiers; `partNote` gives the operator's text.
const withProgram = 'Loaded with the program' // i18n-ignore
export const sampleNote = 'Sample geometry; not a matched model for this program' // i18n-ignore

/** An attachment's note in the panel's language. */
export function partNote(note: string): string {
  if (note === withProgram) return t('program.step.withProgram')
  if (note === sampleNote) return t('program.step.sample')
  return note
}
let generation = 0
/** The note of the import in progress. */
let importing = ''
let worker: Worker | null = null
let timer: ReturnType<typeof setTimeout> | undefined

export function cancelImport(): void {
  generation++
  worker?.terminate()
  worker = null
  clearTimeout(timer)
  part.loading = false
}

export function removePart(): void {
  cancelImport()
  part.attachment = null
  part.error = ''
  part.placed = false
}

watch([
  (): unknown => panel.status?.program.file,
  (): unknown => panel.status?.program.revision,
  (): unknown => isRemoteScreen() ? panel.hello : null,
  (): boolean => !isRemoteScreen() || live.value,
], () => {
  if (part.attachment !== null || part.loading) {
    // A model loaded with the program goes with it; only one the operator attached is reported.
    const attached = (part.attachment !== null && part.attachment.note !== withProgram) ||
      (part.loading && importing !== withProgram)
    removePart()
    if (attached) part.error = t('program.step.removed')
  }
  void loadProgramPart()
})

/** Where the native panel serves the STEP model beside a program (same folder and base name). */
export function programPartUrl(file: string): string {
  return `coffeecnc://panel/program-part?program=${encodeURIComponent(file)}`
}

/** Loads the model beside the loaded program, when there is one (turo: load it with the .nc). */
export async function loadProgramPart(fetcher: typeof fetch = fetch): Promise<void> {
  const file = panel.status?.program.file
  const revision = panel.status?.program.revision
  const remote = isRemoteScreen()
  const hello = panel.hello
  if (!file || panel.simulated || !live.value) return
  let bytes: ArrayBuffer
  try {
    const url = remote
      ? `/program-part?revision=${String(revision ?? 0)}&program=${encodeURIComponent(file)}`
      : programPartUrl(file)
    const response = await fetcher(url)
    if (!response.ok) return
    bytes = await response.arrayBuffer()
  } catch { return }
  // Only for the program it was asked for, and never over a model the operator attached since.
  if (!live.value || (remote && panel.hello !== hello) ||
    panel.status?.program.file !== file || panel.status.program.revision !== revision ||
    part.attachment !== null || part.loading) return
  const base = file.slice(file.lastIndexOf('/') + 1).replace(/\.[^.]*$/, '')
  await importPart(new File([bytes], `${base}.step`), withProgram) // i18n-ignore
}

export async function importPart(file: File, note = ''): Promise<void> {
  cancelImport()
  part.error = ''
  const status = panel.status
  if (!live.value || !status?.program.file) { part.error = t('program.step.loadProgram'); return }
  if (!/\.(step|stp)$/i.test(file.name) || file.size === 0 || file.size > maxStepBytes) {
    part.error = t('program.step.badFile'); return
  }
  const binding: ModelBinding = { file: status.program.file, revision: status.program.revision, frame: frameOf(status.offsets) }
  const request = generation
  importing = note
  part.loading = true
  timer = setTimeout(() => { cancelImport(); part.error = t('program.step.timeout') }, importTimeoutMs)
  try {
    const bytes = await file.arrayBuffer()
    if (request !== generation) return
    worker = new Worker(new URL('./step.worker.ts', import.meta.url), { type: 'module' })
    worker.onmessage = (event: MessageEvent<ImportReply>): void => {
      if (request !== generation) return
      const reply = event.data
      cancelImport()
      if (!reply.ok) { part.error = reply.error; return }
      part.attachment = markRaw({ name: file.name, note, binding, geometry: reply.geometry })
      part.placement = { translation: [0, 0, 0], rotation: [0, 0, 0] }
      part.visible = true
      part.placed = false
    }
    worker.onerror = (): void => {
      if (request !== generation) return
      cancelImport()
      part.error = t('program.step.importerFailed')
    }
    worker.postMessage(bytes, [bytes])
  } catch (error) {
    if (request !== generation) return
    cancelImport()
    part.error = error instanceof Error ? error.message : t('program.step.unreadable')
  }
}

export function placePart(placement: Placement): boolean {
  const status = panel.status
  const attachment = part.attachment
  if (!live.value || status === null || attachment === null) return false
  if (status.program.file !== attachment.binding.file || status.program.revision !== attachment.binding.revision) return false
  if (![...placement.translation, ...placement.rotation].every(Number.isFinite) || placement.translation.some((n) => Math.abs(n) > 1e6) || placement.rotation.some((n) => Math.abs(n) > 360)) return false
  part.placement = placement
  part.attachment = markRaw({ ...attachment, binding: { ...attachment.binding, frame: frameOf(status.offsets) } })
  part.placed = true
  return true
}

if (import.meta.hot) import.meta.hot.dispose(cancelImport)
