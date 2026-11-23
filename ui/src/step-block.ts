// Why an attached STEP part is hidden. Apart from step-model.ts so that the STEP import worker,
// which uses step-model.ts, does not bundle the panel text.
import { t } from './i18n'
import type { Preview, Status } from './protocol'
import { sameFrame, type ModelBinding } from './step-model'

/** Why the part is hidden, in the panel's language, or ''. */
export function modelBlock(binding: ModelBinding, status: Status | null, preview: Preview | null, live: boolean): string {
  if (!live || status === null) return t('program.stepModel.noState')
  if (binding.file !== status.program.file || binding.revision !== status.program.revision) return t('program.stepModel.programChanged')
  if (!sameFrame(binding.frame, status.offsets)) return t('program.stepModel.originChanged')
  if (preview?.context !== status.preview_context || preview.state === 'pending' || preview.state === 'unavailable') {
    return t('program.stepModel.waitingToolpath')
  }
  return ''
}
