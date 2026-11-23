// Every area of one language, merged. Each area file has its area as its only top-level key.
import common from './common/pt.json'
import status from './status/pt.json'
import manual from './manual/pt.json'
import program from './program/pt.json'
import tools from './tools/pt.json'
import probe from './probe/pt.json'
import operations from './operations/pt.json'
import messages from './messages/pt.json'
import backend from './backend/pt.json'

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
