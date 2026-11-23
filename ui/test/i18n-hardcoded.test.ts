// Fails when operator-visible English is written into a component or panel module instead of a
// message catalog. It reads every .vue file and the panel modules: template text, the static
// attributes an operator sees or hears (title, aria-label, placeholder, ...), and string
// literals in template expressions and scripts that read as words. Technical text (units, axis
// letters, G/M words, CSS classes, protocol values) passes. A literal that must stay English,
// for example a protocol sentence compared with what the backend sent, takes a trailing
// `// i18n-ignore` comment on its line, or `<!-- i18n-ignore -->` before a template element.
import { describe, expect, it } from 'vitest'
import { parse as parseSfc } from 'vue/compiler-sfc'
import { NodeTypes, type ElementNode, type RootNode, type TemplateChildNode } from '@vue/compiler-core'
import ts from 'typescript'

const sources = import.meta.glob<string>(['../src/**/*.ts', '../src/**/*.vue'], { query: '?raw', import: 'default', eager: true })

/** Modules that are not operator text: protocol, simulator and G-code generators, the catalogs. */
const exempt = [
  /^i18n\//,
  /^sim\//,
  /^protocol\.ts$/,
  // The store logs English sentences; they are catalogued in parts/messages and translated
  // where they are shown (test/i18n-native.test.ts checks every one is known).
  /^store\.ts$/,
  /\.d\.ts$/,
  /\.worker\.ts$/,
]

const visibleAttributes = new Set(['title', 'aria-label', 'aria-description', 'placeholder', 'alt', 'label',
  'data-numeric-effect', 'data-numeric-label', 'data-numeric-title'])

/** Units, axis letters, G/M words, keys and similar technical tokens shown as they are. */
const technical = /^(?:[A-Z]{1,4}\d*(?:\.\d+)?|[GMTSFHDPQRIJKLN]\d+(?:\.\d+)?|mm|in|inch|rpm|RPM|mm\/min|in\/min|ipm|deg|°|%|[XYZABCUVW]|[XYZ]{2,3}|G5[4-9](?:\.[1-3])?|G9[0-4]|MDI|NC|STEP|WCS|OK|ID|E-stop|E-STOP|CoffeeCNC|LinuxCNC|HAL|INI|NML|DRO|CAM|USB)$/

function words(text: string): boolean {
  const trimmed = text.replace(/\s+/g, ' ').trim()
  if (!/\p{L}{2,}/u.test(trimmed)) return false
  // Every word technical: "mm", "G54 X", "X Y Z".
  return !trimmed.split(/[\s/,:;()·×+\-–—…]+/).filter(Boolean).every((word) => technical.test(word) || !/\p{L}/u.test(word))
}

