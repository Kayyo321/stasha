#!/usr/bin/env python3
"""Static manifest checks; runtime/compiler evidence comes from the A1 runners."""
import unittest
import run_bounds_a1 as bounds

class BoundsA1ManifestTests(unittest.TestCase):
    def test_manifest_is_bounded_and_unique(self):
        cases=bounds.cases()
        self.assertEqual(len(cases),83)
        self.assertEqual(len({c.name for c in cases}),83)
        self.assertEqual(sum(c.outcome=='rejection' for c in cases),9)
        self.assertTrue(all(c.reason=='bounds' for c in cases if c.outcome=='trap'))
    def test_wide_indices_never_dereference(self):
        for c in bounds.cases():
            if c.name.startswith('wide_'):
                self.assertIn(' *rw _p=&',c.source)
                self.assertNotIn('_p[',c.source)
    def test_diagnostics_stay_compile_only(self):
        cases={c.name:c for c in bounds.cases()}
        for name in ('oversized_array_view','indexed_update_inc','indexed_update_dec',
                     'unchecked_outside_unsafe','raw_requires_unsafe','raw_requires_hi',
                     'raw_void_element','function_view_safe','function_view_unsafe'):
            self.assertEqual(cases[name].outcome,'rejection')
            self.assertTrue(cases[name].diagnostic)
    def test_once_order_and_shape_failures(self):
        cases={c.name:c for c in bounds.cases()}
        for name,trace in [('shape_checks_after_index',12),('shape_checks_after_range_operands',123),
                           ('plain_assignment_rhs_first',312),('compound_target_first',12)]:
            self.assertIn('bounds_expect('+str(trace)+')',cases[name].source)
            self.assertEqual(cases[name].outcome,'trap')
        for operation in ('read','address','store','compound','range'):
            self.assertEqual(cases['once_order_'+operation].outcome,'success')
    def test_support_rejects_wrong_reason_or_trace(self):
        self.assertIn('reason != 6',bounds.SUPPORT)
        self.assertIn('trace != expected',bounds.SUPPORT)
        self.assertIn('_Exit(87)',bounds.SUPPORT)
        self.assertIn('STASHA_TRAP:bounds',bounds.SUPPORT)
if __name__=='__main__':unittest.main()
