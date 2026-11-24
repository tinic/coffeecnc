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
    "max_spindle_rpm": 5000,
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
    "operator_settings_storage": 1,
    "quit_supported": true,
    "macros": [
      { "label": "Park", "mdi": "G53 G0 Z0" },
      { "label": "Clamp check", "mdi": "O<clamp> call" }
    ],
    "diagnostics": true
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
    "operator_settings_storage": 1,
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
        "on_hard_limit": false,
        "reference_mark": true,
        "at_reference_mark": true
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
        "on_hard_limit": false,
        "reference_mark": true,
        "at_reference_mark": true
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
        "on_hard_limit": false,
        "reference_mark": true,
        "at_reference_mark": true
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
        "on_hard_limit": false,
        "reference_mark": false,
        "at_reference_mark": false
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
        "on_hard_limit": false,
        "reference_mark": false,
        "at_reference_mark": false
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
        "on_hard_limit": false,
        "reference_mark": false,
        "at_reference_mark": false
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
    "probe_input": false,
    "paused_z": {
      "offset": 0,
      "max_lift": 20,
      "displaced": false,
      "reason": "Pause a running program before jogging Z.",
      "localization": {
        "key": "backend.nmlStatus.pauseFirst",
        "params": {}
      }
    }
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
        "available": true,
        "current": true
      },
      {
        "wcs": 2,
        "offset": [
          100,
          100,
          -100
        ],
        "rotation_xy": 15,
        "available": true,
        "current": true
      },
      {
        "wcs": 3,
        "offset": [
          -12.5,
          0.125,
          0
        ],
        "rotation_xy": -90,
        "available": true,
        "current": false
      },
      {
        "wcs": 4,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true,
        "current": true
      },
      {
        "wcs": 5,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true,
        "current": true
      },
      {
        "wcs": 6,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true,
        "current": true
      },
      {
        "wcs": 7,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true,
        "current": true
      },
      {
        "wcs": 8,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": true,
        "current": true
      },
      {
        "wcs": 9,
        "offset": [
          1,
          2,
          3
        ],
        "rotation_xy": 0,
        "available": true,
        "current": true
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
        "available": false,
        "current": false
      },
      {
        "wcs": 2,
        "offset": [
          100,
          100,
          -100
        ],
        "rotation_xy": 15,
        "available": true,
        "current": true
      },
      {
        "wcs": 3,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false,
        "current": false
      },
      {
        "wcs": 4,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false,
        "current": false
      },
      {
        "wcs": 5,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false,
        "current": false
      },
      {
        "wcs": 6,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false,
        "current": false
      },
      {
        "wcs": 7,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false,
        "current": false
      },
      {
        "wcs": 8,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false,
        "current": false
      },
      {
        "wcs": 9,
        "offset": [
          0,
          0,
          0
        ],
        "rotation_xy": 0,
        "available": false,
        "current": false
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
    "truncated": false,
    "stamp": "9f6b1c2e04d3a587"
  },
  {
    "type": "program",
    "revision": 4,
    "file": "/home/cnc/nc_files/huge.ngc",
    "lines": [
      "G21",
      "G0 X0"
    ],
    "truncated": true,
    "stamp": "0d41e5a3b2c79f10"
  },
  {
    "type": "preview",
    "context": 4821340,
    "state": "failed",
    "error": {
      "line": 3,
      "text": "The preview ends at line 3: a (log,...) comment writes a file.",
      "localization": {
        "key": "backend.previewEngine.endsAtLine",
        "params": { "line": "3", "detail": "a (log,...) comment writes a file" },
        "params_localization": {
          "detail": { "key": "backend.previewEngine.stopFileWritingComment", "params": { "word": "log," } }
        }
      }
    },
    "segments": [],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "preview",
    "context": 4821341,
    "state": "failed",
    "error": {
      "line": 0,
      "text": "The preview program's result is not valid (a record of an unknown kind); the path is shown as far as it was.",
      "localization": {
        "key": "backend.previewEngine.invalidResult",
        "params": { "detail": "a record of an unknown kind" },
        "params_localization": {
          "detail": { "key": "backend.previewWire.unknownRecord", "params": {} }
        }
      }
    },
    "segments": [],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "preview",
    "context": 4821342,
    "state": "failed",
    "error": {
      "line": 3,
      "text": "The preview ends at line 3: a probe move (G38): where it ends depends on the machine.",
      "localization": {
        "key": "backend.previewEngine.endsAtLine",
        "params": { "line": "3", "detail": "a probe move (G38): where it ends depends on the machine" },
        "params_localization": {
          "detail": { "key": "backend.previewEngine.stopProbe", "params": {} }
        }
      }
    },
    "segments": [],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "preview",
    "context": 4821343,
    "state": "failed",
    "error": {
      "line": 3,
      "text": "The preview ends at line 3: M199 runs a program of the machine's own: what it does is not known to the preview.",
      "localization": {
        "key": "backend.previewEngine.endsAtLine",
        "params": { "line": "3", "detail": "M199 runs a program of the machine's own: what it does is not known to the preview" },
        "params_localization": {
          "detail": { "key": "backend.previewEngine.stopMachineMCode", "params": { "code": "199" } }
        }
      }
    },
    "segments": [],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "preview",
    "context": 4821344,
    "state": "failed",
    "error": {
      "line": 5,
      "text": "The preview ends at line 5: inner.ngc line 3: a probe move (G38): where it ends depends on the machine.",
      "localization": {
        "key": "backend.previewEngine.endsAtSubroutineLine",
        "params": {
          "line": "5", "file": "inner.ngc", "sourceLine": "3",
          "detail": "a probe move (G38): where it ends depends on the machine"
        },
        "params_localization": {
          "detail": { "key": "backend.previewEngine.stopProbe", "params": {} }
        }
      }
    },
    "segments": [],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "preview",
    "context": 4821337,
    "state": "pending",
    "error": null,
    "segments": [],
    "bounds": null,
    "dwells": []
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
        ],
        "feed": 0
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
        ],
        "feed": 450
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
    },
    "dwells": [
      {
        "line": 6,
        "seconds": 2
      }
    ],
    "check": {
      "tools": [],
      "frames": [],
      "feed": null,
      "spindle": null,
      "units": [],
      "travel": "unknown",
      "errors": [
        "T3 has no verified length in the tool table."
      ],
      "warnings": [
        "Additional controller check is required.",
        "Feed per revolution is excluded from the feed range."
      ],
      "errors_localization": [
        {
          "key": "backend.previewEngine.noVerifiedLength",
          "params": {
            "tool": "3"
          }
        }
      ],
      "warnings_localization": [
        null,
        {
          "key": "backend.previewEngine.feedPerRevolution",
          "params": {}
        }
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
        ],
        "feed": 0
      }
    ],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "preview",
    "context": 0,
    "state": "unavailable",
    "error": null,
    "segments": [],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "preview",
    "context": 4821339,
    "state": "unavailable",
    "error": {
      "line": 0,
      "text": "Cutter compensation (G41) is active: no preview.",
      "localization": {
        "key": "backend.previewEngine.cutterCompensation",
        "params": {
          "code": "G41"
        }
      }
    },
    "segments": [],
    "bounds": null,
    "dwells": []
  },
  {
    "type": "message",
    "level": "error",
    "text": "The text of /home/cnc/outside.ngc cannot be shown: Path is outside the program directory.",
    "time": 1700000001600,
    "localization": {
      "key": "backend.nmlPanelSource.textNotShown",
      "params": { "file": "/home/cnc/outside.ngc", "detail": "Path is outside the program directory." },
      "params_localization": {
        "detail": { "key": "backend.programStore.outsideDirectory", "params": {} }
      }
    }
  },
  {
    "type": "message",
    "level": "error",
    "text": "The text of /home/cnc/part.ngc cannot be shown: Program cannot be read: The file cannot be read.",
    "time": 1700000001700,
    "localization": {
      "key": "backend.nmlPanelSource.programReadFailed",
      "params": { "file": "/home/cnc/part.ngc", "detail": "The file cannot be read." },
      "params_localization": {
        "detail": { "key": "backend.fileSystem.cannotRead", "params": {} }
      }
    }
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
    "text": "T1 restored in the spindle.",
    "time": 1700000001000,
    "localization": {
      "key": "backend.nmlPort.restored",
      "params": {
        "tool": "1"
      }
    }
  },
  {
    "type": "message",
    "level": "error",
    "text": "The restore was not completed: /home/cnc/mill.ini was replaced, but its folder cannot be synchronised: Input/output error The files are as they were.",
    "time": 1700000001800,
    "localization": {
      "key": "backend.machineData.notCompleted",
      "params": { "detail": "/home/cnc/mill.ini was replaced, but its folder cannot be synchronised: Input/output error" },
      "params_localization": {
        "detail": {
          "key": "backend.machineData.configFolderNotSynced",
          "params": { "path": "/home/cnc/mill.ini", "detail": "Input/output error" }
        }
      }
    }
  },
  {
    "type": "message",
    "level": "info",
    "text": "Jog stopped: probe contact",
    "time": 1700000001100,
    "localization": {
      "key": "backend.jogStop.probeContact",
      "params": {}
    }
  },
  {
    "type": "message",
    "level": "error",
    "text": "Panel request ignored: Missing required field: id.",
    "time": 1700000001200,
    "localization": {
      "key": "messages.session.requestIgnored",
      "params": { "detail": "Missing required field: id." },
      "params_localization": {
        "detail": { "key": "messages.request.missingField", "params": { "field": "id" } }
      }
    }
  },
  {
    "type": "message",
    "level": "error",
    "text": "The machine status cannot be shown: status: motion.velocity is not a finite number.",
    "time": 1700000001300,
    "localization": {
      "key": "messages.panelFeed.statusCannotShow",
      "params": { "detail": "status: motion.velocity is not a finite number." },
      "params_localization": {
        "detail": { "key": "messages.wire.notFinite", "params": { "field": "status: motion.velocity" } }
      }
    }
  },
  {
    "type": "message",
    "level": "error",
    "text": "Jog stop failed on X: LinuxCNC did not answer.",
    "time": 1700000001400,
    "localization": {
      "key": "messages.session.jogStopFailed",
      "params": { "axis": "X", "detail": "LinuxCNC did not answer." },
      "params_localization": {
        "detail": { "key": "messages.session.linuxcncNoAnswer", "params": {} }
      }
    }
  },
  {
    "type": "result",
    "id": 53,
    "outcome": "failed",
    "reason": "Saved on disk, but not reopened. LinuxCNC did not open the program.",
    "localization": {
      "key": "messages.session.savedNotReopened",
      "params": { "detail": "LinuxCNC did not open the program." },
      "params_localization": {
        "detail": { "key": "backend.nmlPort.notOpened", "params": {} }
      }
    }
  },
  {
    "type": "result",
    "id": 41,
    "outcome": "accepted",
    "reason": ""
  },
  {
    "type": "result",
    "id": 89,
    "outcome": "failed",
    "reason": "The backup was not written: /home/cnc/mill.ini cannot be read: Input/output error",
    "localization": {
      "key": "backend.machineData.notWritten",
      "params": { "detail": "/home/cnc/mill.ini cannot be read: Input/output error" },
      "params_localization": {
        "detail": {
          "key": "backend.machineData.cannotRead",
          "params": { "path": "/home/cnc/mill.ini", "detail": "Input/output error" }
        }
      }
    }
  },
  {
    "type": "result",
    "id": 58,
    "outcome": "rejected",
    "reason": "This line is inside a canned cycle.",
    "refusal": {
      "key": "backend.previewRestart.cannedCycle",
      "params": {}
    }
  },
  {
    "type": "result",
    "id": 42,
    "outcome": "rejected",
    "reason": "Home all configured axes first."
  },
  {
    "type": "result",
    "id": 51,
    "outcome": "rejected",
    "reason": "The interpreter modes changed. Review a fresh program check.",
    "refusal": {
      "key": "backend.nmlPort.modesChanged",
      "params": {}
    }
  },
  {
    "type": "result",
    "id": 46,
    "outcome": "rejected",
    "reason": "Jog velocity is above the Z axis limit of 750 mm/min.",
    "refusal": {
      "key": "messages.policy.jogVelocityAxisLimit",
      "params": { "axis": "Z", "limit": "750" }
    }
  },
  {
    "type": "result",
    "id": 43,
    "outcome": "failed",
    "reason": "LinuxCNC reported an error.",
    "localization": {
      "key": "backend.nmlPort.reportedError",
      "params": {}
    }
  },
  {
    "type": "result",
    "id": 44,
    "outcome": "timed_out",
    "reason": "LinuxCNC did not answer."
  },
  {
    "type": "result",
    "id": 56,
    "outcome": "timed_out",
    "reason": "The previous jog stop on X is not confirmed: LinuxCNC did not answer.",
    "localization": {
      "key": "messages.session.previousStopAxis",
      "params": { "axis": "X", "detail": "LinuxCNC did not answer." },
      "params_localization": {
        "detail": { "key": "messages.session.linuxcncNoAnswer", "params": {} }
      }
    }
  },
  {
    "type": "result",
    "id": 47,
    "outcome": "timed_out",
    "reason": "LinuxCNC did not answer.",
    "localization": {
      "key": "messages.session.linuxcncNoAnswer",
      "params": {}
    }
  },
  {
    "type": "result",
    "id": 48,
    "outcome": "failed",
    "reason": "A program named part.ngc exists already.",
    "localization": {
      "key": "backend.programEdit.exists",
      "params": { "name": "part.ngc" }
    }
  },
  {
    "type": "result",
    "id": 57,
    "outcome": "rejected",
    "reason": "This line runs in G55. Select G55, then choose the line again.",
    "refusal": {
      "key": "backend.nmlPort.restartSelectWcs",
      "params": { "wcs": "G55" }
    }
  },
  {
    "type": "result",
    "id": 45,
    "outcome": "accepted",
    "reason": "",
    "restart": {
      "plan_id": 3,
      "line": 25,
      "tool": 98,
      "needs_tool_change": true,
      "wcs": 1,
      "h": 98,
      "spindle_direction": "stopped",
      "spindle_rpm": 0,
      "flood": false,
      "mist": false,
      "feed": 600,
      "units": "mm",
      "plane": "XY",
      "distance_mode": "absolute",
      "clearance_z": 30,
      "start": [
        -16,
        12,
        30
      ],
      "approach_feed": 300
    }
  },
  {
    "type": "result",
    "id": 49,
    "outcome": "rejected",
    "reason": "Missing required field: path.",
    "refusal": {
      "key": "messages.request.missingField",
      "params": { "field": "path" }
    }
  },
  {
    "type": "result",
    "id": 52,
    "outcome": "failed",
    "reason": "Tool measured. The updated program check is unavailable.",
    "localization": {
      "key": "backend.toolChangeMeasurement.measuredCheckUnavailable",
      "params": {}
    }
  },
  {
    "type": "message",
    "level": "error",
    "text": "Program progress could not be saved. the temporary file cannot be made: Permission denied",
    "time": 1700000001500,
    "localization": {
      "key": "backend.programProgress.notSaved",
      "params": { "storageError": "the temporary file cannot be made: Permission denied" }
    }
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
        },
        "touch-probe": {
          "diameter": 4,
          "shankDiameter": 12,
          "bodyDiameter": 28,
          "bodyLength": 32,
          "stylusLength": 20
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
    "reason": "Tool model settings in /data/G0759 are already in use by another CoffeeCNC. Tool models are not saved.",
    "document": null
  },
  {
    "type": "tool_models_reason",
    "machine_id": "G0759",
    "revision": 0,
    "reason": "Tool model settings in /data/G0759 are already in use by another CoffeeCNC. Tool models are not saved.",
    "localization": {
      "key": "backend.toolModelStore.inUse",
      "params": { "path": "/data/G0759" }
    }
  },
  {
    "type": "operator_settings",
    "machine_id": "G0759",
    "revision": 2,
    "availability": "available",
    "reason": "",
    "document": {
      "version": 1,
      "startup_units": "inch",
      "enabled_wcs": [
        1,
        2,
        3
      ],
      "default_spindle_rpm": 1800,
      "default_jog_velocity": 600,
      "tool_reference_height": 6.35,
      "program_layout": "split",
      "theme": "dark",
      "language": "ja",
      "recent_programs": [
        "/home/cnc/nc_files/air/03-pocket.ngc"
      ],
      "saved_positions": [
        {
          "name": "Vise jaw",
          "position": [
            -120.5,
            33.25,
            -10
          ]
        }
      ],
      "go_to_safe_z": 40
    }
  },
  {
    "type": "operator_settings",
    "machine_id": null,
    "revision": 0,
    "availability": "session_only",
    "reason": "No machine identity configured",
    "document": null
  },
  {
    "type": "operator_settings",
    "machine_id": "G0759",
    "revision": 0,
    "availability": "unavailable",
    "reason": "/home/cnc/operator-settings.json exceeds 16 KiB. The file was left alone.",
    "document": null,
    "localization": {
      "key": "backend.operatorSettingsStore.fileTooLarge",
      "params": {
        "file": "/home/cnc/operator-settings.json"
      }
    }
  },
  {
    "type": "result",
    "id": 53,
    "outcome": "rejected",
    "reason": "Default jog speed exceeds the configured machine limit.",
    "refusal": {
      "key": "backend.operatorSettings.jogSpeedLimit",
      "params": {}
    }
  },
  {
    "type": "result",
    "id": 54,
    "outcome": "rejected",
    "reason": "Invalid panel presentation.",
    "refusal": {
      "key": "backend.operatorSettings.invalidPresentation",
      "params": {}
    }
  },
  {
    "type": "offset_history",
    "revision": 4,
    "storage": "saved",
    "reason": "",
    "entries": [
      {
        "id": 12,
        "time": 1700000003000,
        "source": "restore",
        "target": "wcs",
        "number": 1,
        "changes": [
          {
            "field": "x",
            "before": 0,
            "after": 112.5
          }
        ],
        "restores": 10
      },
      {
        "id": 11,
        "time": 1700000002000,
        "source": "tool_table",
        "target": "tool",
        "number": 3,
        "changes": [
          {
            "field": "length",
            "before": 48.2,
            "after": 50
          },
          {
            "field": "diameter",
            "before": 6,
            "after": 6.35
          }
        ],
        "restores": null
      },
      {
        "id": 10,
        "time": 1700000001000,
        "source": "zero",
        "target": "wcs",
        "number": 1,
        "changes": [
          {
            "field": "x",
            "before": 112.5,
            "after": 0
          }
        ],
        "restores": null
      },
      {
        "id": 9,
        "time": 1700000000500,
        "source": "outside",
        "target": "g92",
        "number": 0,
        "changes": [
          {
            "field": "z",
            "before": 0,
            "after": 2
          }
        ],
        "restores": null
      },
      {
        "id": 8,
        "time": 1700000000250,
        "source": "probe",
        "target": "wcs",
        "number": 2,
        "changes": [
          {
            "field": "rotation_xy",
            "before": null,
            "after": 15
          }
        ],
        "restores": null
      },
      {
        "id": 7,
        "time": 1700000000000,
        "source": "tool_table",
        "target": "tool",
        "number": 4,
        "changes": [
          {
            "field": "length",
            "before": 61.75,
            "after": null
          },
          {
            "field": "diameter",
            "before": 3.175,
            "after": null
          }
        ],
        "restores": null
      }
    ]
  },
  {
    "type": "offset_history",
    "revision": 1,
    "storage": "session_only",
    "reason": "No machine identity configured",
    "entries": []
  },
  {
    "type": "offset_history",
    "revision": 2,
    "storage": "saved",
    "reason": "The saved offset history was damaged and starts empty. The damaged file is kept as offset-history.json.corrupt.",
    "entries": [],
    "localization": {
      "key": "backend.offsetHistory.damagedKept",
      "params": { "file": "offset-history.json.corrupt" }
    }
  },
  {
    "type": "alarm_log",
    "storage": "saved",
    "reason": "",
    "session": 3,
    "skipped": 1,
    "entries": [
      { "session": 1, "time": 1700000000000, "level": "error", "text": "Joint 2 following error", "outcome": null, "command": null },
      { "session": 2, "time": 1700000100000, "level": "info", "text": "Program loaded", "outcome": null, "command": null },
      { "session": 2, "time": 1700000105000, "level": "error", "text": "The machine is off.", "outcome": "rejected",
        "command": { "type": "home", "axis": "x" } }
    ]
  },
  {
    "type": "alarm_log",
    "storage": "saved",
    "reason": "The alarm log could not be saved: No space left on device",
    "session": 3,
    "skipped": 0,
    "entries": [],
    "localization": {
      "key": "backend.alarmLog.notSaved",
      "params": { "detail": "No space left on device" }
    }
  },
  {
    "type": "alarm_log",
    "storage": "session_only",
    "reason": "No machine identity configured",
    "session": 1,
    "skipped": 0,
    "entries": []
  },
  {"type": "backup_written", "path": "/media/cnc/STICK/coffeecnc-G0759-20261123-140200.tar.gz", "files": 8, "bytes": 5321, "not_included": [{"path": "LIB:basic_sim.tcl", "reason": "a HAL file of LinuxCNC's own library", "localization": {"key": "backend.machineData.libHal", "params": {}}}]},
  {"type": "backup_written", "path": "/home/cnc/coffeecnc-machine-20261123-140200.tar.gz", "files": 3, "bytes": 1024, "not_included": []},
  {"type": "result", "id": 31, "outcome": "accepted", "reason": "Nothing to restore: every file is the same.", "localization": {"key": "backend.machineData.nothing", "params": {}}},
  {"type": "restore_summary", "path": "/media/cnc/STICK/coffeecnc-G0759-20261123-140200.tar.gz", "token": "3f1c8e0a9b2d4c75", "machine_id": "G0759", "created": 1795960920000, "files": [{"role": "ini", "name": "config/g0759.ini", "target": "/home/cnc/linuxcnc/configs/g0759/g0759.ini", "change": "same", "bytes": 4012, "symlink": false}, {"role": "hal", "name": "config/g0759.hal", "target": "/home/cnc/linuxcnc/configs/g0759/g0759.hal", "change": "changed", "bytes": 2210, "symlink": true}, {"role": "tool_table", "name": "tool-table/tool.tbl", "target": "/home/cnc/linuxcnc/configs/g0759/tool.tbl", "change": "changed", "bytes": 310, "symlink": false}, {"role": "parameters", "name": "parameters/g0759.var", "target": "/home/cnc/linuxcnc/configs/g0759/g0759.var", "change": "at_exit", "bytes": 1882, "symlink": false}, {"role": "data", "name": "data/tool-models.json", "target": "/home/cnc/.local/share/coffeecnc/G0759/tool-models.json", "change": "new", "bytes": 512, "symlink": false}]},
  {"type": "restore_summary", "path": "/home/cnc/old.tar.gz", "token": "0000000000000001", "machine_id": null, "created": 0, "files": [{"role": "ini", "name": "config/mill.ini", "target": "/home/cnc/linuxcnc/configs/mill/mill.ini", "change": "same", "bytes": 10, "symlink": false}]},
  {
    "type": "message",
    "level": "error",
    "text": "The restore was not completed: The restore was prepared 181 s before CoffeeCNC ended, more than 120 s: nothing was written. The files are as they were.",
    "time": 1700000001700,
    "localization": {
      "key": "backend.machineData.notCompleted",
      "params": { "detail": "The restore was prepared 181 s before CoffeeCNC ended, more than 120 s: nothing was written." },
      "params_localization": {
        "detail": { "key": "backend.machineData.stale", "params": { "age": "181", "limit": "120" } }
      }
    }
  },
  {
    "type": "message",
    "level": "error",
    "text": "The restore was not completed: LinuxCNC was started while the files were written (/tmp/linuxcnc.lock was removed). The files are as they were.",
    "time": 1700000001800,
    "localization": {
      "key": "backend.machineData.notCompleted",
      "params": { "detail": "LinuxCNC was started while the files were written (/tmp/linuxcnc.lock was removed)." },
      "params_localization": {
        "detail": { "key": "backend.machineData.restoreLockLost", "params": { "path": "/tmp/linuxcnc.lock" } }
      }
    }
  },
  {
    "type": "diagnostics",
    "inputs": [
      { "role": "min_limit", "joint": 0, "axis": "x", "name": "joint.0.neg-lim-sw-in", "kind": "pin", "state": "undriven", "value": false },
      { "role": "home_switch", "joint": 0, "axis": "x", "name": "joint.0.home-sw-in", "kind": "pin", "state": "live", "value": true },
      { "role": "amp_fault", "joint": 0, "axis": "x", "name": "joint.0.amp-fault-in", "kind": "pin", "state": "unwired", "value": false },
      { "role": "estop_chain", "name": "iocontrol.0.emc-enable-in", "kind": "pin", "state": "live", "value": true },
      { "role": "spindle_speed_out", "name": "spindle.0.speed-out", "kind": "pin", "state": "live", "value": 1200 },
      { "role": "spindle_speed_in", "name": "spindle.0.speed-in", "kind": "pin", "state": "live", "value": null },
      { "role": "configured", "label": "Setter contact", "name": "setter-tripped", "kind": "signal", "state": "live", "value": false },
      { "role": "configured", "label": "VFD errors", "name": "vfd.modbus-errors", "kind": "pin", "state": "missing", "value": null }
    ],
    "tools": [
      { "tool": "halshow", "available": true, "open": false },
      { "tool": "halmeter", "available": true, "open": true },
      { "tool": "halscope", "available": false, "open": false }
    ]
  },
  {
    "type": "result",
    "id": 50,
    "outcome": "rejected",
    "reason": "halscope is not installed: it is not in the search path (/usr/bin:/bin).",
    "refusal": {
      "key": "backend.halDiagnostics.notInstalled",
      "params": { "name": "halscope", "path": "/usr/bin:/bin" }
    }
  },
  {
    "type": "result",
    "id": 55,
    "outcome": "failed",
    "reason": "LinuxCNC is not waiting for that tool change any more.",
    "localization": {
      "key": "backend.nmlPort.toolChangeGone",
      "params": {}
    }
  },
  {
    "type": "result",
    "id": 59,
    "outcome": "failed",
    "reason": "Home the machine before jogging Y: two motors drive it.",
    "localization": {
      "key": "messages.policy.homeBeforeGantryJog",
      "params": { "axis": "Y", "motors": "two" }
    }
  }
] satisfies FromMachine[]

// A new FromMachine message requires coverage here, even if its fields are all optional.
const messageTypes: Record<FromMachine['type'], true> = {
  hello: true, status: true, tool_table: true, tool_models: true, tool_models_reason: true, operator_settings: true, work_offsets: true, program: true,
  preview: true, message: true, result: true, directory: true, offset_history: true, alarm_log: true, backup_written: true,
  restore_summary: true, diagnostics: true,
}

describe('protocol/fixtures/messages.json', () => {
  it('matches the typed panel contract exactly for every native fixture', () => {
    expect(fixtures).toEqual(examples)
  })

  it('covers every machine-to-panel message kind', () => {
    expect([...new Set(examples.map((message) => message.type))].sort()).toEqual(Object.keys(messageTypes).sort())
  })
})
