#!/usr/bin/env python3
"""Manifest regressions; compiler qualification is run_slice_views.py."""
import unittest

import run_slice_views as views


class SliceViewManifestTests(unittest.TestCase):
    def test_bounded_manifest(self):
        cases = views.cases()
        self.assertEqual(len(cases), 76)
        self.assertEqual(len({case.name for case in cases}), 76)
        self.assertEqual(sum(case.outcome == 'success' for case in cases), 56)
        self.assertEqual(sum(case.outcome == 'trap' for case in cases), 17)
        self.assertEqual(sum(case.outcome == 'rejection' for case in cases), 3)

    def test_source_width_and_access_matrix(self):
        by_name = {case.name: case for case in views.cases()}
        for path in ('member', 'call', 'index'):
            for typ in ('i16', 'i32', 'i64', 'f64'):
                for operation in ('read', 'store', 'compound'):
                    case = by_name['_'.join(('reslice', path, typ, operation))]
                    self.assertEqual(case.outcome, 'success')
                    self.assertIn('[lo++:hi++][index++]', case.source)
                    self.assertIn('calls !=', case.source)
                    self.assertIn('select !=', case.source)

    def test_array_member_storage_matrix(self):
        by_name = {case.name: case for case in views.cases()}
        for typ in ('i16', 'i32', 'i64', 'f64'):
            for operation in ('read', 'store', 'compound'):
                case = by_name['_'.join(('array_member', typ, operation))]
                self.assertEqual(case.outcome, 'success')
                self.assertIn('box.values[lo++:hi++][index++]', case.source)
                self.assertIn('len.(tail) != 2 || cap.(tail) != 2', case.source)

    def test_bad_ranges_are_not_dereferenced(self):
        for case in views.cases():
            if case.outcome == 'trap':
                self.assertIn('_invalid =', case.source)
                self.assertNotIn('_invalid[', case.source)
                self.assertEqual(case.reason, 'slice range')

    def test_unsigned_range_and_expansion_controls(self):
        names = {case.name for case in views.cases()}
        self.assertTrue({'unsigned_narrow_bounds', 'wide_lo_before_narrowing',
                         'wide_hi_before_narrowing', 'max_unsigned_bound',
                         'expand_to_capacity', 'empty_at_capacity',
                         'unsafe_range_suppressed'} <= names)


if __name__ == '__main__':
    unittest.main()
