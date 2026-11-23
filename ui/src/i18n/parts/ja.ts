// Every area of one language, merged. Each area file has its area as its only top-level key.
import common from './common/ja.json'
import status from './status/ja.json'
import manual from './manual/ja.json'
import program from './program/ja.json'
import tools from './tools/ja.json'
import probe from './probe/ja.json'
import operations from './operations/ja.json'
import messages from './messages/ja.json'
import backend from './backend/ja.json'

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
