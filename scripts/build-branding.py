#!/usr/bin/env python3
"""Export CoffeeCNC's mark and outlined wordmark. See the branding README for setup."""
from pathlib import Path
from xml.etree import ElementTree as ET

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'ui/public/branding'
SVG = '{http://www.w3.org/2000/svg}'
BLUE = '#377ef2'


def document(name: str, viewbox: str, body: str) -> None:
    (DEST / name).write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="{viewbox}" role="img" aria-labelledby="title">\n'
        '  <title id="title">CoffeeCNC</title>\n'
        f'{body}\n</svg>\n'
    )


def main() -> None:
    DEST.mkdir(parents=True, exist_ok=True)
    source = ET.parse(ROOT / 'docs/design/branding/coffeecnc-mark-source.svg').getroot()
    mark = '\n'.join(f'    <path d="{node.attrib["d"]}"/>' for node in source.findall(f'{SVG}path'))
    font = instantiateVariableFont(TTFont(ROOT / 'ui/public/fonts/DM-Sans-400-700.woff2'), {'wght': 700})
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    scale = 42 / font['head'].unitsPerEm
    pen = SVGPathPen(glyphs, ntos=lambda value: f'{value:.3f}'.rstrip('0').rstrip('.'))
    cursor = 58.0
    # The wordmark has optical tracking of -0.7 px at its 42 px master size.
    for character in 'CoffeeCNC':
        glyph = glyphs[cmap[ord(character)]]
        glyph.draw(TransformPen(pen, (scale, 0, 0, -scale, cursor, 47)))
        cursor += glyph.width * scale - .7
    width = round(cursor + 3)
    wordmark = pen.getCommands()
    for name, symbol, text in (
        ('light', BLUE, '#20272c'),
        ('dark', BLUE, '#edf0f2'),
        ('mono', 'currentColor', 'currentColor'),
    ):
        document(f'coffeecnc-logo-{name}.svg', f'0 0 {width} 64',
                 f'  <g fill="{symbol}">\n{mark}\n  </g>\n'
                 f'  <path fill="{text}" d="{wordmark}"/>')
    document('coffeecnc-mark.svg', '0 0 48 64', f'  <g fill="{BLUE}">\n{mark}\n  </g>')
    document('coffeecnc-icon.svg', '0 0 64 64',
             f'  <rect width="64" height="64" rx="14" fill="{BLUE}"/>\n'
             f'  <g fill="#fff" transform="translate(8 6.4) scale(1 .8)">\n{mark}\n  </g>')
    print(f'Exported five SVGs to {DEST}; wordmark viewBox 0 0 {width} 64')


if __name__ == '__main__':
    main()
