import type { FixedSetterLimits, Hello, Tool, Vec3 } from '../protocol'
import { toolIdentity, type ToolModelAssignment } from '../tool-model'

export interface ProbeBlock { centre: [number, number]; half: [number, number]; angle: number }

export const demoProbe = 99
export const demoSetter: FixedSetterLimits = {
  reference_tool: 98, travel_mm: 30, feed_mm_min: 900, latch_feed_mm_min: 300, latch_backoff_mm: 1,
  xy_tolerance_mm: 0.5, clearance_mm: 12, material_travel_mm: 35,
  check_tolerance_mm: 0.1,
  material_features: true,
  circle_check: true,
}
export const demoProbeTools: readonly Tool[] = [
  { number: 98, pocket: 98, length: 52, diameter: 6, comment: 'Setter reference' },
  { number: 99, pocket: 99, length: 48, diameter: 4, comment: 'Ruby touch probe' },
]

export function hasDemoProbeFixture(simulated: boolean, hello: Hello | null): boolean {
  return simulated && hello?.touch_probe_tool === demoProbe && hello.fixed_setter?.reference_tool === demoSetter.reference_tool
}

interface Fixture {
  stock: ProbeBlock
  slanted: ProbeBlock
  gauge: ProbeBlock
  pocket: ProbeBlock
  slot: ProbeBlock
  ridge: ProbeBlock
  bore: [number, number]
  boss: [number, number]
  setter: Vec3
  top: number
  stockHeight: number
  recessDepth: number
  boreRadius: number
  bossRadius: number
  setterHeight: number
  setterRadius: number
}
export const demoFixture: Fixture = {
  stock: { centre: [150, 100], half: [40, 30], angle: 0 },
  slanted: { centre: [255, 85], half: [40, 30], angle: 3 },
  gauge: { centre: [285, 190], half: [12, 15], angle: 0 },
  pocket: { centre: [172, 85], half: [12, 9], angle: 0 },
  slot: { centre: [125, 110], half: [6, 14], angle: 0 },
  ridge: { centre: [343, 146], half: [8, 16], angle: 0 },
  bore: [155, 108], boss: [235, 170], setter: [55, 55, -150],
  top: -120, stockHeight: 18, recessDepth: 12, boreRadius: 10, bossRadius: 12,
  setterHeight: 12, setterRadius: 10,
}

export function probeFixtureBounds(): { min: Vec3; max: Vec3 } {
  const { stock, slanted, gauge, ridge, boss, bossRadius, setter, setterRadius, setterHeight, top } = demoFixture
  const skew = slanted.angle * Math.PI / 180
  const skewX = Math.cos(skew) * slanted.half[0] + Math.sin(skew) * slanted.half[1]
  const skewY = Math.cos(skew) * slanted.half[1] + Math.sin(skew) * slanted.half[0]
  return {
    min: [Math.min(stock.centre[0] - stock.half[0], setter[0] - setterRadius),
      Math.min(slanted.centre[1] - skewY, setter[1] - setterRadius), setter[2] - setterHeight],
    max: [Math.max(slanted.centre[0] + skewX, gauge.centre[0] + gauge.half[0],
      ridge.centre[0] + ridge.half[0], boss[0] + bossRadius),
      Math.max(gauge.centre[1] + gauge.half[1], boss[1] + bossRadius), top],
  }
}

const toolShapes: Record<number, Omit<ToolModelAssignment, 'identity'>> = {
  1: { family: 'end-mill', overrides: { cuttingLength: 20, shankDiameter: 10, overallLength: 62.5, stickout: 44 } },
  2: { family: 'end-mill', overrides: { cuttingLength: 14, shankDiameter: 6, overallLength: 48.2, stickout: 30 } },
  3: { family: 'drill', overrides: { cuttingLength: 41, shankDiameter: 5, overallLength: 71, stickout: 55 } },
  4: { family: 'spot-drill', overrides: { cuttingLength: 8, shankDiameter: 8, overallLength: 40.35, stickout: 24 } },
  98: { family: 'generic', overrides: { cuttingLength: 18, shankDiameter: 6, overallLength: 52, stickout: 36 } },
  99: { family: 'touch-probe', overrides: { bodyDiameter: 21, bodyLength: 18, stylusLength: 16, shankDiameter: 10 } },
}
export function demoToolAssignment(tool: Tool): ToolModelAssignment | undefined {
  const shape = toolShapes[tool.number]
  return shape === undefined ? undefined : { ...shape, identity: toolIdentity(tool) }
}