/** A script literal reads as operator text: a capitalized word, or several words. */
function prose(text: string): boolean {
  if (!words(text)) return false
  if (/^[a-z][\w.-]*$/.test(text)) return false // identifiers, keys, classes
  if (/^[.#[]|^\w+[=:]|[{}<>]|^--|^\/|^https?:|^data:|^\w+\(/.test(text)) return false // selectors, CSS, paths, URLs
  if (/^[a-z]/.test(text) && !text.includes(' ')) return false
  return /^\p{Lu}\p{Ll}/u.test(text) || / \p{L}/u.test(text) || /^\p{Ll}+ \p{L}/u.test(text)
}

interface Finding { file: string; line: number; text: string }

function ignored(lines: string[], line: number): boolean {
  return (lines[line - 1] ?? '').includes('i18n-ignore') || (lines[line - 2] ?? '').includes('i18n-ignore')
}

function scriptFindings(file: string, code: string, lineOffset: number, all: string[]): Finding[] {
  const found: Finding[] = []
  const source = ts.createSourceFile(file, code, ts.ScriptTarget.Latest, true, ts.ScriptKind.TS)
  function literal(node: ts.Node, text: string): void {
    const parent = node.parent
    // Imports, type literals, property keys and console/error/developer messages are not operator text.
    if (ts.isImportDeclaration(parent) || ts.isExportDeclaration(parent) || ts.isLiteralTypeNode(parent) ||
        ts.isExternalModuleReference(parent)) return
    if ((ts.isPropertyAssignment(parent) || ts.isPropertyDeclaration(parent)) && parent.name === node) return
    if (ts.isNewExpression(parent) && parent.expression.getText().endsWith("Error")) return
    if (ts.isCallExpression(parent) && /^(?:console\.\w+|t|i18n\.global\.t|translateMessage|querySelector\w*|\w+\.querySelector\w*|matchMedia)$/.test(parent.expression.getText())) return
    if (!prose(text)) return
    const line = lineOffset + source.getLineAndCharacterOfPosition(node.getStart()).line + 1
    if (!ignored(all, line)) found.push({ file, line, text })
  }
  function visit(node: ts.Node): void {
    if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) literal(node, node.text)
    else if (ts.isTemplateExpression(node)) literal(node, [node.head.text, ...node.templateSpans.map((span) => span.literal.text)].join(' x '))
    ts.forEachChild(node, visit)
  }
  visit(source)
  return found
}

function templateFindings(file: string, root: RootNode, lineOffset: number, all: string[]): Finding[] {
  const found: Finding[] = []
  const at = (line: number): number => lineOffset + line
  function expression(code: string, line: number): void {
    for (const finding of scriptFindings(file, `(${code})`, at(line) - 1, all)) found.push(finding)
  }
  function children(nodes: TemplateChildNode[]): void {
    nodes.forEach((node, index) => {
      const before = nodes.slice(0, index).reverse().find((n) => n.type !== NodeTypes.TEXT || n.content.trim() !== '')
      if (before?.type === NodeTypes.COMMENT && before.content.includes('i18n-ignore')) return
      visit(node)
    })
  }
  function visit(node: TemplateChildNode): void {
    switch (node.type) {
      case NodeTypes.TEXT:
        if (words(node.content) && !ignored(all, at(node.loc.start.line))) found.push({ file, line: at(node.loc.start.line), text: node.content.trim() })
        return
      case NodeTypes.INTERPOLATION:
        if (node.content.type === NodeTypes.SIMPLE_EXPRESSION) expression(node.content.content, node.loc.start.line)
        return
      case NodeTypes.ELEMENT: {
        const element: ElementNode = node
        for (const prop of element.props) {
          if (prop.type === NodeTypes.ATTRIBUTE) {
            const component = /^[A-Z]/.test(element.tag)
            const value = prop.value?.content ?? ''
            if ((visibleAttributes.has(prop.name) || component && prop.name !== 'class' && prop.name !== 'id') &&
                (visibleAttributes.has(prop.name) ? words(value) : prose(value)) && !ignored(all, at(prop.loc.start.line)))
              found.push({ file, line: at(prop.loc.start.line), text: value })
          } else if (prop.exp?.type === NodeTypes.SIMPLE_EXPRESSION && prop.name !== 'for' && !(prop.name === 'bind' && prop.arg?.type === NodeTypes.SIMPLE_EXPRESSION && prop.arg.content === 'class')) {
            expression(prop.exp.content, prop.exp.loc.start.line)
          }
        }
        children(element.children)
        return
      }
      case NodeTypes.IF:
        for (const branch of node.branches) {
          const condition = branch.condition
          if (condition?.type === NodeTypes.SIMPLE_EXPRESSION) expression(condition.content, condition.loc.start.line)
          children(branch.children)
        }
        return
      case NodeTypes.FOR:
        children(node.children)
        return
      case NodeTypes.COMMENT:
      case NodeTypes.COMPOUND_EXPRESSION:
      case NodeTypes.IF_BRANCH:
      case NodeTypes.TEXT_CALL:
        return
    }
  }
  children(root.children)
  return found
}

export function hardCodedText(): Finding[] {
  const found: Finding[] = []
  for (const [path, text] of Object.entries(sources)) {
    const file = path.replace(/^\.\.\/src\//, '')
    if (exempt.some((pattern) => pattern.test(file))) continue
    const lines = text.split('\n')
    if (file.endsWith('.ts')) found.push(...scriptFindings(file, text, 0, lines))
    else if (file.endsWith('.vue')) {
      const { descriptor } = parseSfc(text, { filename: file })
      for (const block of [descriptor.script, descriptor.scriptSetup]) {
        if (block) found.push(...scriptFindings(file, block.content, block.loc.start.line - 1, lines))
      }
      if (descriptor.template?.ast) found.push(...templateFindings(file, descriptor.template.ast, 0, lines))
    }
  }
  return found
}

describe('operator text', () => {
  it('lives in the message catalogs, not in components or panel modules', () => {
    const found = hardCodedText().map((finding) => `${finding.file}:${String(finding.line)}: ${finding.text}`)
    expect(found).toEqual([])
  })
})
