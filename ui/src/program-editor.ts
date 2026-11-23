// The program editor's model: what it may edit, its undo history and its search. The view is
// ProgramEditor.vue; saving is `save_program`.

import { ref } from 'vue'

import { t } from './i18n'
import type { ProgramText } from './protocol'

/** The longest program text `save_program` writes (the backend's max_saved_program_bytes). */
export const maxProgramBytes = 512 * 1024

/** What the editor is opened for: the loaded program, or a new one in `folder`. */
export type EditorTarget = { kind: 'loaded' } | { kind: 'new'; folder: string; taken: readonly string[] }

/** The editor to open, if any; App.vue shows it. */
export const editorTarget = ref<EditorTarget | null>(null)

/** The program's text as the editor holds it: lines joined by `\n`, with a final line end. */
export function programSource(lines: readonly string[]): string {
  return lines.length === 0 ? '' : `${lines.join('\n')}\n`
}

export function byteLength(text: string): number {
  return new TextEncoder().encode(text).length
}

/** Why this program cannot be edited here, or null. */
export function uneditableReason(program: ProgramText): string | null {
  if (program.truncated === true) return t('program.editorModel.truncated')
  if (program.lines.some((line) => line.includes('�')))
    return t('program.editorModel.notUtf8')
  if (byteLength(programSource(program.lines)) > maxProgramBytes) return t('program.editorModel.tooLarge')
  return null
}

export interface EditorState {
  text: string
  start: number
  end: number
}

/**
 * Undo and redo of whole texts. Typing in one place within `mergeMs` is one step. The oldest
 * steps go first once the history holds more than `maxSteps` or `maxCharacters`.
 */
export class EditHistory {
  private undoStack: EditorState[] = []
  private redoStack: EditorState[] = []
  private lastEdit = -Infinity
  private lastKind = ''

  constructor(private readonly maxSteps = 200, private readonly maxCharacters = 16 * 1024 * 1024, private readonly mergeMs = 1000) {}

  /** Records `before`, the state ahead of an edit of `kind` made at `now`. */
  record(before: EditorState, kind: string, now: number): void {
    const typing = kind === 'insertText' || kind === 'deleteContentBackward' || kind === 'deleteContentForward'
    const merge = typing && kind === this.lastKind && now - this.lastEdit < this.mergeMs && this.undoStack.length > 0
    if (!merge) {
      this.undoStack.push(before)
      let characters = this.undoStack.reduce((sum, state) => sum + state.text.length, 0)
      while (this.undoStack.length > this.maxSteps || (characters > this.maxCharacters && this.undoStack.length > 1)) {
        characters -= this.undoStack.shift()?.text.length ?? 0
      }
    }
    this.redoStack = []
    this.lastEdit = now
    this.lastKind = kind
  }

  get canUndo(): boolean { return this.undoStack.length > 0 }
  get canRedo(): boolean { return this.redoStack.length > 0 }

  undo(current: EditorState): EditorState | null {
    const state = this.undoStack.pop()
    if (state === undefined) return null
    this.redoStack.push(current)
    this.lastKind = ''
    return state
  }

  redo(current: EditorState): EditorState | null {
    const state = this.redoStack.pop()
    if (state === undefined) return null
    this.undoStack.push(current)
    this.lastKind = ''
    return state
  }
}

/** Where `query` occurs in `text`, ignoring case as G-code does: start offsets. At most `limit`. */
export function findAll(text: string, query: string, limit = 10_000): number[] {
  if (query === '') return []
  const haystack = text.toLowerCase()
  const needle = query.toLowerCase()
  const found: number[] = []
  for (let at = haystack.indexOf(needle); at >= 0 && found.length < limit; at = haystack.indexOf(needle, at + needle.length)) {
    found.push(at)
  }
  return found
}

/** The 1-based line of character `offset`. */
export function lineOf(text: string, offset: number): number {
  let line = 1
  for (let at = text.indexOf('\n'); at >= 0 && at < offset; at = text.indexOf('\n', at + 1)) line += 1
  return line
}

/** The offset where 1-based `line` starts; the end of the text past the last line. */
export function lineStart(text: string, line: number): number {
  let at = 0
  for (let n = 1; n < line; n += 1) {
    const end = text.indexOf('\n', at)
    if (end < 0) return text.length
    at = end + 1
  }
  return at
}
