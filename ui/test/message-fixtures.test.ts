// The C++ encoders and panel share this fixture. Keep a literal mirror checked by TypeScript:
// changing either wire fields or panel types must update both sides of the contract deliberately.
import { describe, expect, it } from 'vitest'
import fixtures from '../../protocol/fixtures/messages.json'
import type { FromMachine } from '../src/protocol'

const examples = [
  {
    "type": "hello",
    "protocol": 1,
    "machine_name": "G0759",
    "coolant": { "flood": false, "mist": false },
    "simulated": false,
    "axes": [
      "x",
      "y",
      "z"
    ],
    "machine_units": "mm",
    "jog_increments": [
      0.01,
      0.1,
      1,
      10
    ],
    "max_jog_velocity": 1500,
    "axis_max_jog_velocity": {
      "x": 1500,
      "y": 1500,
      "z": 750
    },
    "homing_required": false,
    "max_feed_override": 1.2,
    "min_spindle_override": 0.5,
    "max_spindle_override": 1.5,
    "machine_id": "G0759",
    "tool_model_storage": 1,
    "quit_supported": true
  },
  {
    "type": "hello",
    "protocol": 1,
    "machine_name": "Demo mill",
    "simulated": true,
    "axes": [
      "x",
      "y",
      "z"
    ],
    "machine_units": "inch",
    "jog_increments": [
      0.0254,
      0.254,
      2.54
    ],
    "max_jog_velocity": 3048,
    "axis_max_jog_velocity": {
      "x": 3048,
      "y": 3048,
      "z": 3048
    },
    "homing_required": true,
    "max_feed_override": 2,
    "min_spindle_override": 0.5,
    "max_spindle_override": 1,
    "machine_id": null,
    "tool_model_storage": 1,
    "quit_supported": true
  },
  {
    "type": "status",
    "seq": 1,
    "link": "live",
    "machine": "on",
    "mode": "auto",
    "interp": "reading",
    "axes": [
      {
        "axis": "x",
        "homed": true,
        "homing": false,
        "machine": 112.5,
        "work": 12.5,
        "dtg": 3.25,
        "min_limit": 0,
        "max_limit": 300,
        "on_hard_limit": false
      },
      {
        "axis": "y",
        "homed": true,
        "homing": false,
        "machine": 80,
        "work": -20,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 200,
        "on_hard_limit": false
      },
      {
        "axis": "z",
        "homed": true,
        "homing": false,
        "machine": -42.75,
        "work": 5.45,
        "dtg": 0,
        "min_limit": -120,
        "max_limit": 0,
        "on_hard_limit": false
      }
    ],
    "offsets": {
      "wcs": 2,
      "g5x": [
        100,
        100,
        -100
      ],
      "g92": [
        0,
        0,
        3.6
      ],
      "tool": [
        0,
        0,
        48.2
      ],
      "rotation_xy": 15
    },
    "motion": {
      "velocity": 540,
      "feed_mode": "per_minute",
      "programmed_feed": 600,
      "feed_override": 0.9,
      "rapid_override": 0.5,
      "in_position": false
    },
    "spindle": {
      "direction": "forward",
      "commanded_rpm": 6000,
      "override": 1.1,
      "actual_rpm": 6587.5,
      "at_speed": true
    },
    "coolant": {
      "flood": true,
      "mist": false
    },
    "tool": {
      "in_spindle": 3,
      "prepared": 4
    },
    "program": {
      "file": "/home/cnc/nc_files/part.ngc",
      "revision": 3,
      "current_line": 12,
      "motion_line": 10,
      "optional_stop": true,
      "block_delete": false
    },
    "preview_context": 4821337,
    "wait": {
      "kind": "none"
    },
    "active_codes": [
      "G1",
      "G55",
      "G90",
      "G94",
      "M3",
      "M8"
    ],
    "program_units": "mm",
    "probe_input": true
  },
  {
    "type": "status",
    "seq": 2,
    "link": "stale",
    "machine": "on",
    "mode": "auto",
    "interp": "paused",
    "axes": [
      {
        "axis": "x",
        "homed": true,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 300,
        "on_hard_limit": false
      },
      {
        "axis": "y",
        "homed": true,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 200,
        "on_hard_limit": false
      },
      {
        "axis": "z",
        "homed": true,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": -120,
        "max_limit": 0,
        "on_hard_limit": false
      }
    ],
    "offsets": {
      "wcs": 1,
      "g5x": [
        0,
        0,
        0
      ],
      "g92": [
        0,
        0,
        0
      ],
      "tool": [
        0,
        0,
        0
      ],
      "rotation_xy": 0
    },
    "motion": {
      "velocity": 0,
      "feed_mode": "inverse_time",
      "programmed_feed": 2.5,
      "feed_override": 1,
      "rapid_override": 1,
      "in_position": true
    },
    "spindle": {
      "direction": "reverse",
      "commanded_rpm": 800,
      "override": 1,
      "actual_rpm": 0,
      "at_speed": false
    },
    "coolant": {
      "flood": false,
      "mist": false
    },
    "tool": {
      "in_spindle": 3,
      "prepared": null
    },
    "program": {
      "file": "/home/cnc/nc_files/part.ngc",
      "revision": 3,
      "current_line": 12,
      "motion_line": 10,
      "optional_stop": true,
      "block_delete": false
    },
    "preview_context": 4821337,
    "wait": {
      "kind": "paused"
    },
    "active_codes": [
      "G1",
      "G54",
      "G93",
      "M4"
    ],
    "program_units": "mm",
    "probe_input": false
  },
  {
    "type": "status",
    "seq": 3,
    "link": "down",
    "machine": "estop",
    "mode": "manual",
    "interp": "idle",
    "axes": [
      {
        "axis": "x",
        "homed": false,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 0,
        "on_hard_limit": false
      },
      {
        "axis": "y",
        "homed": false,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 0,
        "on_hard_limit": false
      },
      {
        "axis": "z",
        "homed": false,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 0,
        "on_hard_limit": false
      }
    ],
    "offsets": {
      "wcs": 1,
      "g5x": [
        0,
        0,
        0
      ],
      "g92": [
        0,
        0,
        0
      ],
      "tool": [
        0,
        0,
        0
      ],
      "rotation_xy": 0
    },
    "motion": {
      "velocity": 0,
      "feed_mode": "per_minute",
      "programmed_feed": 0,
      "feed_override": 1,
      "rapid_override": 1,
      "in_position": true
    },
    "spindle": {
      "direction": "stopped",
      "commanded_rpm": 0,
      "override": 1,
      "actual_rpm": null,
      "at_speed": null
    },
    "coolant": {
      "flood": false,
      "mist": false
    },
    "tool": {
      "in_spindle": 0,
      "prepared": null
    },
    "program": {
      "file": null,
      "revision": 0,
      "current_line": 0,
      "motion_line": 0,
      "optional_stop": false,
      "block_delete": false
    },
    "preview_context": 0,
    "wait": {
      "kind": "none"
    },
    "active_codes": [],
    "program_units": "mm",
    "probe_input": null
  },
  {
    "type": "status",
    "seq": 4,
    "link": "live",
    "machine": "on",
    "mode": "auto",
    "interp": "waiting",
    "axes": [
      {
        "axis": "x",
        "homed": true,
        "homing": false,
        "machine": 10,
        "work": 10,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 300,
        "on_hard_limit": false
      },
      {
        "axis": "y",
        "homed": true,
        "homing": false,
        "machine": 20,
        "work": 20,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 200,
        "on_hard_limit": false
      },
      {
        "axis": "z",
        "homed": true,
        "homing": false,
        "machine": -1.5,
        "work": -1.5,
        "dtg": 0,
        "min_limit": -120,
        "max_limit": 0,
        "on_hard_limit": true
      }
    ],
    "offsets": {
      "wcs": 1,
      "g5x": [
        0,
        0,
        0
      ],
      "g92": [
        0,
        0,
        0
      ],
      "tool": [
        0,
        0,
        0
      ],
      "rotation_xy": 0
    },
    "motion": {
      "velocity": 0,
      "feed_mode": "per_revolution",
      "programmed_feed": 0.05,
      "feed_override": 1,
      "rapid_override": 1,
      "in_position": true
    },
    "spindle": {
      "direction": "stopped",
      "commanded_rpm": 0,
      "override": 1,
      "actual_rpm": null,
      "at_speed": null
    },
    "coolant": {
      "flood": false,
      "mist": false
    },
    "tool": {
      "in_spindle": 3,
      "prepared": 4
    },
    "program": {
      "file": "/home/cnc/nc_files/part.ngc",
      "revision": 3,
      "current_line": 8,
      "motion_line": 7,
      "optional_stop": false,
      "block_delete": true
    },
    "preview_context": 4821337,
    "wait": {
      "kind": "tool_change",
      "tool": 4
    },
    "active_codes": [
      "G0",
      "G20",
      "G95"
    ],
    "program_units": "inch",
    "probe_input": null
  },
  {
    "type": "status",
    "seq": 5,
    "link": "live",
    "machine": "on",
    "mode": "mdi",
    "interp": "waiting",
    "axes": [
      {
        "axis": "x",
        "homed": false,
        "homing": true,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 300,
        "on_hard_limit": false
      },
      {
        "axis": "y",
        "homed": false,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": 0,
        "max_limit": 200,
        "on_hard_limit": false
      },
      {
        "axis": "z",
        "homed": true,
        "homing": false,
        "machine": 0,
        "work": 0,
        "dtg": 0,
        "min_limit": -120,
        "max_limit": 0,
        "on_hard_limit": false
      }
    ],
    "offsets": {
      "wcs": 1,
      "g5x": [
        0,
        0,
        0
      ],
      "g92": [
        0,
        0,
        0
      ],
      "tool": [
        0,
        0,
        0
      ],
      "rotation_xy": 0
    },
    "motion": {
      "velocity": 0,
      "feed_mode": "per_minute",
      "programmed_feed": 0,
      "feed_override": 1,
      "rapid_override": 1,
      "in_position": true
    },
    "spindle": {
      "direction": "stopped",
      "commanded_rpm": 0,
      "override": 1,
      "actual_rpm": null,
      "at_speed": null
    },
    "coolant": {
      "flood": false,
      "mist": false
    },
    "tool": {
      "in_spindle": 0,
      "prepared": null
    },
    "program": {
      "file": null,
      "revision": 0,
      "current_line": 0,
      "motion_line": 0,
      "optional_stop": false,
      "block_delete": false
    },
    "preview_context": 0,
    "wait": {
      "kind": "dwell",
      "seconds_left": 1.5
    },
    "active_codes": [
      "G4"
    ],
    "program_units": "mm",
    "probe_input": null
  },
  {
    "type": "tool_table",
    "revision": 7,
    "tools": [
      {
        "number": 3,
        "pocket": 3,
        "length": 48.2,
        "diameter": 6,
        "comment": "End mill 6 mm"
      },
      {
        "number": 4,
        "pocket": 4,
        "length": 61.75,
        "diameter": 3.175,
        "comment": ""
      }
    ]
  },
  {
    "type": "tool_table",
    "revision": 0,
    "tools": []
  },
  {
    "type": "work_offsets",
    "revision": 5,
    "systems": [
      {
        "wcs": 1,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true
      },
      {
        "wcs": 2,
        "offset": [
          100,
          100,
          -100
        ],
        "rotation_xy": 15,
        "available": true
      },
      {
        "wcs": 3,
        "offset": [
          -12.5,
          0.125,
          0
        ],
        "rotation_xy": -90,
        "available": true
      },
      {
        "wcs": 4,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true
      },
      {
        "wcs": 5,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true
      },
      {
        "wcs": 6,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true
      },
      {
        "wcs": 7,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true
      },
      {
        "wcs": 8,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true
      },
      {
        "wcs": 9,
        "offset": [
          1,
          2,
          3
        ],
        "rotation_xy": 0,
        "available": true
      }
    ]
  },
  {
    "type": "work_offsets",
    "revision": 6,
    "systems": [
      {
        "wcs": 1,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      },
      {
        "wcs": 2,
        "offset": [
          100,
          100,
          -100
        ],
        "rotation_xy": 15,
        "available": true
      },
      {
        "wcs": 3,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      },
      {
        "wcs": 4,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      },
      {
        "wcs": 5,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      },
      {
        "wcs": 6,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      },
      {
        "wcs": 7,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      },
      {
        "wcs": 8,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      },
      {
        "wcs": 9,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false
      }
    ]
  },
  {
    "type": "program",
    "revision": 3,
    "file": "/home/cnc/nc_files/part.ngc",
    "lines": [
      "G21 G90 G54",
      "",
      "(pi\u00e8ce)",
      "G0 X0 Y0 Z10",
      "M2"
    ],
    "truncated": false
  },
  {
    "type": "program",
    "revision": 4,
    "file": "/home/cnc/nc_files/huge.ngc",
    "lines": [
      "G21",
      "G0 X0"
    ],
    "truncated": true
  },
  {
    "type": "preview",
    "context": 4821337,
    "state": "pending",
    "error": null,
    "segments": [],
    "bounds": null
  },
  {
    "type": "preview",
    "context": 4821337,
    "state": "ready",
    "error": null,
    "segments": [
      {
        "line": 4,
        "kind": "rapid",
        "tool": 3,
        "points": [
          0,
          0,
          10,
          0,
          0,
          2
        ]
      },
      {
        "line": 5,
        "kind": "feed",
        "tool": 3,
        "points": [
          0,
          0,
          2,
          0,
          0,
          -1.5,
          30,
          0,
          -1.5,
          30,
          20.5,
          -1.5
        ]
      }
    ],
    "bounds": {
      "min": [
        0,
        0,
        -1.5
      ],
      "max": [
        30,
        20.5,
        2
      ]
    }
  },
  {
    "type": "preview",
    "context": 4821338,
    "state": "failed",
    "error": {
      "line": 6,
      "text": "Unknown G code used"
    },
    "segments": [
      {
        "line": 4,
        "kind": "rapid",
        "tool": 3,
        "points": [
          0,
          0,
          10,
          0,
          0,
          2
        ]
      }
    ],
    "bounds": null
  },
  {
    "type": "preview",
    "context": 0,
    "state": "unavailable",
    "error": null,
    "segments": [],
    "bounds": null
  },
  {
    "type": "message",
    "level": "error",
    "text": "Joint 0 on limit switch error",
    "time": 1700000000123
  },
  {
    "type": "message",
    "level": "info",
    "text": "Probe tripped",
    "time": 1700000001000
  },
  {
    "type": "result",
    "id": 41,
    "outcome": "accepted",
    "reason": ""
  },
  {
    "type": "result",
    "id": 42,
    "outcome": "rejected",
    "reason": "Home all configured axes first."
  },
  {
    "type": "result",
    "id": 43,
    "outcome": "failed",
    "reason": "LinuxCNC reported an error."
  },
  {
    "type": "result",
    "id": 44,
    "outcome": "timed_out",
    "reason": "LinuxCNC did not answer."
  },
  {
    "type": "directory",
    "path": "/home/cnc/nc_files",
    "parent": null,
    "entries": [
      {
        "name": "fixtures",
        "kind": "directory",
        "bytes": 0,
        "modified": 1700000000000
      },
      {
        "name": "part.ngc",
        "kind": "file",
        "bytes": 1834,
        "modified": 1700000123456
      }
    ],
    "truncated": false
  },
  {
    "type": "directory",
    "path": "/home/cnc/nc_files/fixtures",
    "parent": "/home/cnc/nc_files",
    "entries": [],
    "truncated": false
  },
  {
    "type": "directory",
    "path": "/home/cnc/nc_files/many",
    "parent": "/home/cnc/nc_files",
    "entries": [
      {
        "name": "0001.ngc",
        "kind": "file",
        "bytes": 12,
        "modified": 1700000200000
      }
    ],
    "truncated": true
  },
  {
    "type": "tool_models",
    "machine_id": "G0759",
    "revision": 2,
    "availability": "available",
    "reason": "",
    "document": {
      "version": 1,
      "defaults": {
        "end-mill": {
          "diameter": 10,
          "cuttingLength": 22,
          "shankDiameter": 10,
          "overallLength": 72,
          "stickout": 35,
          "tipAngle": 90
        },
        "ball-mill": {
          "diameter": 10,
          "cuttingLength": 20,
          "shankDiameter": 10,
          "overallLength": 72,
          "stickout": 35,
          "tipAngle": 90
        },
        "drill": {
          "diameter": 10,
          "cuttingLength": 60,
          "shankDiameter": 10,
          "overallLength": 100,
          "stickout": 75,
          "tipAngle": 118
        },
        "spot-drill": {
          "diameter": 10,
          "cuttingLength": 15,
          "shankDiameter": 10,
          "overallLength": 60,
          "stickout": 30,
          "tipAngle": 90
        },
        "chamfer": {
          "diameter": 10,
          "cuttingLength": 12,
          "shankDiameter": 10,
          "overallLength": 60,
          "stickout": 30,
          "tipAngle": 90
        },
        "face-mill": {
          "diameter": 50,
          "cuttingLength": 12,
          "shankDiameter": 20,
          "overallLength": 70,
          "stickout": 40,
          "tipAngle": 90
        },
        "generic": {
          "diameter": 10,
          "cuttingLength": 20,
          "shankDiameter": 10,
          "overallLength": 70,
          "stickout": 35,
          "tipAngle": 90
        }
      },
      "assignments": [
        {
          "tool": {
            "number": 3,
            "pocket": 3,
            "diameter": 6,
            "comment": "End mill 6 mm"
          },
          "family": "end-mill",
          "overrides": {
            "stickout": 30
          }
        }
      ]
    }
  },
  {
    "type": "tool_models",
    "machine_id": null,
    "revision": 0,
    "availability": "session_only",
    "reason": "No machine identity configured",
    "document": null
  },
  {
    "type": "tool_models",
    "machine_id": "G0759",
    "revision": 0,
    "availability": "unavailable",
    "reason": "Tool model settings are already in use",
    "document": null
  }
] satisfies FromMachine[]

// A new FromMachine message requires coverage here, even if its fields are all optional.
const messageTypes: Record<FromMachine['type'], true> = {
  hello: true, status: true, tool_table: true, tool_models: true, work_offsets: true, program: true,
  preview: true, message: true, result: true, directory: true,
}

describe('protocol/fixtures/messages.json', () => {
  it('matches the typed panel contract exactly for every native fixture', () => {
    expect(fixtures).toEqual(examples)
  })

  it('covers every machine-to-panel message kind', () => {
    expect([...new Set(examples.map((message) => message.type))].sort()).toEqual(Object.keys(messageTypes).sort())
  })
})
