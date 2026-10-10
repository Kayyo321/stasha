"""Positive inventory and typed-stride oracle checks."""
from pathlib import Path
import tempfile
import unittest
import run_pointer_arithmetic as arithmetic


class PointerArithmeticTests(unittest.TestCase):
    def test_positive_bounded_inventory(self):
        cases = arithmetic.cases()
        self.assertEqual(len(cases), 24)
        self.assertEqual(len({c.name for c in cases}), len(cases))
        self.assertTrue(all(c.outcome == 'success' for c in cases))
        names = {c.name for c in cases}
        for name in ('addition_i8', 'addition_i64', 'addition_f64', 'addition_signed_negative',
                     'subtraction_unsigned_offset', 'field_pointer', 'call_pointer',
                     'alias_pointee', 'pointer_slot_pointee', 'struct_pointee', 'operands_once',
                     'void_byte_compatibility', 'derived_store', 'derived_compound'):
            self.assertIn(name, names)

    def test_pointer_slot_case_does_not_dereference_bad_stride(self):
        case = next(c for c in arithmetic.cases() if c.name == 'pointer_slot_pointee')
        self.assertIn('if p + 1 != &a[1]', case.source)

    def test_ir_oracle_rejects_byte_stride_for_i32(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'pre.ll').write_text('%ptrarith = getelementptr i8, ptr %p, i64 1\n')
            result = dict(name='addition_i32', status='PASS', problems=[])
            arithmetic.check_stride(result, work)
            self.assertEqual(result['status'], 'FAIL')
            self.assertEqual(len(result['problems']), 1)

    def test_ir_oracle_accepts_typed_stride(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'pre.ll').write_text('%ptrarith12 = getelementptr i32, ptr %p, i64 1\n')
            result = dict(name='addition_i32', status='PASS', problems=[])
            arithmetic.check_stride(result, work)
            self.assertEqual(result['status'], 'PASS')
            self.assertEqual(result['problems'], [])


if __name__ == '__main__':
    unittest.main()
