"""Deterministic lifecycle checks, separate from authentic capture verification."""

import importlib.util
from pathlib import Path
import unittest


SPEC = importlib.util.spec_from_file_location(
    "diagsession_fixtures", Path(__file__).resolve().parents[1] / "scripts/diagsession_fixtures.py"
)
FIXTURES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURES)


class NativeLifetimeProofTest(unittest.TestCase):
    def test_release_and_pointer_reuse_preserve_intentionally_retained_allocation(self):
        active = {}
        # The caller deliberately retains address 0x1000 in a static container.
        events = [
            (10, 1, 2, 33, (3, 64, 0x1000, 3)),
            (20, 1, 2, 33, (3, 256, 0x2000, 3)),
            (30, 1, 2, 36, (3, 0x2000, 3)),
            (40, 1, 2, 33, (3, 32, 0x2000, 3)),
        ]
        for event in events:
            FIXTURES.apply_heap_event(active, event)
        self.assertEqual(active, {(1, 3, 0x1000): (10, 64, 2), (1, 3, 0x2000): (40, 32, 2)})
        FIXTURES.apply_heap_event(active, (50, 1, 2, 36, (3, 0x2000, 3)))
        self.assertEqual(active, {(1, 3, 0x1000): (10, 64, 2)})

    def test_matching_addresses_in_different_processes_and_heaps_stay_distinct(self):
        active = {}
        for pid, heap, size in ((1, 3, 64), (2, 3, 128), (1, 4, 256)):
            FIXTURES.apply_heap_event(active, (10, pid, 5, 33, (heap, size, 0x1000, 3)))
        FIXTURES.apply_heap_event(active, (20, 1, 5, 36, (3, 0x1000, 3)))
        self.assertEqual(active, {(2, 3, 0x1000): (10, 128, 5), (1, 4, 0x1000): (10, 256, 5)})

    def test_realloc_summary_preserves_allocating_timestamp_and_thread(self):
        active = {}
        for event in (
            (10, 1, 2, 33, (3, 64, 0x1000, 3)),
            (20, 1, 2, 33, (3, 128, 0x2000, 3)),
            (21, 1, 2, 36, (3, 0x1000, 3)),
            (22, 1, 9, 34, (3, 0x2000, 0x1000, 128, 64, 3)),
        ):
            FIXTURES.apply_heap_event(active, event)
        self.assertEqual(active, {(1, 3, 0x2000): (20, 128, 2)})

    def test_unknown_free_rejects_an_incomplete_fixture(self):
        with self.assertRaises(KeyError):
            FIXTURES.apply_heap_event({}, (10, 1, 2, 36, (3, 0x1000, 3)))


if __name__ == "__main__":
    unittest.main()
