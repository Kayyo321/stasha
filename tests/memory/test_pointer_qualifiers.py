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

    def test_unchecked_address_requires_explicit_unsafe_scope(self):
        case = next(case for case in qualifiers.cases()
                    if case.name == 'slice_addr_readonly_container_unchecked')
        self.assertEqual(case.outcome, 'success')
        self.assertIn('stack Box *r p = &b;', case.source)
        self.assertIn('unsafe { stack i32 *rw q = &p.view[unchecked: 0];', case.source)
        self.assertIn('q[0] = 9; } if a[0] != 9', case.source)

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
