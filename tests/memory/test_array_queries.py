#!/usr/bin/env python3
"""Runner safety, manifest and fail-closed IR oracle tests (no compiler needed)."""
from dataclasses import FrozenInstanceError
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import run_array_queries as query


class ManifestTests(unittest.TestCase):
    def test_manifest_counts_and_names(self):
        cases = query.cases()
        self.assertEqual(len(cases), 156)
        self.assertEqual(len({c.name for c in cases}), 156)
        self.assertEqual(dict(query.Counter(c.outcome for c in cases)), {
            'runtime': 41, 'rejection': 44, 'boundary_ir': 9, 'legacy_ir': 14, 'bounds_ir': 48})

    def test_execution_capabilities_are_disjoint_and_frozen(self):
        for case in query.cases():
            self.assertIs(type(case), query.RuntimeCase if case.outcome == 'runtime' else query.CompileCase)
            with self.assertRaises(FrozenInstanceError):
                case.outcome = 'runtime'
        with self.assertRaises(ValueError):
            query.CompileCase('bad', '', 'runtime')
        with self.assertRaises(ValueError):
            query.CompileCase('bad', '', 'rejection')

    def test_complete_independent_bounds_matrix(self):
        by_name = {c.name: c for c in query.compile_cases()}
        for label in ('negative', 'upper', 'wide'):
            for form in ('variable', 'literal'):
                for mode in ('unsafe', 'checked'):
                    for operation in ('read', 'store', 'compound'):
                        case = by_name['_'.join(('bounds', label, form, mode, operation))]
                        self.assertEqual(case.outcome, 'bounds_ir')
                        self.assertEqual(case.ir_checks, (('panic', 2, form == 'variable'),))
        for op in ('len', 'cap'):
            for name in ('huge_extent', 'static_self_direct', 'static_self_subarray', 'static_self_nested',
                         'writeonly_owner', 'element_alias', 'slice_alias', 'array_parameter_call',
                         'readonly_owner_mutation', 'interface_wrong_writer'):
                self.assertEqual(by_name[name + '_' + op].outcome, 'rejection')
            for name in ('array_owner_field', 'array_owner_legacy_len', 'array_owner_pointer_field',
                         'array_owner_partial_index', 'array_owner_nested', 'array_owner_nested_partial'):
                self.assertEqual(by_name[name + '_' + op].ir_checks, (('return', 'main', 0),))

    def test_queries_are_independently_asserted(self):
        for case in query.runtime_cases():
            with self.subTest(case=case.name):
                self.assertRegex(case.source, r'if\s*\(\s*len\.\(')
                self.assertRegex(case.source, r'if\s*\(\s*cap\.\(')
                self.assertNotIn('print.', case.source)
        direct = next(c.source for c in query.runtime_cases() if c.name == 'direct')
        self.assertLess(direct.index('if (len.(a) != 2)'), direct.index('a[len.(a)-1]'))
        self.assertLess(direct.index('if (cap.(a) != 2)'), direct.index('a[len.(a)-1]'))

    def test_historical_negative_not_reused(self):
        by_name = {c.name: c for c in query.cases()}
        self.assertIn('a[len.(a)-3]', by_name['query_dependent_len'].source)
        self.assertIn('a[cap.(a)]', by_name['query_dependent_cap'].source)
        self.assertTrue(all('len.(a)-1' not in c.source for c in query.compile_cases() if not c.name.startswith('interface_inline_query_index')))
        for op in ('len', 'cap'):
            self.assertEqual(by_name['interface_inline_query_index_' + op].outcome, 'rejection')
            self.assertIn('stack i32 last=' + op + '.(a)-1;', by_name['interface_last_element_' + op].source)

    def test_list_does_not_require_compiler(self):
        with patch('builtins.print') as printed:
            self.assertEqual(query.main(['--list']), 0)
        self.assertIn('can_execute', printed.call_args.args[0])


class DispatchSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.args = SimpleNamespace(compiler=Path('/trusted/compiler'), compile_timeout=1, run_timeout=1)
        self.calls = []

    def tearDown(self):
        self.temp.cleanup()

    def accepted_ir(self, command, cwd, prefix, timeout, env=None):
        command = [str(p) for p in command]
        self.calls.append(command)
        self.assertEqual(command[1], 'ir', 'compile-only case reached build or native execution')
        output = Path(command[command.index('-o') + 1])
        output.write_text('define i32 @main() {\nentry:\n  ret i32 0\n}\n')
        return dict(category='exit', returncode=0, stdout='', stderr='', command=command)

    def test_every_compile_only_fixture_stays_ir_only_even_if_accepted(self):
        with patch.object(query, 'capture', side_effect=self.accepted_ir):
            with patch.object(query, 'run_runtime_case', side_effect=AssertionError('must not execute')):
                for case in query.compile_cases():
                    result = query.run_case(case, 0, self.args, self.root / case.name)
                    self.assertTrue(result['never_executed'])
                    self.assertNotIn('run', result)
                    if case.outcome == 'rejection': self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(len(self.calls), len(query.compile_cases()))

    def test_runtime_entry_rejects_compile_cases_before_tools(self):
        with patch.object(query, 'capture') as cap:
            for case in query.compile_cases():
                with self.assertRaises(TypeError):
                    query.run_runtime_case(case, 0, self.args, self.root / case.name)
            cap.assert_not_called()

    def test_compile_entry_rejects_runtime_case(self):
        with self.assertRaises(TypeError):
            query.run_compile_case(query.runtime_cases()[0], 0, self.args, self.root / 'wrong')

    def test_unknown_fixture_cannot_execute(self):
        fake = SimpleNamespace(name='fake', source='', outcome='runtime')
        with patch.object(query, 'capture') as cap:
            with self.assertRaises(TypeError): query.run_case(fake, 0, self.args, self.root / 'fake')
            cap.assert_not_called()

    def test_stale_work_directory_is_rejected(self):
        with self.assertRaises(FileExistsError):
            query.start_receipt(query.runtime_cases()[0], 0, self.root)

    def test_runtime_missing_binary_does_not_execute(self):
        case = query.runtime_cases()[0]
        with patch.object(query, 'capture', return_value=dict(category='exit', returncode=0, stdout='', stderr='')) as cap:
            result = query.run_case(case, 0, self.args, self.root / 'missing')
        self.assertEqual(cap.call_count, 1)
        self.assertEqual(result['status'], 'FAIL')
        self.assertNotIn('run', result)

    def test_exact_empty_runtime_outputs(self):
        case = query.RuntimeCase('simple', query.program('simple', 'ret 0;'))
        def fake_capture(command, cwd, prefix, timeout, env=None):
            command = [str(v) for v in command]
            if len(command) > 1:
                binary = Path(command[command.index('-o')+1]); binary.write_text('stub'); binary.chmod(0o700)
                Path(env['STS_DUMP_IR']).write_text('define i32 @main() {\nentry:\nret i32 0\n}\n')
                return dict(category='exit', returncode=0, stdout='', stderr='')
            return dict(category='exit', returncode=0, stdout='\n', stderr='')
        with patch.object(query, 'capture', side_effect=fake_capture):
            result = query.run_case(case, 0, self.args, self.root / 'nonempty')
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('runtime stdout is not exactly empty', result['problems'])


