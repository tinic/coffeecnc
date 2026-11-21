"""Writes the STEP model beside each air program: the part the program would cut if it ran at
depth. Coordinates are G54 millimetres with Z0 on the stock top, so the air path (Z+5 and up)
floats just above the model. The panel loads <program>.step with <program>.ngc.

    python3 -m pip install cadquery && python3 parts.py
"""
import math
from pathlib import Path

import cadquery as cq

HERE = Path(__file__).resolve().parent
TOOL = 6.35          # T1, the 1/4 in end mill the programs are written for
DEPTH = 3.0          # how deep the features go below the stock top
STOCK = 12.0         # stock thickness under the top


def stock(x: float, y: float) -> cq.Workplane:
    """A block centred on G54 X0 Y0 with its top at Z0."""
    return cq.Workplane("XY").box(x, y, STOCK, centered=(True, True, False)).translate((0, 0, -STOCK))


def floor(x: float, y: float) -> cq.Workplane:
    """Stock whose top is DEPTH below Z0, for parts whose features stand up to Z0."""
    return stock(x, y).translate((0, 0, -DEPTH))


def boss(sketch: cq.Workplane) -> cq.Workplane:
    return sketch.extrude(DEPTH).translate((0, 0, -DEPTH))


def slots(segments: list[tuple[float, float, float, float]]) -> cq.Workplane:
    """Grooves of the tool's width along straight XY segments, DEPTH deep."""
    cut = None
    for x0, y0, x1, y1 in segments:
        length = ((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5
        angle = math.degrees(math.atan2(y1 - y0, x1 - x0))
        slot = (cq.Workplane("XY").slot2D(length + TOOL, TOOL, angle)
                .extrude(DEPTH).translate(((x0 + x1) / 2, (y0 + y1) / 2, -DEPTH)))
        cut = slot if cut is None else cut.union(slot)
    return cut


def parts() -> dict[str, cq.Workplane]:
    r = TOOL / 2
    return {
        # Outside profile around the 40 mm square: a square boss.
        "01-square": floor(64, 64).union(boss(cq.Workplane("XY").rect(40 - TOOL, 40 - TOOL))),
        # Outside profile around the R15 circles: a round boss.
        "02-arcs": floor(64, 64).union(boss(cq.Workplane("XY").circle(15 - r))),
        # Raster over X±24 Y±20: a rectangular pocket with the tool's corner radius.
        "03-pocket": stock(72, 64).cut(
            cq.Workplane("XY").rect(48 + TOOL, 40 + TOOL).extrude(DEPTH)
            .edges("|Z").fillet(r - 0.01).translate((0, 0, -DEPTH))),
        # Nine holes on a 18 x 12 grid; the programs drill with a 5 mm drill.
        "04-drill": stock(56, 44).cut(
            cq.Workplane("XY").pushPoints([(x, y) for x in (-18, 0, 18) for y in (-12, 0, 12)])
            .circle(2.5).extrude(STOCK).translate((0, 0, -STOCK))),
        # The feed moves between the stops: three grooves.
        "05-pauses": stock(48, 48).cut(slots([(-15, -15, 15, -15), (15, 0, -15, 0), (-15, 0, -15, 15)])),
        # One groove per tool.
        "06-toolchange": stock(48, 40).cut(slots([(-16, -12, 16, -12), (-16, 12, 16, 12)])),
    }


if __name__ == "__main__":
    for name, part in parts().items():
        cq.exporters.export(part, str(HERE / f"{name}.step"))
        print(name)
