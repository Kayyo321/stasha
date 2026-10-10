import unittest
from run_language_tests import evaluate_runtime
class LanguageRuntimeOracle(unittest.TestCase):
    def result(self, text, exit=0, category='success'):
        return {'stdout':text,'stderr':'','returncode':exit,'category':category}
    def test_real_pass(self): self.assertEqual(evaluate_runtime(self.result('test A ... PASS\n3 passed, 0 failed\n')), [])
    def test_compile_only(self): self.assertTrue(evaluate_runtime(self.result('compiled successfully\n')))
    def test_empty(self): self.assertTrue(evaluate_runtime(self.result('0 passed, 0 failed\n')))
    def test_failure(self): self.assertTrue(evaluate_runtime(self.result('2 passed, 1 failed\n')))
    def test_exit(self): self.assertTrue(evaluate_runtime(self.result('3 passed, 0 failed\n', 1)))
    def test_timeout(self): self.assertTrue(evaluate_runtime(self.result('3 passed, 0 failed\n', 0, 'timeout')))
    def test_duplicate(self): self.assertTrue(evaluate_runtime(self.result('3 passed, 0 failed\n3 passed, 0 failed\n')))
    def test_sanitizer(self): self.assertTrue(evaluate_runtime(self.result('runtime error: bad\n3 passed, 0 failed\n')))
