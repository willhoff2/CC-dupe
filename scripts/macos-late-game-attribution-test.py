#!/usr/bin/env python3
"""Unit tests for macos-late-game-attribution.py's parsing and per-frame conversion.

    python3 scripts/macos-late-game-attribution-test.py
"""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("macos-late-game-attribution.py")
SPEC = importlib.util.spec_from_file_location("attribution", SCRIPT)
attribution = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(attribution)

# Shaped like `sample` output: a recursive Pathfinder frame must be counted once.
SAMPLE = """Call graph:
    1000 Thread_1   DispatchQueue_1: com.apple.main-thread  (serial)
    + 1000 GameEngine::update()  (in zh) + 1  [0x1]
    +   600 GameLogic::update()  (in zh) + 1  [0x1]
    +   ! 500 Pathfinder::processPathfindQueue()  (in zh) + 1  [0x1]
    +   !   400 Pathfinder::processPathfindQueue()  (in zh) + 1  [0x1]
    +   400 W3DGameClient::update()  (in zh) + 1  [0x1]
    +     300 W3DDisplay::draw()  (in zh) + 1  [0x1]
    200 Thread_2
    + 200 worker  (in zh) + 1  [0x1]

Total number in stack
"""


def round_entry(index, start_frame, end_frame, objects, frames, seconds, fps):
    return {
        "index": index,
        "frames_since_last_round": {
            "frames": frames, "seconds": seconds, "fps_effective": fps,
            "interval_median_ms": 40.0, "renderer_median_ms": 25.0,
            "outside_scene_median_ms": 15.0,
        },
        "escape": {"before": {"frame": start_frame, "object_count": objects}},
        "state_end": {"frame": end_frame},
    }


class AttributionTest(unittest.TestCase):
    def test_recursion_counted_once_and_main_thread_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "round04.sample.txt"
            path.write_text(SAMPLE)
            counts = attribution.inclusive_counts(attribution.main_thread_nodes(path))
        self.assertEqual(counts["Pathfinder::processPathfindQueue()"], 500)
        self.assertEqual(counts["GameLogic::update()"], 600)
        self.assertNotIn("worker", counts)

    def test_logic_rate_and_profile_milliseconds(self):
        run = {"rounds": [
            round_entry(3, 1000, 1010, 800, 0, 0, 0),
            round_entry(4, 1610, 1620, 900, 20, 20.0, 25.0),  # 600 logic frames in 20 s
            round_entry(5, 2120, 2130, 950, 20, 25.0, 20.0),
        ]}
        windows = attribution.window_rows(run)
        self.assertEqual([window["round"] for window in windows], [4, 5])
        self.assertAlmostEqual(windows[0]["logic_per_s"], 30.0)
        self.assertAlmostEqual(windows[1]["logic_per_s"], 20.0)
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            (run_dir / "run-logs").mkdir()
            (run_dir / "run-logs" / "round04.sample.txt").write_text(SAMPLE)
            (run_dir / "run.json").write_text(json.dumps(run))
            profiles = attribution.profile_rows(run_dir, windows)
        # Mean frame = average of the bracketing windows' 40 ms and 50 ms.
        self.assertAlmostEqual(profiles[0]["mean_frame_ms"], 45.0)
        logic_ms = profiles[0]["shares"]["logic"] * profiles[0]["mean_frame_ms"]
        self.assertAlmostEqual(logic_ms, 27.0)


if __name__ == "__main__":
    unittest.main()
