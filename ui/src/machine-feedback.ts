import { t } from './i18n'
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
    text: value === null ? t('common.unavailable') : stale ? t('status.feedback.staleValue', { value, stale: t('common.stale') }) : value,
    known: value !== null,
    stale: value !== null && stale,
  })
  const rpm = readable ? status.spindle.actual_rpm : null
  const atSpeed = readable ? status.spindle.at_speed : null
  const probe = readable ? status.probe_input : null
  return {
    rpm: reading(typeof rpm === 'number' && Number.isFinite(rpm) && rpm >= 0 ? `${String(Math.round(rpm))} rpm` : null), // i18n-ignore
    atSpeed: reading(readable && status.spindle.direction === 'stopped' ? '—' : typeof atSpeed === 'boolean' ? t(atSpeed ? 'common.yes' : 'common.no') : null),
    probe: reading(typeof probe === 'boolean' ? t(probe ? 'status.feedback.tripped' : 'status.feedback.clear') : null),
    probeTripped: probe === true && !stale,
  }
}
