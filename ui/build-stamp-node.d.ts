// The few functions of Node that build-stamp.ts uses. The panel does not depend on
// @types/node; when it does, this file goes.

declare module 'node:crypto' {
  interface Hash {
    update(data: string | Uint8Array): Hash
    digest(encoding: 'hex'): string
  }
  export function createHash(algorithm: 'sha256'): Hash
}

declare module 'node:fs' {
  interface Stats {
    isDirectory(): boolean
    isFile(): boolean
  }
  export function existsSync(path: string): boolean
  export function readdirSync(path: string): string[]
  export function readFileSync(path: string): Uint8Array
  export function statSync(path: string): Stats
  export function writeFileSync(path: string, data: string): void
}

declare module 'node:path' {
  export const sep: string
  export function isAbsolute(path: string): boolean
  export function join(...parts: string[]): string
  export function relative(from: string, to: string): string
  export function resolve(...parts: string[]): string
}
