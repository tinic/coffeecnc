// Every area of one language, merged. Each area file has its area as its only top-level key.
import common from './common/fr.json'
import status from './status/fr.json'
import manual from './manual/fr.json'
import program from './program/fr.json'
import tools from './tools/fr.json'
import probe from './probe/fr.json'
import operations from './operations/fr.json'
import messages from './messages/fr.json'
import backend from './backend/fr.json'

export default {
  ...common,
  ...status,
  ...manual,
  ...program,
  ...tools,
  ...probe,
  ...operations,
  ...messages,
  ...backend,
}
