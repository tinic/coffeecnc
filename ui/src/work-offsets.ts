import type { Status, Vec3, WorkOffsets } from './protocol'
import type { LinkView } from './store'

export interface WorkOffsetRow {
  wcs: number
  offset: Vec3 | null
  rotation_xy: number | null
  active: boolean
  stale: boolean
}

function validOffset(value: unknown): value is Vec3 {
  return Array.isArray(value) && value.length === 3 && value.every(Number.isFinite)
}

/** Unavailable coordinates never become displayed zeros, including on a disconnected page. */
export function workOffsetRows(offsets: WorkOffsets | null, status: Status | null, link: LinkView): WorkOffsetRow[] {
  if (offsets === null) return []
  const readable = status !== null && status.link !== 'down' &&
    (link === 'live' || link === 'stale' || link === 'silent')
  const fresh = readable && link === 'live' && status.link === 'live'
  return Array.from({ length: 9 }, (_, index) => {
    const wcs = index + 1
    const matches = offsets.systems.filter((system) => system.wcs === wcs)
    const system = matches.length === 1 ? matches[0] : undefined
    const availability: unknown = system?.available
    const known = readable && system !== undefined &&
      (availability === undefined || availability === true) && validOffset(system.offset) &&
      Number.isFinite(system.rotation_xy)
    return {
      wcs,
      offset: known ? system.offset : null,
      rotation_xy: known ? system.rotation_xy : null,
      active: fresh && status.offsets.wcs === wcs,
      stale: known && !fresh,
    }
  })
}
