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
})
