#!/usr/bin/env python3
"""Complete AST inventory and exact compile-only runner failure contracts."""
import json,re,unittest
from pathlib import Path
from i1b2_cases import matrix
from run_i0 import inventory_issues
from test_i0_runner import RunnerContracts
ROOT=Path(__file__).resolve().parents[2]
class LocalInventory(unittest.TestCase):
    def test_cases_exact(self):
        expected=json.loads(Path(__file__).with_name('i1b2_inventory.json').read_text())
        self.assertEqual(inventory_issues([c.name for c in matrix()],expected['cases']),[])
    def test_ast_exhaustive(self):
        header=(ROOT/'src/ast/ast.h').read_text();enum=header[header.index('    NodeModule,'):header.index('} node_kind_t;')]
        kinds=set(re.findall(r'\bNode\w+',enum));self.assertEqual(len(kinds),107)
        source=(ROOT/'src/codegen/cg_iface_locals.c').read_text();start=source.index('static void iface_local_visit(iface_local_plan_t *p,node_t *n,unsigned depth) {')
        visitor=source[start:source.index('static boolean_t iface_local_same_path',start)]
        self.assertEqual(set(re.findall(r'case (Node\w+)',visitor)),kinds)
        expected=json.loads(Path(__file__).with_name('i1b2_inventory.json').read_text())
        self.assertEqual(set(expected['ast_kinds']),kinds)
        self.assertIn('default:iface_local_fail',visitor)
    def test_no_origin_upgrade(self):
        source=(ROOT/'src/codegen/cg_iface_locals.c').read_text()
        self.assertNotIn('symbol->iface_excluded_origin =',source)
        self.assertNotIn('symbol->iface_excluded_origin=',source)
        self.assertIn('symbol->iface_source->decl!=b->decl',source)
    def test_meaningful_paired_matrix(self):
        self.assertGreaterEqual(sum(c.positive for c in matrix()),20)
        self.assertGreaterEqual(sum(not c.positive for c in matrix()),20)
        self.assertTrue(all(c.pattern for c in matrix() if not c.positive))
if __name__=='__main__':unittest.main(verbosity=2)
