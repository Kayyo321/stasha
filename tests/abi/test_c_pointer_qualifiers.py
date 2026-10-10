import unittest
from run_c_pointer_qualifiers import process_problems


class ProcessGatesTests(unittest.TestCase):
    def process(self, **kwargs):
        return dict(dict(category='exit', returncode=0, stdout='', stderr=''), **kwargs)

    def test_clean_success(self):
        self.assertEqual(process_problems(self.process()), [])

    def test_rejection_requires_exact_normal_exit(self):
        self.assertEqual(process_problems(self.process(returncode=1, stderr='error: permission'), 1), [])
        for category, code in [('signal', -11), ('timeout', 1), ('exit', 0), ('launch-error', None)]:
            self.assertTrue(process_problems(self.process(category=category, returncode=code), 1))

    def test_internal_failures_do_not_count_as_rejections(self):
        for text in ['LLVM IR verification failed', 'AddressSanitizer', 'Segmentation fault']:
            self.assertTrue(process_problems(self.process(returncode=1, stderr=text), 1))

    def test_warnings_and_errors_do_not_count_as_success(self):
        for text in ['warning: unused', 'oracle.c:3: warning: mismatch', 'error: failure']:
            self.assertTrue(process_problems(self.process(stderr=text)))


if __name__ == '__main__':
    unittest.main()
