declare module 'occt-import-js' {
  interface OcctImporter {
    ReadStepFile(bytes: Uint8Array, params: {
      linearUnit: 'millimeter'
      linearDeflectionType: 'absolute_value'
      linearDeflection: number
      angularDeflection: number
    }): unknown
  }
  export default function createOcct(options: { locateFile: (path: string) => string }): Promise<OcctImporter>
}
