#!/usr/bin/env node
// Checks that a built panel is the bundle of the sources beside it, from the stamp the build
// wrote (ui/build-stamp.ts, dist/build-stamp.json). No dependencies.
//
//   node check-panel-stamp.mjs <repository root> [<dist directory>]
//
// The dist directory is <repository root>/ui/dist unless given. Fails (exit code 1) when
//
//   - there is no stamp, or it is not one this checker reads
//   - a file the stamp lists as input is missing or does not hash to what the stamp says
//   - a file exists under ui/src or ui/public that the stamp does not list: a source that was
//     added after the build. No file there is exempt
//   - the files of the bundle do not hash to the stamp's assets_digest: a bundle of another
//     build, or one that was edited
//
// Output: `panel=match|mismatch inputs=<digest> assets=<digest> [detail=<first difference>]`
// on one line; with a mismatch the digests are those of the stamp, where there is one.
// Exit code 0 for a match, 1 for a mismatch, 2 for wrong usage.

import { createHash } from 'node:crypto'
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs'
import { join, relative, resolve, sep } from 'node:path'

const sha256 = (data) => createHash('sha256').update(data).digest('hex')

function filesBelow(directory) {
  if (!existsSync(directory)) return []
  const found = []
  const walk = (current) => {
    for (const name of readdirSync(current).sort()) {
      const path = join(current, name)
      if (statSync(path).isDirectory()) walk(path)
      else found.push(relative(directory, path).split(sep).join('/'))
    }
  }
  walk(directory)
  return found
}

function digestOf(entries) {
  const hash = createHash('sha256')
  for (const [path, digest] of [...entries].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))) {
    hash.update(`${path}\0${digest}\n`)
  }
  return hash.digest('hex')
}

function finish(match, stamp, detail) {
  const word = (text) => String(text).replace(/\s+/g, '_')
  console.log(
    `panel=${match ? 'match' : 'mismatch'} inputs=${stamp?.inputs_digest ?? 'none'} assets=${stamp?.assets_digest ?? 'none'}` +
      (detail ? ` detail=${word(detail)}` : ''),
  )
  process.exit(match ? 0 : 1)
}

const [rootArgument, distArgument] = process.argv.slice(2)
if (!rootArgument) {
  console.error('usage: check-panel-stamp.mjs <repository root> [<dist directory>]')
  process.exit(2)
}
const root = resolve(rootArgument)
const dist = resolve(distArgument ?? join(root, 'ui', 'dist'))

let stamp
try {
  stamp = JSON.parse(readFileSync(join(dist, 'build-stamp.json'), 'utf8'))
} catch {
  finish(false, undefined, `no build-stamp.json in ${dist}`)
}
if (stamp.version !== 1 || typeof stamp.inputs !== 'object' || stamp.inputs === null) {
  finish(false, undefined, 'the stamp is not version 1')
}

const hashed = []
for (const [path, expected] of Object.entries(stamp.inputs)) {
  const file = join(root, path)
  if (!existsSync(file)) finish(false, stamp, `input ${path} is missing`)
  const actual = sha256(readFileSync(file))
  if (actual !== expected) finish(false, stamp, `input ${path} has changed since the build`)
  hashed.push([path, actual])
}
if (digestOf(hashed) !== stamp.inputs_digest) finish(false, stamp, 'the inputs digest does not match the listed inputs')

for (const directory of ['ui/src', 'ui/public']) {
  for (const file of filesBelow(join(root, directory))) {
    const path = `${directory}/${file}`
    if (!(path in stamp.inputs)) finish(false, stamp, `${path} was not part of the build`)
  }
}

const assets = filesBelow(dist)
  .filter((file) => file !== 'build-stamp.json')
  .map((file) => [file, sha256(readFileSync(join(dist, file)))])
if (digestOf(assets) !== stamp.assets_digest) finish(false, stamp, 'the bundle is not the one the stamp was written for')

finish(true, stamp)
