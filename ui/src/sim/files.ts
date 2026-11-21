// The simulator's program directory: original demo programs for the supported subset, and
// unmodified LinuxCNC samples that use codes outside it (they show the failed-preview path).

import type { Directory, DirectoryEntry } from '../protocol'

import twoTools from '../../../samples/upstream/linuxcnc/nc_files/gmoccapy_2_tools_with_cutter_radius_compensation.ngc?raw'
import gridprobe from '../../../samples/upstream/linuxcnc/nc_files/gridprobe.ngc?raw'
import holeCircle from '../../../samples/upstream/linuxcnc/nc_files/hole-circle.ngc?raw'
import drillPattern from './programs/drill-pattern-inch.ngc?raw'
import plate from './programs/plate-face-pocket.ngc?raw'
import slot from './programs/slot.ngc?raw'

export const programDirectory = '/home/sim/nc_files'
export const samplesDirectory = `${programDirectory}/linuxcnc-samples`

const modified = Date.UTC(2026, 10, 14, 9, 0, 0)

export function defaultFiles(): Map<string, string> {
  return new Map([
    [`${programDirectory}/plate-face-pocket.ngc`, plate],
    [`${programDirectory}/drill-pattern-inch.ngc`, drillPattern],
    [`${programDirectory}/slot.ngc`, slot],
    [`${samplesDirectory}/gmoccapy_2_tools_with_cutter_radius_compensation.ngc`, twoTools],
    [`${samplesDirectory}/gridprobe.ngc`, gridprobe],
    [`${samplesDirectory}/hole-circle.ngc`, holeCircle],
  ])
}

export function splitLines(text: string): string[] {
  const lines = text.split(/\r?\n/)
  if (lines.at(-1) === '') {
    lines.pop()
  }
  return lines
}

/**
 * Where `save_program` writes `path`: absolute below the program directory, or relative to it.
 * Folders must exist (the simulator has no empty ones); names follow the native rules.
 */
export function newProgramPath(files: ReadonlyMap<string, string>, path: string): string | { error: string } {
  if (path === '' || path.endsWith('/')) return { error: 'Name the program file.' }
  const absolute = path.startsWith('/') ? path : `${programDirectory}/${path}`
  const parts: string[] = []
  for (const part of absolute.split('/').slice(1)) {
    if (part === '..') parts.pop()
    else if (part !== '.' && part !== '') parts.push(part)
  }
  const name = parts.pop() ?? ''
  // eslint-disable-next-line no-control-regex
  if (name === '' || name.startsWith('.') || /[\x00-\x1f\x7f]/.test(name))
    return { error: 'Use a file name without control characters that does not start with a dot.' }
  const directory = `/${parts.join('/')}`
  if (directory !== programDirectory && !directory.startsWith(`${programDirectory}/`))
    return { error: 'New programs go in the program directory.' }
  if (directory !== programDirectory && listDirectory(files, directory) === null)
    return { error: 'Path is not available: No such file or directory' }
  const file = `${directory}/${name}`
  return file.length > 254 ? { error: 'Program path is longer than 254 bytes.' } : file
}

/** The listing of `path`, or null when it is not a directory below the program directory. */
export function listDirectory(files: ReadonlyMap<string, string>, path: string | null): Directory | null {
  const directory = (path ?? programDirectory).replace(/\/+$/, '')
  if (directory !== programDirectory && !directory.startsWith(`${programDirectory}/`)) {
    return null
  }
  const entries = new Map<string, DirectoryEntry>()
  for (const [file, text] of files) {
    if (!file.startsWith(`${directory}/`)) {
      continue
    }
    const rest = file.slice(directory.length + 1)
    const slash = rest.indexOf('/')
    if (slash < 0) {
      entries.set(rest, { name: rest, kind: 'file', bytes: new TextEncoder().encode(text).length, modified })
    } else {
      const name = rest.slice(0, slash)
      entries.set(name, { name, kind: 'directory', bytes: 0, modified })
    }
  }
  if (entries.size === 0 && directory !== programDirectory) {
    return null
  }
  const sorted = [...entries.values()].sort(
    (a, b) => Number(b.kind === 'directory') - Number(a.kind === 'directory') || a.name.localeCompare(b.name),
  )
  return {
    type: 'directory',
    path: directory,
    parent: directory === programDirectory ? null : directory.slice(0, directory.lastIndexOf('/')),
    entries: sorted,
  }
}
