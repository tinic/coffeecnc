// Every area of one language, merged. Each area file has its area as its only top-level key.
import common from './common/zh.json'
import status from './status/zh.json'
import manual from './manual/zh.json'
import program from './program/zh.json'
import tools from './tools/zh.json'
import probe from './probe/zh.json'
import operations from './operations/zh.json'
import messages from './messages/zh.json'
import backend from './backend/zh.json'

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
