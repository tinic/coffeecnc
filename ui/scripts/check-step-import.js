// Exercise the pinned, real WASM importer against our research corpus. This verifies
// display geometry and output units, not agreement between a STEP file and its NC job.
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
import { performance } from 'node:perf_hooks'

import createOcct from 'occt-import-js'

const require = createRequire(import.meta.url)
const wasmBinary = await readFile(require.resolve('occt-import-js/dist/occt-import-js.wasm'))
const occt = await createOcct({ wasmBinary })
const manifest = JSON.parse(await readFile(new URL('../../samples/manifest.json', import.meta.url), 'utf8'))
const samples = manifest.datasets.flatMap((dataset) => dataset.files.map((file) => file.local_path))
  .filter((path) => /\.(step|stp)$/i.test(path))
// Independently documented physical extents catch input-unit mistakes, not just output scaling.
const expectedSizes = {
  'ops_block.step': [100, 70, 20],
  'ops_2d.step': [140, 100, 20],
  'lego_4x2_01.step': [100.6, 49.8, 38.1],
}
const params = {
  linearUnit: 'millimeter',
  linearDeflectionType: 'absolute_value',
  linearDeflection: 0.05,
  angularDeflection: 0.5,
}

function inspect(result) {
  assert.equal(result.success, true, 'STEP transfer failed')
  assert.ok(result.meshes.length > 0, 'No display meshes')
  const bounds = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] }
  let vertices = 0
  let triangles = 0
  for (const mesh of result.meshes) {
    const positions = mesh.attributes.position.array
    const indices = mesh.index.array
    assert.ok(positions.length > 0 && positions.length % 3 === 0, 'Invalid vertex array')
    assert.ok(indices.length > 0 && indices.length % 3 === 0, 'Invalid triangle array')
    const count = positions.length / 3
    for (let i = 0; i < positions.length; i++) {
      assert.ok(Number.isFinite(positions[i]), 'Non-finite position')
      const axis = i % 3
      bounds.min[axis] = Math.min(bounds.min[axis], positions[i])
      bounds.max[axis] = Math.max(bounds.max[axis], positions[i])
    }
    for (const index of indices) {
      assert.ok(Number.isInteger(index) && index >= 0 && index < count, 'Invalid vertex index')
    }
    const normals = mesh.attributes.normal?.array
    if (normals !== undefined) {
      assert.equal(normals.length, positions.length, 'Normal/position count mismatch')
      assert.ok(normals.every(Number.isFinite), 'Non-finite normal')
    }
    vertices += count
    triangles += indices.length / 3
  }
  let references = 0
  function visit(node) {
    assert.equal(typeof node.name, 'string')
    for (const index of node.meshes) {
      assert.ok(Number.isInteger(index) && index >= 0 && index < result.meshes.length, 'Invalid mesh reference')
      references++
    }
    for (const child of node.children) visit(child)
  }
  visit(result.root)
  assert.ok(references > 0, 'Hierarchy does not reference a mesh')
  return { meshes: result.meshes.length, vertices, triangles, bounds }
}

for (const [index, file] of samples.entries()) {
  const bytes = await readFile(new URL(`../../${file}`, import.meta.url))
  const started = performance.now()
  const result = inspect(occt.ReadStepFile(bytes, params))
  console.log(JSON.stringify({ file, ...result, milliseconds: Math.round(performance.now() - started) }))
  const expected = expectedSizes[file.split('/').at(-1)]
  if (expected !== undefined) {
    for (let axis = 0; axis < 3; axis++) {
      assert.ok(Math.abs(result.bounds.max[axis] - result.bounds.min[axis] - expected[axis]) < 0.15,
        `Wrong physical size for ${file} axis ${axis}; check STEP input units`)
    }
  }

  // Exercise the importer's unit parameter with the same geometry. Scale the
  // tessellation tolerance too, so it describes the same physical distance.
  if (index === 0) {
    const inches = inspect(occt.ReadStepFile(bytes, {
      ...params, linearUnit: 'inch', linearDeflection: params.linearDeflection / 25.4,
    }))
    for (const side of ['min', 'max']) {
      for (let axis = 0; axis < 3; axis++) {
        assert.ok(Math.abs(inches.bounds[side][axis] * 25.4 - result.bounds[side][axis]) < 0.001,
          'Millimeter/inch output bounds disagree')
      }
    }
  }
}

assert.equal(occt.ReadStepFile(new TextEncoder().encode('not a STEP file'), params).success, false)
console.log(`${samples.length} STEP models: mesh integrity, cm/inch input units, output units and invalid-input checks passed.`)
