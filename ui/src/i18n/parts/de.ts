// Every area of one language, merged. Each area file has its area as its only top-level key.
import common from './common/de.json'
import status from './status/de.json'
import manual from './manual/de.json'
import program from './program/de.json'
import tools from './tools/de.json'
import probe from './probe/de.json'
import operations from './operations/de.json'
import messages from './messages/de.json'
import backend from './backend/de.json'

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
