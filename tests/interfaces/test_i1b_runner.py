#!/usr/bin/env python3
"""Static I1b inventory/oracle tests plus shared fail-closed runner probes."""
import json, unittest
from pathlib import Path
from i1b_cases import matrix
from run_i0 import inventory_issues
from test_i0_runner import RunnerContracts
class I1bInventory(unittest.TestCase):
    def test_exact_static_inventory(self):
        expected=json.loads(Path(__file__).with_name('i1b_inventory.json').read_text())
        self.assertEqual(inventory_issues([x.name for x in matrix()],expected['cases']),[])
        self.assertEqual(expected['stage'],'I1b.1')
    def test_each_rule_has_both_slot_routes(self):
        names={x.name for x in matrix()}
        for name in names:
            if name.startswith('dynamic-'):self.assertIn(name.replace('dynamic-','qualified-',1),names)
    def test_all_negatives_have_semantic_pattern(self):
        negatives=[x for x in matrix() if not x.positive]
        self.assertGreater(len(negatives),40)
        self.assertTrue(all(x.pattern and ('interface' in x.pattern or 'C ABI' in x.pattern) for x in negatives))
    def test_positive_controls_are_not_empty(self):self.assertGreater(sum(x.positive for x in matrix()),50)
if __name__=='__main__':unittest.main(verbosity=2)