class IrOracleTests(unittest.TestCase):
    def valid_guard(self):
        return '''entry:
  %wide = sext i32 %index to i64
  %bad = icmp uge i64 %wide, 2
  br i1 %bad, label %panic, label %ok
panic:
  call void @__stasha_panic(i32 6)
  unreachable
ok:
  %address = getelementptr [2 x i32], ptr %a, i32 0, i64 %wide
  %value = load i32, ptr %address
  ret i32 %value
'''

    def test_accepts_guarded_full_width_cfg(self):
        self.assertEqual(query.panic_guard_problems(self.valid_guard(), 2, True, 0), [])

    def test_rejects_wrong_extent_width_predicate_reason_or_edge(self):
        for old, new in [('i64 %wide, 2', 'i64 %wide, 3'), ('icmp uge i64', 'icmp uge i32'),
                         ('icmp uge', 'icmp sge'), ('i32 6)', 'i32 5)'),
                         ('label %panic, label %ok', 'label %ok, label %panic'),
                         ('  unreachable', '  ret i32 0')]:
            with self.subTest(change=(old, new)):
                self.assertTrue(query.panic_guard_problems(self.valid_guard().replace(old, new), 2, True, 0))

    def test_panic_substring_without_edge_is_not_sufficient(self):
        body = self.valid_guard().replace('  br i1 %bad, label %panic, label %ok', '  br label %ok')
        self.assertTrue(query.panic_guard_problems(body, 2, True, 0))

    def test_protected_access_cannot_precede_guard(self):
        body = self.valid_guard().replace('  %wide =', '  %early = getelementptr [2 x i32], ptr %a, i32 0, i64 2\n  %wide =')
        self.assertTrue(query.panic_guard_problems(body, 2, True, 0))

    def test_optimized_invalid_body_must_be_nonreturning(self):
        body = 'entry:\n  call void @__stasha_panic(i32 6)\n  unreachable\n'
        self.assertEqual(query.panic_guard_problems(body, 2, False, 3), [])
        self.assertTrue(query.panic_guard_problems('entry:\n  br i1 %choose, label %panic, label %wrong\npanic:\n  call void @__stasha_panic(i32 6)\n  unreachable\nwrong:\nret i32 0\n', 2, False, 3))

    def test_disconnected_panic_does_not_qualify(self):
        body = 'entry:\n  unreachable\nelsewhere:\n  call void @__stasha_panic(i32 6)\n  unreachable\n'
        self.assertTrue(query.panic_guard_problems(body, 2, False, 3))
        body = 'entry:\n  br label %loop\nloop:\n  br label %loop\nelsewhere:\n  call void @__stasha_panic(i32 6)\n  unreachable\n'
        self.assertTrue(query.panic_guard_problems(body, 2, False, 3))

    def test_success_edge_must_dominate_access(self):
        body = self.valid_guard().replace('entry:', 'guard:')
        body = 'entry:\n  br i1 %choose, label %guard, label %ok\n' + body
        self.assertTrue(query.panic_guard_problems(body, 2, True, 0))

    def test_compound_initializer_gep_is_not_protected_access(self):
        body = self.valid_guard().replace('  %wide =', '  %init = getelementptr [2 x i32], ptr %compound_arr, i32 0, i32 0\n  %wide =')
        self.assertEqual(query.panic_guard_problems(body, 2, True, 0), [])

    def test_guard_must_check_the_selected_index(self):
        body = self.valid_guard().replace('ptr %a, i32 0, i64 %wide', 'ptr %a, i32 0, i64 %other')
        self.assertTrue(query.panic_guard_problems(body, 2, True, 0))
        # A folded true branch cannot certify an unrelated unknown index.
        body = body.replace('br i1 %bad', 'br i1 true')
        self.assertTrue(query.panic_guard_problems(body, 2, False, 0))

    def test_folded_guard_requires_an_independently_invalid_constant(self):
        body = self.valid_guard().replace('br i1 %bad', 'br i1 true')
        for value in ('-1', '2', '18446744073709551615'):
            selected = body.replace('ptr %a, i32 0, i64 %wide', 'ptr %a, i32 0, i64 ' + value)
            self.assertEqual(query.panic_guard_problems(selected, 2, False, 0), [])
        for value in ('0', '1'):
            selected = body.replace('ptr %a, i32 0, i64 %wide', 'ptr %a, i32 0, i64 ' + value)
            self.assertTrue(query.panic_guard_problems(selected, 2, False, 0))

    def test_every_feasible_return_matches_expected_value(self):
        body = 'entry:\n  br i1 %choose, label %good, label %bad\ngood:\n  ret i32 2147483647\nbad:\n  ret i32 0\n'
        for function in ('main', 'query'):
            case = query.CompileCase('max', '', 'boundary_ir', ir_checks=(('return', function, 2147483647),))
            ir = 'define i32 @' + function + '() {\n' + body + '}\n'
            if function != 'main': ir += 'define i32 @main() {\nentry:\nret i32 0\n}\n'
            with self.subTest(function=function):
                self.assertTrue(query.inspect_ir(case, ir, 0))
                self.assertFalse(query.inspect_ir(case, ir.replace('ret i32 0', 'ret i32 2147483647'), 0))

    def test_return_oracle_ignores_only_proven_infeasible_returns(self):
        body = 'entry:\n  br i1 true, label %good, label %bad\ngood:\n  ret i32 3\nbad:\n  ret i32 0\n'
        self.assertEqual(query.exact_return_problems(body, 3), [])
        self.assertTrue(query.exact_return_problems(body.replace('br i1 true', 'br i1 false'), 3))
        self.assertTrue(query.exact_return_problems('entry:\n  unreachable\nelsewhere:\n  ret i32 3\n', 3))
        self.assertTrue(query.exact_return_problems('entry:\n  ret i32 %unknown\n', 3))

    def test_boundary_return_is_function_specific(self):
        case = query.CompileCase('max', '', 'boundary_ir', ir_checks=(('return', 'query', 2147483647),))
        ir = 'define i32 @main() {\nentry:\nret i32 2147483647\n}\ndefine i32 @query() {\nentry:\nret i32 0\n}\n'
        self.assertTrue(query.inspect_ir(case, ir, 0))
        self.assertFalse(query.inspect_ir(case, ir.replace('ret i32 0', 'ret i32 2147483647'), 0))

    def test_zero_legacy_cannot_silently_gain_field_extent(self):
        case = query.CompileCase('legacy', '', 'legacy_ir', ir_checks=(('return', 'main', 0),))
        self.assertTrue(query.inspect_ir(case, 'define i32 @main() {\nentry:\nret i32 3\n}\n', 0))

    def test_source_span_and_owned_diagnostic_required(self):
        result = dict(category='exit', returncode=1, stderr='error: unrelated\n', stdout='')
        self.assertTrue(query.diagnostic_problems(result, Path('x.sts'), 'fixed-array len extent exceeds INT32_MAX'))
        result['stderr'] = 'error: fixed-array len extent exceeds INT32_MAX\n --> x.sts:2:3\n'
        self.assertEqual(query.diagnostic_problems(result, Path('x.sts'), 'fixed-array len extent exceeds INT32_MAX'), [])
        result['stderr'] += 'warning: surprise\n'
        self.assertTrue(query.diagnostic_problems(result, Path('x.sts'), 'fixed-array len extent exceeds INT32_MAX'))


if __name__ == '__main__':
    unittest.main()
