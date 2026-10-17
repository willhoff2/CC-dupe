#!/usr/bin/env python3
"""Unit tests for macos-late-game-replay-summary.py's window arithmetic and rule evaluation.

    python3 scripts/macos-late-game-replay-summary-test.py
"""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("macos-late-game-replay-summary.py")
SPEC = importlib.util.spec_from_file_location("summary", SCRIPT)
summary = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summary)


def write_headless_run(directory, last_frame, ms_per_frame, mismatch=False):
    """A headless run whose frames each take ms_per_frame, back to back from frame 0."""
    directory.mkdir(parents=True)
    rows = ["frame,start_ms,logic_ms,ai_ms,objects"]
    # Frame 0 repeats during map load; only its last row may count.
    rows.append("0,0.000,500.000,0.000,10")
    for frame in range(0, last_frame + 1):
        rows.append(f"{frame},{1000 + frame * ms_per_frame:.3f},{ms_per_frame:.3f},{ms_per_frame / 2:.3f},100")
    (directory / "logic.csv").write_text("\n".join(rows) + "\n")
    record = {"crc_mismatch_lines": ["CRC Mismatch in Frame 700"] if mismatch else [],
              "elapsed_lines": [], "returncode": 0}
    (directory / "run.json").write_text(json.dumps(record))
    return directory


class HeadlessTest(unittest.TestCase):
    def test_window_wall_time_runs_from_frame_18000_to_the_end_of_the_last_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = summary.headless_summary(write_headless_run(Path(tmp) / "a", 18000 + 99, 10.0))
            self.assertAlmostEqual(run["window_wall_s"], 100 * 10.0 / 1000.0)
            self.assertEqual(run["last_frame"], 18099)
            self.assertAlmostEqual(run["per_minute"][0]["logic_ms_mean"], 10.0)

    def test_a_desynced_run_has_no_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = summary.headless_summary(write_headless_run(Path(tmp) / "b", 701, 1.0, mismatch=True))
            self.assertIsNone(run["window_wall_s"])
            self.assertIsNone(run["window_logic_ms_mean"])


class RulesTest(unittest.TestCase):
    def runs(self, tmp, b_ms, b_mismatch=False, b_last=21000):
        headless = {}
        for recording, a_ms in (("before-2", 30.0), ("aftersync-1", 70.0)):
            headless[("A", recording)] = [summary.headless_summary(
                write_headless_run(Path(tmp) / f"A-{recording}", 21000, a_ms))]
            headless[("B", recording)] = [summary.headless_summary(
                write_headless_run(Path(tmp) / f"B-{recording}", b_last, b_ms * a_ms / 30.0, b_mismatch))]
        return headless

    def test_h1_headless_threshold_is_inclusive_at_0_6(self):
        with tempfile.TemporaryDirectory() as tmp:
            outcome = summary.evaluate_rules(self.runs(tmp, 18.0), {})
            self.assertTrue(outcome["B_valid"])
            self.assertAlmostEqual(outcome["H1_headless"]["before-2"]["ratio"], 0.6)
            self.assertTrue(outcome["H1"].startswith("headless part met"))

    def test_mismatch_or_short_playback_invalidates_b(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(summary.evaluate_rules(self.runs(tmp, 10.0, b_mismatch=True), {})["B_valid"])
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(summary.evaluate_rules(self.runs(tmp, 10.0, b_last=20000), {})["B_valid"])

    def test_heavier_game_compares_a_over_the_common_minutes(self):
        with tempfile.TemporaryDirectory() as tmp:
            heavier = summary.evaluate_rules(self.runs(tmp, 10.0), {})["heavier_game"]
            self.assertAlmostEqual(heavier["ratio"], 70.0 / 30.0)
            self.assertEqual(heavier["verdict"], "CONFIRMED")


class RenderedTest(unittest.TestCase):
    def test_late_window_and_backend_alignment(self):
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            engine = ["logic_frame,start_ms,client_ms,logic_ms"]
            frames = ["frame,interval_ms,scene_ms,acquire_ms,present_ms,target_w,target_h,swapchain_w,"
                      "swapchain_h,points_w,points_h,scale"]
            for index, frame in enumerate(range(21590, 21620)):
                engine.append(f"{frame},{5000 + index * 40.0:.3f},25.000,15.000")
                frames.append(f"{index},{40.0 if index else 0.0:.3f},20.000,0.1,{3.0 if frame >= 21600 else 9.0},"
                              "1,1,1,1,1,1,1")
            (run_dir / "engine.csv").write_text("\n".join(engine) + "\n")
            (run_dir / "frames.csv").write_text("\n".join(frames) + "\n")
            (run_dir / "run.json").write_text(json.dumps({"crc_mismatch_lines": [], "ended_by": "x"}))
            run = summary.rendered_summary(run_dir, 21615)
            self.assertEqual(run["late_passes"], 15)
            self.assertAlmostEqual(run["frame_ms_median"], 40.0)
            self.assertAlmostEqual(run["logic_ms_median"], 15.0)
            self.assertAlmostEqual(run["present_ms_median"], 3.0)


if __name__ == "__main__":
    unittest.main()
