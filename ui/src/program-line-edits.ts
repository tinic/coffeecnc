import { computed, shallowRef, watch, type ComputedRef, type ShallowRef } from 'vue'

import { t } from './i18n'
import { translateMessage } from './i18n/messages'
import { connectionBlock } from './machine-block'
import { byteLength, machineBusy, maxProgramBytes, programSource, uneditableReason } from './program-editor'
import type { CommandBody, ProgramText } from './protocol'
import type { PanelState, ResultListener } from './store'

export type LineAction = 'delete' | 'cut' | 'copy' | 'insert-above' | 'insert-below'

interface UndoEdit {
  file: string
  oldRevision: number
  before: string
  after: string
  saved: { revision: number; stamp: string } | null
}

interface LineEditConnection {
  send: (request: CommandBody) => number
  onResult: (listener: ResultListener) => () => void
}

interface ProgramLineEdits {
  block: ComputedRef<string | null>
  pending: ShallowRef<boolean>
  canUndo: ComputedRef<boolean>
  message: ComputedRef<{ text: string; error: boolean } | null>
  hasClipboard: ComputedRef<boolean>
  apply: (action: LineAction, first: number, last: number) => void
  undoLast: () => void
  dismiss: () => void
}

/** One clipboard and one undo per panel. Switching workspaces keeps them in memory. */
export function createProgramLineEdits(panel: Pick<PanelState, 'status' | 'program' | 'link'>, connection: LineEditConnection): ProgramLineEdits {
  const clipboard = shallowRef<readonly string[]>([])
  const undo = shallowRef<UndoEdit | null>(null)
  const pending = shallowRef(false)
  const feedback = shallowRef<{ file: string; text: string | (() => string); error: boolean } | null>(null)
  const current = computed(() => {
    const program = panel.program
    const loaded = panel.status?.program
    return program && program.file === loaded?.file && program.revision === loaded.revision ? program : null
  })
  const fileBlock = computed(() => {
    const program = current.value
    return program === null ? t('program.lineEdits.waitText') : !program.stamp ? t('program.lineEdits.reopen') : uneditableReason(program)
  })
  const block = computed(() => {
    return connectionBlock(panel.link) ?? fileBlock.value ??
      (machineBusy(panel.status) ? t('program.lineEdits.stopFirst') : null) ??
      (pending.value || undo.value?.saved === null ? t('program.lineEdits.reopening') : null)
  })
  const canUndo = computed(() => Boolean(undo.value?.saved))
  const message = computed(() => {
    const entry = feedback.value
    if (entry === null || entry.file !== panel.status?.program.file) return null
    return { text: typeof entry.text === 'function' ? entry.text() : entry.text, error: entry.error }
  })

  function dismiss(): void {
    feedback.value = null
    if (undo.value?.saved) undo.value = null
  }

  function reconcileUndo(): void {
    const edit = undo.value
    const program = current.value
    if (edit === null) return
    const loaded = panel.status?.program.file
    if ((loaded && loaded !== edit.file) || (!loaded && edit.saved !== null)) {
      undo.value = null
    } else if (program !== null && program.revision !== edit.oldRevision) {
      if (edit.saved === null && programSource(program.lines) === edit.after) {
        undo.value = { ...edit, saved: { revision: program.revision, stamp: program.stamp } }
      } else if (program.revision !== edit.saved?.revision || program.stamp !== edit.saved.stamp) {
        undo.value = null
      }
    }
  }
  watch([current, (): string | null | undefined => panel.status?.program.file], reconcileUndo)

  function save(program: ProgramText, text: string, success: string, nextUndo: UndoEdit | null, cut?: string[]): void {
    if (text.trim() === '' || byteLength(text) > maxProgramBytes) {
      feedback.value = { file: program.file, text: text.trim() === '' ? t('program.lineEdits.wouldBeEmpty') : t('program.lineEdits.wouldExceed'), error: true }
      return
    }
    const command: CommandBody = { type: 'save_program', path: program.file, stamp: program.stamp, text }
    pending.value = true
    feedback.value = null
    const unsubscribe = connection.onResult((sent, result): void => {
      if (sent !== command) return
      unsubscribe()
      pending.value = false
      if (result.outcome !== 'accepted') {
        feedback.value = { file: program.file, text: (): string => translateMessage(result.reason, result.refusal) || t('program.lineEdits.notSaved'), error: true }
        return
      }
      if (cut !== undefined) clipboard.value = cut
      undo.value = nextUndo
      reconcileUndo()
      feedback.value = { file: program.file, text: success, error: false }
    })
    connection.send(command)
  }

  function apply(action: LineAction, first: number, last: number): void {
    const program = current.value
    if (block.value !== null || program === null ||
      !Number.isSafeInteger(first) || !Number.isSafeInteger(last) || first < 1 || last < first || last > program.lines.length) return
    const selected = program.lines.slice(first - 1, last)
    const single = first === last
    if (action === 'copy') {
      clipboard.value = selected
      feedback.value = { file: program.file, text: single ? t('program.lineEdits.lineCopied', { line: first })
        : t('program.lineEdits.linesCopied', { first, last }), error: false }
      return
    }
    let lines: readonly string[]
    const inserting = action === 'insert-above' || action === 'insert-below'
    if (inserting) {
      if (first !== last || clipboard.value.length === 0) return
      const index = action === 'insert-above' ? first - 1 : last
      lines = [...program.lines.slice(0, index), ...clipboard.value, ...program.lines.slice(index)]
    } else {
      lines = [...program.lines.slice(0, first - 1), ...program.lines.slice(last)]
    }
    const before = programSource(program.lines)
    const after = programSource(lines)
    const success = inserting ? t('program.lineEdits.inserted')
      : action === 'cut' ? single ? t('program.lineEdits.lineCut', { line: first }) : t('program.lineEdits.linesCut', { first, last })
      : single ? t('program.lineEdits.lineDeleted', { line: first }) : t('program.lineEdits.linesDeleted', { first, last })
    save(program, after, success, { file: program.file, oldRevision: program.revision, before, after, saved: null },
      action === 'cut' ? selected : undefined)
  }

  function undoLast(): void {
    const program = current.value
    const edit = undo.value
    if (block.value !== null || program === null || !edit?.saved ||
      program.file !== edit.file || program.revision !== edit.saved.revision || program.stamp !== edit.saved.stamp) return
    save(program, edit.before, t('program.lineEdits.undone'), null)
  }

  return {
    block, pending, canUndo, message, apply, undoLast, dismiss,
    hasClipboard: computed(() => clipboard.value.length !== 0),
  }
}
