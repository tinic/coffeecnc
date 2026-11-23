// Every area of one language, merged. Each area file has its area as its only top-level key.
import common from './common/en.json'
import status from './status/en.json'
import manual from './manual/en.json'
import program from './program/en.json'
import tools from './tools/en.json'
import probe from './probe/en.json'
import operations from './operations/en.json'
import messages from './messages/en.json'
import backend from './backend/en.json'

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
