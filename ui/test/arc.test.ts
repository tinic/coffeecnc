import { describe, expect, it } from 'vitest'
import { arcAnglesValid, arcCentreUncertainty, arcSpread, bossDiameterPlausible, circleThrough, contactSigma, measuredSpan } from '../src/arc'

// The same geometry as the controller's (backend/tests/core_tests.cpp arc_geometry).
describe('three-point circle', () => {
  const onCircle = (degrees: number): [number, number] =>
    [4 + 7 * Math.cos(degrees * Math.PI / 180), -2 + 7 * Math.sin(degrees * Math.PI / 180)]

  it.each([[0, 120, 240], [-80, -40, 0], [200, 240, 280], [10, 25, 40]] as const)(
    'finds the centre and radius through %s°, %s°, %s°', (a, b, c) => {
      const points = [onCircle(a), onCircle(b), onCircle(c)] as const
      const circle = circleThrough(points)
      expect(circle?.centre[0]).toBeCloseTo(4, 9)
      expect(circle?.centre[1]).toBeCloseTo(-2, 9)
      expect(circle?.radius).toBeCloseTo(7, 9)
      expect(measuredSpan(points, circle?.centre ?? [0, 0])).toBeCloseTo(arcSpread([a, b, c]).span, 9)
    })

  it('has no circle through points on a line', () => {
    expect(circleThrough([[0, 0], [1, 1], [2, 2]])).toBeNull()
    expect(circleThrough([[0, 0], [1, 0], [2, 1e-9]])).toBeNull()
  })

  it('compares directions around the full turn', () => {
    expect(arcSpread([350, 365, 10])).toEqual({ closest: 5, span: 20 })
    expect(arcSpread([0, 120, 240])).toEqual({ closest: 120, span: 240 })
    expect(arcSpread([-80, -40, 0])).toEqual({ closest: 40, span: 80 })
    const sets: [number, number, number][] = [[0, 120, 240], [-80, -40, 0], [0, 5, 90], [0, 12, 24], [0, 10, 30], [-45, 0, 45], [-44, 0, 44]]
    expect(sets.map(arcAnglesValid)).toEqual([true, false, false, false, false, true, false])
  })

  // The controller's figures (core_tests.cpp): an independent Monte Carlo with 2.5 µm radial noise on r = 8.
  it.each([[20, 0.40], [30, 0.18], [40, 0.10], [80, 0.026], [240, 0.0057]] as const)(
    'estimates the centre uncertainty over a %s° arc as about %s mm', (span, expected) => {
      const at = (degrees: number): [number, number] => [8 * Math.cos(degrees * Math.PI / 180), 8 * Math.sin(degrees * Math.PI / 180)]
      const found = arcCentreUncertainty([at(-span / 2), at(0), at(span / 2)], [0, 0], [0.0025, 0.0025, 0.0025])
      expect(Math.abs((found ?? 0) - expected)).toBeLessThanOrEqual(0.12 * expected)
    })

  it('takes a contact as no better than 1 µm and checks a boss against its entered diameter', () => {
    expect(contactSigma(0)).toBe(0.001)
    expect(contactSigma(0.006)).toBeCloseTo(0.006 / 1.693, 9)
    expect([bossDiameterPlausible(12.4, 12), bossDiameterPlausible(13.3, 12), bossDiameterPlausible(3.6, 3)]).toEqual([true, false, false])
  })
})
