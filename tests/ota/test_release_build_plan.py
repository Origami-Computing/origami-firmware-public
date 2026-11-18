import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from plan_release_candidate import plan


class PlanTests(unittest.TestCase):
    def test_push_requires_new_version(self):
        self.assertIsNone(plan('push', 'v1.0.12', 'v1.0.12'))
        self.assertEqual(plan('push', 'v1.0.13', 'v1.0.12', run_id='123', attempt='2'),
                         'v1.0.13-public-review.123002')
        with self.assertRaises(ValueError): plan('push', 'v1.0.11', 'v1.0.12')

    def test_dispatch_matches_version(self):
        tag='v1.0.13-public-review.1'
        self.assertEqual(plan('workflow_dispatch', 'v1.0.13', requested=tag), tag)
        with self.assertRaises(ValueError): plan('workflow_dispatch', 'v1.0.12', requested=tag)
        with self.assertRaises(ValueError): plan('pull_request', 'v1.0.13', 'v1.0.12')
