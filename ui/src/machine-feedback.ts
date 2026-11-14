import type { Status } from './protocol'
import type { LinkView } from './store'

export interface FeedbackReading {
  text: string
  known: boolean
  stale: boolean
}

/** Feedback comes only from reported inputs; a spindle command is never a measurement. */
export function machineFeedback(status: Status | null, link: LinkView): {
  rpm: FeedbackReading
  atSpeed: FeedbackReading
  probe: FeedbackReading
  probeTripped: boolean
} {
  const readable = status !== null && status.link !== 'down' &&
    (link === 'live' || link === 'stale' || link === 'silent')
  const stale = readable && (link !== 'live' || status.link !== 'live')
  const reading = (value: string | null): FeedbackReading => ({
    text: value === null ? 'Unavailable' : `${value}${stale ? ' · stale' : ''}`,
    known: value !== null,
    stale: value !== null && stale,
  })
  const rpm = readable ? status.spindle.actual_rpm : null
  const atSpeed = readable ? status.spindle.at_speed : null
  const probe = readable ? status.probe_input : null
  return {
    rpm: reading(typeof rpm === 'number' && Number.isFinite(rpm) && rpm >= 0 ? `${String(Math.round(rpm))} rpm` : null),
    atSpeed: reading(typeof atSpeed === 'boolean' ? atSpeed ? 'Yes' : 'No' : null),
    probe: reading(typeof probe === 'boolean' ? probe ? 'Tripped' : 'Clear' : null),
    probeTripped: probe === true && !stale,
  }
}
