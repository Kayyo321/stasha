"""Inventory checks; fail-closed compilation/execution is tested in test_access_runner."""
import unittest
import run_pointer_qualifiers as qualifiers


class QualifierInventoryTests(unittest.TestCase):
    def test_bounded_inventory(self):
        cases = qualifiers.cases()
        self.assertEqual(len(cases), 29)
        self.assertEqual(len({c.name for c in cases}), len(cases))
        self.assertEqual(sum(c.outcome == 'rejection' for c in cases), 14)
        self.assertEqual(sum(c.outcome == 'success' for c in cases), 15)
        self.assertTrue(all(c.diagnostic for c in cases if c.outcome == 'rejection'))

    def test_negative_and_positive_controls(self):
        cases = {c.name: c for c in qualifiers.cases()}
        for name in ('explicit_cast_widen', 'inner_covariance', 'inner_covariance_cast',
                     'deep_covariance_writable_ancestor', 'unsafe_does_not_leak',
                     'slice_addr_writeonly_container', 'slice_descriptor_readonly'):
            self.assertEqual(cases[name].outcome, 'rejection')
        for name in ('readonly_covariance', 'nested_identity', 'outer_readonly_inner_writable',
                     'unsafe_cast_widen', 'slice_addr_readonly_container'):
            self.assertEqual(cases[name].outcome, 'success')
        self.assertIn('type View: []i32;', cases['slice_descriptor_readonly'].source)
        self.assertIn('View *rw', cases['slice_descriptor_readonly'].source)


if __name__ == '__main__':
    unittest.main()
