import { describe, expect, it } from 'vitest'
import { probeGuide } from '../src/probe-guide'
import type { FixedSetterLimits } from '../src/protocol'

const limits: FixedSetterLimits = {
  reference_tool: 98, travel_mm: 15, material_travel_mm: 20, feed_mm_min: 240,
  xy_tolerance_mm: 0.5, latch_feed_mm_min: 45, latch_backoff_mm: 1,
}

describe('illustrated probe motion', () => {
  it('uses the configured corner reach in the jog instructions', () => {
    const tuned = { ...limits, material_geometry: { reach_mm: 8, margin_mm: 4, clearance_mm: 6, depth_mm: 3 } }
    expect(probeGuide({ kind: 'corner', x: 'x_plus', y: 'y_plus' }, tuned, 'y_plus', null).setup)
      .toContain('within 8 mm')
  })
  it('shows the initial contact, three slow latches, then retract and result', () => {
    const plan = probeGuide({ kind: 'surface', direction: 'z_top' }, limits, 'y_plus', null)
    expect(plan.steps.map(step => step.kind)).toEqual([
      'start', 'search', 'move', 'contact', 'move', 'contact', 'move', 'contact', 'move', 'result',
    ])
    expect(plan.steps[2]?.point[2]).toBe(3)
    expect(plan.steps[8]?.point).toEqual(plan.steps[0]?.point)
    expect(plan.steps[9]?.contact).toEqual([0, 0, 0])
  })

  it('shows a single pass on a setter with no latch configured', () => {
    const single = { ...limits, latch_feed_mm_min: 0 }
    const plan = probeGuide({ kind: 'teach' }, single, 'y_plus', null)
    expect(plan.steps.map(step => step.kind)).toEqual(['start', 'search', 'move', 'result'])
  })

  it('traverses parallel to the selected axis before the second angle contact', () => {
    const plan = probeGuide({ kind: 'angle' }, limits, 'x_plus', -50)
    const starts = [plan.steps[0], plan.steps.find(step => step.label === 'Move to the next search position')]
    expect(starts[0]?.point[0]).toBe(starts[1]?.point[0])
    expect(starts[0]?.point[1]).toBeGreaterThan(starts[1]?.point[1] ?? 0)
    expect(plan.steps.at(-1)?.label).toContain('example +7°')
  })

  it('draws a 3-point bore as three searches outward along the chosen directions', () => {
    const plan = probeGuide({ kind: 'bore_arc' }, limits, 'y_plus', null, [-80, -40, 0])
    const searches = plan.steps.filter(step => step.kind === 'search')
    expect(searches).toHaveLength(3)
    expect(plan.object).toBe('bore')
    for (const [index, degrees] of [-80, -40, 0].entries()) {
      const contact = searches[index]?.point ?? [0, 0, 0]
      // Outward from the start [2, -3] to the ball-centre circle of radius 11.
      expect(Math.hypot(contact[0], contact[1])).toBeCloseTo(11)
      expect(Math.atan2(contact[1] + 3, contact[0] - 2) * 180 / Math.PI).toBeCloseTo(degrees)
      expect(searches[index]?.label).toBe(`Search · contact at ${String(degrees)}°`)
    }
    // Each latch backs off 1 mm along the search line, back towards the start.
    const first = plan.steps.findIndex(step => step.kind === 'search')
    const back = plan.steps[first + 1]?.point ?? [0, 0, 0]
    const contact = searches[0]?.point ?? [0, 0, 0]
    expect(Math.hypot(back[0] - contact[0], back[1] - contact[1])).toBeCloseTo(1)
  })

  it('draws a 3-point boss from outside above the top, inward along each direction', () => {
    const plan = probeGuide({ kind: 'boss_arc' }, limits, 'y_plus', null, [90, 180, 270])
    const searches = plan.steps.filter(step => step.kind === 'search')
    expect(plan.object).toBe('boss')
    expect(searches.map(step => step.contact?.map(value => Math.round(value) + 0))).toEqual([[0, 14, -4], [-14, 0, -4], [0, -14, -4]])
    expect(plan.steps.some(step => step.label === 'Move outside the face at 180°')).toBe(true)
  })

  it('approaches inside corners from inside and keeps the pocket searches at one Z', () => {
    const corner = probeGuide({ kind: 'inside_corner', x: 'x_minus', y: 'y_plus' }, limits, 'y_plus', null)
    const hits = corner.steps.filter(step => step.kind === 'search')
    expect(hits.map(step => step.label)).toEqual(['Search · −X wall', 'Search · +Y wall'])
    expect(hits[0]?.point[0]).toBeLessThan(corner.steps[0]?.point[0] ?? 0)
    expect(hits[1]?.point[1]).toBeGreaterThan(corner.steps[0]?.point[1] ?? 0)
    const pocket = probeGuide({ kind: 'pocket' }, limits, 'y_plus', null)
    expect(pocket.steps.filter(step => step.kind === 'search')).toHaveLength(4)
    expect(pocket.steps.every(step => step.point[2] === -5)).toBe(true)
  })

  it('keeps the ridge traverse above the top and probes across the chosen axis', () => {
    const plan = probeGuide({ kind: 'ridge', axis: 'y' }, limits, 'y_plus', 14)
    expect(plan.featureHalf).toEqual([14, 7])
    expect(plan.steps.filter(step => step.kind === 'search').map(step => step.label))
      .toEqual(['Search · −Y face', 'Search · +Y face'])
    expect(plan.steps.filter(step => step.label.startsWith('Move above')).every(step => step.point[2] >= 5)).toBe(true)
    expect(plan.steps.at(-1)?.point[2]).toBe(9)
  })
})
