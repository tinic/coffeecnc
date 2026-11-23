// Every area of one language, merged. Each area file has its area as its only top-level key.
import common from './common/es.json'
import status from './status/es.json'
import manual from './manual/es.json'
import program from './program/es.json'
import tools from './tools/es.json'
import probe from './probe/es.json'
import operations from './operations/es.json'
import messages from './messages/es.json'
import backend from './backend/es.json'

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
