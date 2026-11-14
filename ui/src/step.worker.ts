/// <reference lib="webworker" />
import createOcct from 'occt-import-js'
import wasmUrl from 'occt-import-js/dist/occt-import-js.wasm?url'
import { maxStepBytes, normalizeModel, type ImportReply } from './step-model'

const scope = self as unknown as DedicatedWorkerGlobalScope
scope.onmessage = (event: MessageEvent<ArrayBuffer>): void => { void read(event.data) }

async function read(bytes: ArrayBuffer): Promise<void> {
  try {
    if (bytes.byteLength === 0 || bytes.byteLength > maxStepBytes) throw new Error('Choose a STEP file up to 25 MB')
    const occt = await createOcct({ locateFile: () => wasmUrl })
    const geometry = normalizeModel(occt.ReadStepFile(new Uint8Array(bytes), {
      linearUnit: 'millimeter', linearDeflectionType: 'absolute_value', linearDeflection: 0.05, angularDeflection: 0.5,
    }))
    const transfers: ArrayBuffer[] = []
    for (const mesh of geometry.meshes) {
      transfers.push(mesh.positions.buffer, mesh.indices.buffer)
      if (mesh.normals !== null) transfers.push(mesh.normals.buffer)
    }
    scope.postMessage({ ok: true, geometry } satisfies ImportReply, transfers)
  } catch (error) {
    scope.postMessage({ ok: false, error: error instanceof Error ? error.message : 'STEP import failed' } satisfies ImportReply)
  }
}
