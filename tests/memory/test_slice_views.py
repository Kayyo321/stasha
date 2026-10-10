#!/usr/bin/env python3
"""Manifest regressions; compiler qualification is run_slice_views.py."""
import unittest

import run_slice_views as views


class SliceViewManifestTests(unittest.TestCase):
    def test_bounded_manifest(self):
        cases = views.cases()
        self.assertEqual(len(cases), 77)
        self.assertEqual(len({case.name for case in cases}), 77)
        self.assertEqual(sum(case.outcome == 'success' for case in cases), 54)
        self.assertEqual(sum(case.outcome == 'trap' for case in cases), 20)
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
                self.assertEqual(case.reason, 'bounds')

    def test_capacity_and_unsafe_do_not_expand_readable_extent(self):
        by_name = {case.name: case for case in views.cases()}
        for name in ('expand_to_capacity', 'empty_at_capacity', 'unsafe_range_suppressed'):
            with self.subTest(name=name):
                self.assertEqual(by_name[name].outcome, 'trap')
                self.assertEqual(by_name[name].reason, 'bounds')
        empty = by_name['empty_at_logical_end']
        self.assertEqual(empty.outcome, 'success')
        self.assertIn('view = a[0:1]', empty.source)
        self.assertIn('empty = view[1:1]', empty.source)
        self.assertIn('len.(empty) != 0 || cap.(empty) != 1', empty.source)

    def test_raw_pointer_view_records_explicit_unsafe_interval(self):
        case = next(case for case in views.cases() if case.name == 'raw_pointer_typed_width')
        self.assertEqual(case.outcome, 'success')
        self.assertIn('unsafe { stack []i32 view = ptr[1:2];', case.source)
        self.assertIn('len.(view) != 1 || cap.(view) != 1', case.source)
        self.assertIn('view[0] += 1000;', case.source)
        self.assertIn('a[0] != 4660 || a[1] != 10000 || a[2] != 11000', case.source)

    def test_unsigned_range_and_expansion_controls(self):
        names = {case.name for case in views.cases()}
        self.assertTrue({'unsigned_narrow_bounds', 'wide_lo_before_narrowing',
                         'wide_hi_before_narrowing', 'max_unsigned_bound',
                         'expand_to_capacity', 'empty_at_capacity', 'empty_at_logical_end',
                         'unsafe_range_suppressed'} <= names)


if __name__ == '__main__':
    unittest.main()
