// Writes dist/build-stamp.json: what this bundle was built from and what it consists of, so
// that the native shell tests can refuse to run a bundle that is not the one of the sources
// beside it (backend/tests/sim/check-panel-stamp.mjs re-hashes everything listed here).
//
//   inputs          one SHA-256 per input file, by repository-relative path:
//                     - every file under ui/src (all of it is input of `vue-tsc && vite build`)
//                     - every file under ui/public (copied into the bundle as it is)
//                     - every other file of the repository the build loaded as a module
//                       (the STEP sample under samples/); files below node_modules are not
//                       listed: package-lock.json pins them
//                     - index.html, package.json, package-lock.json, vite.config.ts,
//                       tsconfig.json, this file and build-stamp-node.d.ts
//   inputs_digest   one SHA-256 over all of the above
//   assets_digest   one SHA-256 over every file of the bundle (this stamp aside): a reused or
//                   hand-edited dist does not match it

import { createHash } from 'node:crypto'
import { existsSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { isAbsolute, join, relative, resolve, sep } from 'node:path'

import type { Plugin } from 'vite'

const configuration = ['index.html', 'package.json', 'package-lock.json', 'vite.config.ts', 'tsconfig.json', 'build-stamp.ts', 'build-stamp-node.d.ts']

function sha256(data: string | Uint8Array): string {
  return createHash('sha256').update(data).digest('hex')
}

/** Every file below `directory`, as paths relative to it with `/`, sorted. */
function filesBelow(directory: string): string[] {
  if (!existsSync(directory)) return []
  const found: string[] = []
  const walk = (current: string): void => {
    for (const name of readdirSync(current).sort()) {
      const path = join(current, name)
      if (statSync(path).isDirectory()) walk(path)
      else found.push(relative(directory, path).split(sep).join('/'))
    }
  }
  walk(directory)
  return found
}

/** One digest over named digests, in path order. */
function digestOf(entries: readonly (readonly [string, string])[]): string {
  const hash = createHash('sha256')
  for (const [path, digest] of [...entries].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))) {
    hash.update(`${path}\0${digest}\n`)
  }
  return hash.digest('hex')
}

export function buildStamp(): Plugin {
  let root = ''
  let outDir = ''
  const loaded = new Set<string>()
  return {
    name: 'coffeecnc-build-stamp',
    apply: 'build',
    configResolved(config): void {
      root = config.root
      outDir = resolve(config.root, config.build.outDir)
    },
    buildEnd(): void {
      for (const id of this.getModuleIds()) {
        const file = id.split('?')[0] ?? ''
        if (isAbsolute(file) && existsSync(file) && statSync(file).isFile()) loaded.add(file)
      }
    },
    closeBundle(): void {
      const repository = resolve(root, '..')
      const inputs = new Set<string>(configuration.map((name) => resolve(root, name)))
      for (const directory of ['src', 'public']) {
        for (const file of filesBelow(resolve(root, directory))) inputs.add(resolve(root, directory, file))
      }
      for (const file of loaded) {
        const path = relative(repository, file)
        if (!path.startsWith('..') && !path.split(sep).includes('node_modules')) inputs.add(file)
      }
      const hashed = [...inputs].map((file) => [relative(repository, file).split(sep).join('/'), sha256(readFileSync(file))] as const)
      const assets = filesBelow(outDir)
        .filter((file) => file !== 'build-stamp.json')
        .map((file) => [file, sha256(readFileSync(join(outDir, file)))] as const)
      const stamp = {
        version: 1,
        inputs: Object.fromEntries([...hashed].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))),
        inputs_digest: digestOf(hashed),
        assets_digest: digestOf(assets),
      }
      writeFileSync(join(outDir, 'build-stamp.json'), `${JSON.stringify(stamp, null, 2)}\n`)
    },
  }
}
