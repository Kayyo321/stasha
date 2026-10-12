#!/usr/bin/env python3
"""Frozen fifth-family positional slice qualification.

Pure selftests never launch a compiler or image. Native qualification requires
all 184 ordinary O0--O3 checks before any of the 784 observed checks. The local
coordinator independently reviews source/IR before authorizing native execution;
that approval is not inferred from this runner or from a successful build.
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

if not __debug__:
    raise RuntimeError('qualification requires Python assertions enabled')
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
FAMILY = 'observed-positional-slices'
PROFILE = 'native-sequential-direct-source-positional-slice-v4'
CORPUS = ROOT / 'tests/observation/fixtures/positional_slices/corpus.json'
CORPUS_SHA256 = '12e1882c72db168c5d8b9bc80ab69c67a924a0007875a0a536d3272734ea0dd1'
ORACLE_INDEX_SHA256 = '3afe36ec7631c6f81330cbb46489d3b87c754ac8b357fae902535c98f911f310'
NATIVE_LEVELS = [0, 1, 2, 3]
FAMILY_TIMEOUT_SECONDS = 3600
COMMAND_TIMEOUT_SECONDS = 120
NATIVE_OOM = 'NOT_RUN / outside positional qualification'


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


support = load_module('positional_source_support', ROOT / 'tests/observation/test_source_imports.py')
sha = support.sha


def load_corpus():
    assert sha(CORPUS) == CORPUS_SHA256, 'changed frozen positional corpus'
    corpus = support.strict_json(CORPUS.read_bytes())
    assert corpus['independent_seal']['status'] == 'PASS_FULL_ORACLE_PREREQUISITE_FOR_EXACT_R2_SCOPE'
    assert corpus['independent_seal']['oracle_index_sha256'] == ORACLE_INDEX_SHA256
    for key, record in corpus['frozen_inputs'].items():
        encoded = (json.dumps(corpus[key], indent=2) + '\n').encode('ascii')
        assert hashlib.sha256(encoded).hexdigest() == record['sha256'], ('changed frozen document', key)
    for name, record in corpus['protocol']['files'].items():
        data = bytes.fromhex(record['data']) if record['encoding'] == 'hex' else record['data'].encode('ascii')
        assert hashlib.sha256(data).hexdigest() == record['sha256'], name
    return corpus


BUNDLE = load_corpus()
CASES = {case['id']: case for case in BUNDLE['r2_corpus']['positive_cases']}
NATIVE_NAMES = tuple(CASES)
METADATA = {case['case_id']: case for case in BUNDLE['source_metadata']['cases']}
REJECTIONS = {case['id']: case for case in BUNDLE['r2_corpus']['rejection_cases']}
RESOURCES = {case['id']: case for case in BUNDLE['r2_corpus']['resource_cases']}
CONTROLS = BUNDLE['inventory_controls']


def ordinary_runner():
    return load_module('positional_ordinary', ROOT / 'tests/memory/run_positional_make.py')


def native_levels(levels):
    assert levels == NATIVE_LEVELS, 'positional native qualification requires --opt 0,1,2,3 in frozen order'


def expected_cases(levels):
    return {'ordinary_native': [f'{name}-ordinary-O{opt}' for opt in levels for name in NATIVE_NAMES],
            'observed_native': [f'{name}-{mode["id"]}-O{opt}' for opt in levels for name in NATIVE_NAMES for mode in CASES[name]['observed_modes']]}


def nonlaunch_inventory(levels):
    return {'compile_only': ['reject-' + name for name in REJECTIONS] + ['bound-' + name for name in RESOURCES] +
            [f'{name}-{mode}-ir-O{opt}' for opt in levels for name in NATIVE_NAMES for mode in ('ordinary', 'warn', 'combined-equal')]}


def materialize(session):
    positive = ordinary_runner().materialize(session)
    for collection in (REJECTIONS, RESOURCES):
        for case in collection.values():
            record = case['source']; relative = 'sources/positional-slices/' + record['path']
            assert not Path(relative).is_absolute() and '..' not in Path(relative).parts
            path = session.artifacts / relative; path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(record['text'].encode('ascii')); assert sha(path) == record['sha256']
    for record in BUNDLE['r2_corpus']['tracked_fixed_sources']:
        assert sha(ROOT / record['target_path']) == record['sha256']
    session.freeze_generated()
    expected = {'sources/positional-slices/' + r['path']: r['sha256']
                for c in CASES.values() for r in c['sources']}
    expected.update({'sources/positional-slices/' + c['source']['path']: c['source']['sha256'] for coll in (REJECTIONS, RESOURCES) for c in coll.values()})
    assert session.generated == expected and len(expected) == 153
    return positive


def flags_for(mode):
    flags = ['--warn-memleak'] if mode['warn'] else []
    if mode['max'] is not None: flags += ['--max-leaked-bytes', str(mode['max'])]
    if mode['min'] is not None: flags += ['--min-test-coverage', str(mode['min'])]
    return flags


def protocol_bytes(name):
    record = BUNDLE['protocol']['files'][name]
    return bytes.fromhex(record['data']) if record['encoding'] == 'hex' else record['data'].encode('ascii')


def protocol_json(name):
    return support.strict_json(protocol_bytes(name))


def validate_json_fields(data):
    """Exact independently frozen emitter fields, including integer token types."""
    contract = protocol_json('JSON-FIELD-CONTRACT.json')
    def value_ok(value, description):
        if value is None and ('OR null' in description or 'string OR null' == description): return
        if 'integer' in description:
            maximum = 2**64 - 1 if '18446744073709551615' in description else 2**32 - 1
            if '0..100' in description: maximum = 100
            assert type(value) is int and 0 <= value <= maximum, (value, description)
        elif description == 'JSON boolean': assert type(value) is bool
        elif description == 'array': assert type(value) is list
        elif description == 'object': assert type(value) is dict
        else:
            assert type(value) is str, (value, description)
            if 'hex' in description:
                assert re.fullmatch(r'(?:[0-9a-f]{2})*', value)
                if '64' in description: assert len(value) == 64
    def row_ok(row, fields):
        assert type(row) is dict and set(row) == set(fields), ('wrong fields', set(row) ^ set(fields))
        for key, desc in fields.items(): value_ok(row[key], desc)
        for key, value in row.items():
            if key.endswith('_bytes_hex'):
                plain = key[:-10]
                if plain in row:
                    assert (value is None) == (row[plain] is None)
                    if value is not None: assert bytes.fromhex(value).decode('utf-8', errors='replace') == row[plain]
    row_ok(data, contract['complete_or_incomplete_parsed_report'])
    assert data['schema'] == data['runtime_abi'] == 4 and data['profile'] == PROFILE
    assert data['allocation_metric_version'] == data['line_metric_version'] == 2
    assert data['report_valid'] is True
    for table, fields in contract['table_row_exact_fields'].items():
        for row in data[table]: row_ok(row, fields)
    row_ok(data['child'], contract['child_exact_fields'])
    assert set(data['allocation_domains']) == set(contract['allocation_domains_exact_fields'])
    for subtotal in data['allocation_domains'].values(): row_ok(subtotal, contract['allocation_domain_subtotal_exact_fields'])
    for key in ('launched', 'waited', 'exited'): assert data['child'][key] in (0, 1)
    assert data['child']['exit_code'] <= 255
    for key in ('lines', 'exclusions'):
        assert all(type(v) is int and 0 <= v <= 2**32 - 1 for v in data[key])
    for f in data['files']:
        assert len(f['exclusions']) == f['physical_line_count']
        assert all(type(v) is int and 1 <= v <= 63 for v in f['exclusions'])
        assert f['lines'] == sorted(set(f['lines'])) and f['hit_lines'] == sorted(set(f['hit_lines']))
        assert set(f['hit_lines']) <= set(f['lines'])
    support.validate_site_metadata(data['sites'], len(data['tests']))
    for site in data['sites']:
        assert 0 <= site['decl_id'] < len(data['declarations'])
        decl = data['declarations'][site['decl_id']]
        assert site['file_id'] == decl['file_id']
        owners = [test for test in data['tests'] if test['decl_id'] == decl['id']]
        owner_test = owners[0]['id'] if owners else None
        assert site['static_test_id'] == owner_test
        assert site['owner'] == (owners[0]['name'] if owners else decl['name'])


def expected_tables(meta):
    declarations = []
    for record in meta['declarations']:
        row = dict(record); row['id'] = row.pop('decl_id'); row['selected'] = int(row['selected'])
        for field in ('module_name', 'name'): row[field + '_bytes_hex'] = row[field].encode('ascii').hex()
        declarations.append(row)
    tests = []
    for record in meta['tests']:
        row = dict(record); row['id'] = row.pop('test_id'); row['name_bytes_hex'] = row['name'].encode('ascii').hex(); tests.append(row)
    anchors = []
    for record in meta['anchors']:
        row = dict(record); row['id'] = row.pop('anchor_id'); row.pop('has_coverage_id'); anchors.append(row)
    sites = []
    for record in meta['sites']:
        row = {k: v for k, v in record.items() if k not in ('binding', 'binding_origin_site')}
        row['static_test_id'] = None if row['owner_test'] == 4294967295 else row['owner_test']
        row['owner_bytes_hex'] = row['owner'].encode('ascii').hex(); sites.append(row)
    return dict(declarations=declarations, tests=tests, anchors=anchors, sites=sites)


def validate_metadata(data, name, entry, *, meta=None):
    meta = METADATA[name] if meta is None else meta
    pairs = meta['coverage']['pairs']
    assert data['eligible_lines'] == len(pairs)
    assert data['coverage_applicable'] is bool(pairs)
    assert data['lines'] == [line for _, line in pairs]
    reasons = [0 if mask & 1 else next(reason for reason in range(1, 6) if mask & (1 << reason))
               for mask in meta['files'][0]['line_reason_masks']]
    assert data['exclusions'] == reasons
    assert data['text_encoding'] == 'UTF-8; raw dynamic text bytes in *_bytes_hex'
    for table, rows in expected_tables(meta).items(): assert data[table] == rows, (name, table, data[table], rows)
    assert len(data['files']) == len(meta['files'])
    for actual, wanted in zip(data['files'], meta['files']):
        path = (entry.parent / wanted['relative_path']).absolute()
        assert actual['id'] == wanted['file_id'] and actual['role'] == wanted['role']
        assert actual['source_sha256'] == wanted['content_sha256']
        assert actual['source_bytes'] == path.stat().st_size
        assert actual['physical_line_count'] == wanted['physical_line_count']
        assert actual['exclusions'] == wanted['line_reason_masks']
        assert actual['declared_module'] == wanted['module']
        requested = None if wanted['file_id'] == 0 else wanted['module']
        assert actual['requested_module'] == requested
        assert actual['lookup_path_bytes_hex'] == os.fsencode(path).hex()
        assert actual['canonical_path_bytes_hex'] == os.fsencode(path.resolve()).hex()
    expected_edges = []
    for record in meta['edges']:
        row = {k: record[k] for k in ('parent_file_id','child_file_id','line','col','ordinal','requested_module')}
        row['lookup_path'] = str(entry.parent / record['lookup_relative_to_entry_directory'])
        for field in ('lookup_path','requested_module'): row[field + '_bytes_hex'] = os.fsencode(row[field]).hex()
        expected_edges.append(row)
    assert data['edges'] == expected_edges
    assert data['source_path_bytes_hex'] == os.fsencode(entry).hex()
    assert data['original_root_spelling_bytes_hex'] == os.fsencode(entry).hex()
    assert data['invocation_directory_bytes_hex'] == os.fsencode(ROOT).hex()


def resource_metadata(name):
    control = next(r for r in CONTROLS['resource_controls'] if r['case_id'] == name)
    success = control['success_map_only_if_admitted']; recipes = control['constructor_source_recipes']
    declarations = [dict(row, file_id=0, source_linkage=0, selected=True, module_name=control['module'],
                         **control['cold_function_span' if row['kind'] == 1 else 'test_span']) for row in success['declarations']]
    test = dict(test_id=0,decl_id=1,file_id=0,name='nonlaunch only',line=control['test_span']['line'],col=control['test_span']['col'])
    anchors = [dict(anchor_id=i,decl_id=0,file_id=0,kind=1,exclusion_reason=0,has_coverage_id=True,coverage_id=i,
                    **recipe['declaration_span']) for i,recipe in enumerate(recipes)]
    sites = [dict(id=i,decl_id=0,file_id=0,kind=6,owner='cold',owner_test=4294967295,
                  slice_element_kind=recipe['element_kind'],slice_abi_stride=recipe['stride'],slice_count_arity=0,
                  slice_initializer_item_count=recipe['item_count'],**recipe['outer_make_span']) for i,recipe in enumerate(recipes)]
    return dict(files=[dict(file_id=0,relative_path=Path(control['source_path']).name,module=control['module'],role=0,
                            physical_line_count=control['physical_line_count'],content_sha256=control['source_sha256'],line_reason_masks=success['line_reason_masks'])],
                edges=[],declarations=declarations,tests=[test],anchors=anchors,sites=sites,
                coverage=dict(pairs=[[0,line] for line in success['eligible_lines']],hits=[]))


def validate_observed_nonlaunch(evidence, name, entry, *, resource=False):
    assert evidence is not None, 'observed nonlaunch positive requires driver metadata'
    assert evidence['manifest_valid'] is True and evidence['report_valid'] is False
    assert evidence['child']['launched'] == evidence['child']['exited'] == 0
    assert evidence['schema'] == 4 and evidence['profile'] == PROFILE
    validate_metadata(evidence,name,entry,meta=resource_metadata(name) if resource else None)
    assert support.recompute_graph(evidence) == evidence['input_graph_sha256']
    assert support.recompute_map(evidence) == evidence['map_sha256']


def domain_totals(meta):
    return {label: dict(live_allocations=meta['endpoint_domains'][domain]['records'], tracked_outstanding_bytes=meta['endpoint_domains'][domain]['bytes'])
            for domain, label in (('1','explicit_user_heap'), ('2','generated_primitive_slice_backing'))}


def run_observed(session, sources, levels):
    native_levels(levels)
    ordinary = ordinary_runner()
    ordinary.validate_positive_sources(session, sources)
    ordinary.validate_ordinary_receipts(session, sources, levels)
    seen_runs, evidence_paths, signatures = set(), set(), {}
    for opt in levels:
        for name in NATIVE_NAMES:
            case, meta, entry = CASES[name], METADATA[name], sources[name]
            for mode, expected_mode in zip(case['observed_modes'], meta['mode_oracles']):
                assert mode['id'] == expected_mode['mode_id']
                key = f'{name}-{mode["id"]}-O{opt}'
                binary = session.artifacts / (key + '.test')
                pre, post = [session.artifacts / (key + suffix) for suffix in ('.pre.ll','.post.ll')]
                dest, result = session.command(key, [session.compiler, 'test', entry, f'-o={opt}', '-o', binary, *flags_for(mode)],
                    expected=mode['expected_exit'], env=dict(os.environ, STS_DUMP_IR_PRE=str(pre), STS_DUMP_IR=str(post)))
                ordinary.assert_output(subprocess.CompletedProcess([], 0, result.stdout, b''), case['ordinary'])
                matches = re.findall(rb'observation: private evidence ([^\r\n]+)', result.stderr); assert len(matches) == 1
                evidence = Path(os.fsdecode(matches[0])); identity = str(evidence.resolve())
                assert identity not in evidence_paths and not evidence.is_symlink(); evidence_paths.add(identity)
                assert binary.is_file() and not binary.is_symlink()
                for leaf in ('driver.json','report'): assert not (evidence / leaf).is_symlink()
                encoded, raw = (evidence / 'driver.json').read_bytes(), (evidence / 'report').read_bytes()
                assert len(encoded) <= 33554432 and len(raw) <= 2097152
                (dest / 'driver.json').write_bytes(encoded); (dest / 'report').write_bytes(raw)
                data = support.strict_json(encoded); validate_json_fields(data)
                assert data['run'] not in seen_runs; seen_runs.add(data['run'])
                totals = domain_totals(meta); live = len(meta['expected_live_records'])
                byte_count = sum(t['tracked_outstanding_bytes'] for t in totals.values())
                bits = support.validate_driver(data, raw.decode('ascii'), session, binary, entry, expected=mode['expected_exit'],
                    pairs=list(map(tuple, meta['coverage']['pairs'])), hits=list(map(tuple, expected_mode['hit_pairs'])),
                    assertions=case['ordinary']['assertions'], tests=case['ordinary']['tests'], live=live, byte_count=byte_count)
                validate_metadata(data, name, entry)
                assert (bits.hex() if bits else '-') == expected_mode['bitmap_hex']
                assert data['allocations'] == meta['expected_live_records'] and data['allocation_domains'] == totals
                assert data['enabled'] == mode['enabled'] and data['warn_memleak'] is mode['warn']
                assert data['max_leaked_bytes'] == mode['max'] and data['min_test_coverage'] == mode['min']
                assert data['config_sha256'] == support.config_digest(session.compiler_hash, session.runtime_hash, opt,
                    enabled=mode['enabled'], warn=int(mode['warn']), maximum=mode['max'], minimum=mode['min'], target=data['target'])[0]
                assert result.stderr.count(b'warning: outstanding tracked allocations at suite endpoint\n') == int(mode['warn'] and live > 0)
                for path in (pre, post):
                    assert path.is_file() and not path.is_symlink()
                    ir = path.read_text(); assert '__stasha_obs_init_v4' in ir
                    assert not re.search(r'__stasha_obs_(?:init|slice_new|slice_rem)_v[123]\b', ir)
                signature = expected_tables(meta), [(f['lines'],f['exclusions']) for f in data['files']]
                if name in signatures: assert signatures[name] == signature
                signatures[name] = signature
                session.cases.append(dict(name=key, stage='observed_native', status='pass', executions=1, run_id=data['run'],
                    source_sha256=sha(entry), binary_sha256=sha(binary), pre_ir_sha256=sha(pre), post_ir_sha256=sha(post),
                    driver_sha256=sha(dest/'driver.json'), report_sha256=sha(dest/'report'),
                    input_graph_sha256=data['input_graph_sha256'], map_sha256=data['map_sha256'], config_sha256=data['config_sha256'],
                    expected_driver_exit=mode['expected_exit'], assertions_passed=case['ordinary']['assertions'], tests=case['ordinary']['tests'],
                    eligible=expected_mode['eligible'], covered=expected_mode['covered'], live_allocations=live,
                    tracked_outstanding_bytes=byte_count, allocation_domains=totals, corpus_sha256=CORPUS_SHA256))


def run_native(session, sources, levels):
    native_levels(levels)
    ordinary = ordinary_runner()
    ordinary.run_ordinary(session, sources, levels)
    ordinary.validate_ordinary_receipts(session, sources, levels)
    run_observed(session, sources, levels)


def rejection_diagnostic(name, result, *, resource=False):
    assert name in (RESOURCES if resource else REJECTIONS) and result.returncode == 1
    assert b'PASS\n' not in result.stdout and b'passed, ' not in result.stdout
    footer = re.compile(rb'\(EE\) \[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] code generation failed\n')
    diagnostic = result.stdout + b'\n' + b''.join(line for line in result.stderr.splitlines(keepends=True) if not footer.fullmatch(line))
    assert not support.POSITIVE_NONLAUNCH_FAILURES.search(diagnostic)
    assert not re.search(rb'linking|archiving|link_object|archive_object', result.stderr, re.I)
    assert re.search(re.escape((name+'.sts').encode()) + rb':[1-9][0-9]*', result.stderr), 'missing owning source location'
    assert b'unsupported by observation profile:' in result.stderr, 'parser/ordinary error does not close an observer boundary'
    # Headlines are separate from snippets/locations in the ordinary renderer.
    plain = re.sub(rb'\x1b\[[0-9;]*m', b'', diagnostic)
    observer_headline = re.compile(rb'error: unsupported by observation profile: [^\r\n]+')
    stderr_plain = re.sub(rb'\x1b\[[0-9;]*m', b'', result.stderr)
    assert any(observer_headline.fullmatch(line) for line in stderr_plain.splitlines()), 'missing genuine observer headline'
    # quit() in common/logger.c emits one final, uncolored summary. It is
    # not a second source error: diag_finish adds 1 and log_err adds its error
    # flag (2), so one observer error plus the main codegen footer totals 3.
    # Accept only canonical positive LP64 counts and exact singular/plural.
    # No warning suffix or nonterminal/duplicated/spoofed summary is ignored.
    last = result.stderr.splitlines(keepends=True)[-1] if result.stderr else b''
    abort = re.fullmatch(rb'error: aborting due to ([1-9][0-9]{0,19}) previous error(s?)\n', last)
    if abort:
        count = int(abort[1])
        assert count <= 2**64 - 1 and bool(abort[2]) == (count != 1), 'malformed abort summary'
        assert plain.endswith(last)
        plain = plain[:-len(last)]
    remaining = b'\n'.join(line for line in plain.splitlines() if not observer_headline.fullmatch(line))
    assert not re.search(rb'\b(?:parse|parser|lex|lexer|syntax)[^\r\n]*(?:error|fail)|\berror(?:\[[^]\r\n]*\])?:', remaining, re.I), 'mixed ordinary/parser diagnostic'
    record = (RESOURCES if resource else REJECTIONS)[name]['source']
    matches = re.findall(re.escape((name+'.sts').encode()) + rb':([1-9][0-9]*)(?::([1-9][0-9]*))?', plain)
    assert matches and all(1 <= int(line) <= 1 + record['text'].count('\n') for line, _ in matches)



def run_nonlaunch(session, sources, levels, ir_compiler):
    ir_compiler = ir_compiler.resolve(strict=True); digest = sha(ir_compiler)
    assert ir_compiler != session.compiler and digest != session.compiler_hash
    session.ir_compiler_hash = digest
    controls = [(f'reject-{name}', case['source'], False, name, False) for name, case in REJECTIONS.items()]
    controls += [(f'bound-{name}', case['source'], case['expected']=='admit_nonlaunch', name, True) for name, case in RESOURCES.items()]
    for key, record, admit, name, resource in controls:
        path = session.artifacts / ('sources/positional-slices/' + record['path'])
        pre, post = [session.artifacts/(key+suffix) for suffix in ('.pre.ll','.post.ll')]
        output = session.artifacts / (key+'.must-not-exist')
        dest, result = session.command(key, [ir_compiler,'test',path,'--warn-memleak','-o',output], expected=1,
            env=dict(os.environ,STS_DUMP_IR_PRE=str(pre),STS_DUMP_IR=str(post)))
        if admit:
            support.validate_positive_nonlaunch_result(result, output)
            for ir in (pre,post): assert ir.is_file() and '__stasha_obs_init_v4' in ir.read_text()
        else:
            rejection_diagnostic(name,result,resource=resource)
            assert not pre.exists() and not post.exists()
        evidence = support.retain_nonlaunch_evidence(dest,result)
        assert not output.exists()
        if admit: validate_observed_nonlaunch(evidence,name,path,resource=True)
        session.cases.append(dict(name=key,stage='compile_only',status='pass',executions=0,source_sha256=sha(path)))
    for opt in levels:
        for name in NATIVE_NAMES:
            for mode_name in ('ordinary','warn','combined-equal'):
                key=f'{name}-{mode_name}-ir-O{opt}'; output=session.artifacts/(key+'.must-not-exist')
                pre,post=[session.artifacts/(key+s) for s in ('.pre.ll','.post.ll')]
                flags=[] if mode_name=='ordinary' else flags_for(next(m for m in CASES[name]['observed_modes'] if m['id']==mode_name))
                dest,result=session.command(key,[ir_compiler,'test',sources[name],f'-o={opt}','-o',output,*flags],expected=1,
                    env=dict(os.environ,STS_DUMP_IR_PRE=str(pre),STS_DUMP_IR=str(post)))
                support.validate_positive_nonlaunch_result(result,output)
                evidence=support.retain_nonlaunch_evidence(dest,result)
                if mode_name!='ordinary': validate_observed_nonlaunch(evidence,name,sources[name])
                for ir in (pre,post):
                    assert ('__stasha_obs_init_v4' in ir.read_text()) == (mode_name!='ordinary')
                    if mode_name=='ordinary': assert '__stasha_obs_' not in ir.read_text()
                session.cases.append(dict(name=key,stage='compile_only',status='pass',executions=0,
                    pre_ir_sha256=sha(pre),post_ir_sha256=sha(post),independent_ir_review='required'))
    assert sha(ir_compiler)==digest


def selftest(artifacts):
    class PureTests(unittest.TestCase):
        def test_inventory_and_frozen_sources(self):
            self.assertEqual(load_corpus(),BUNDLE)
            self.assertEqual((len(CASES),len(REJECTIONS),len(RESOURCES)),(46,102,4))
            inventory=expected_cases(NATIVE_LEVELS)
            self.assertEqual(tuple(map(len,inventory.values())),(184,784))
            self.assertEqual(set(CASES),set(METADATA))
            self.assertFalse(set(sum(inventory.values(),[])) & set(nonlaunch_inventory(NATIVE_LEVELS)['compile_only']))
            for case in CASES.values():
                for record in case['sources']: self.assertEqual(hashlib.sha256(record['text'].encode('ascii')).hexdigest(),record['sha256'])
        def test_thresholds_and_endpoint_attribution(self):
            for name, case in CASES.items():
                meta=METADATA[name];totals=domain_totals(meta)
                self.assertEqual(sum(t['live_allocations'] for t in totals.values()),len(meta['expected_live_records']))
                self.assertEqual(sum(t['tracked_outstanding_bytes'] for t in totals.values()),sum(a['bytes'] for a in meta['expected_live_records']))
                self.assertEqual(meta['ordinary'],case['ordinary'])
                for m,oracle in zip(case['observed_modes'],meta['mode_oracles']):
                    coverage=m['min'] is not None; total=sum(t['tracked_outstanding_bytes'] for t in totals.values())
                    fail=(m['max'] is not None and total>m['max']) or (coverage and (not meta['coverage']['pairs'] or 100*len(meta['coverage']['hits'])<m['min']*len(meta['coverage']['pairs'])))
                    self.assertEqual(m['expected_exit'],int(fail));self.assertEqual(oracle['driver_exit'],int(fail))
                    self.assertEqual(oracle['hit_pairs'],meta['coverage']['hits'] if coverage else [])
            self.assertEqual(METADATA['plain_multiline']['coverage']['pairs'],[[0,3],[0,8],[0,9]])
        def test_source_metadata_mutations(self):
            with tempfile.TemporaryDirectory() as temp:
                for name,meta in METADATA.items():
                    directory=Path(temp)/name;directory.mkdir()
                    for source in CASES[name]['sources']:
                        path=directory/source['relative_path'];path.parent.mkdir(parents=True,exist_ok=True);path.write_text(source['text'])
                    entry=directory/CASES[name]['entry'];data=expected_tables(meta)
                    pairs=meta['coverage']['pairs']
                    data.update(eligible_lines=len(pairs),coverage_applicable=bool(pairs),lines=[line for _,line in pairs],
                        exclusions=[0 if mask&1 else next(reason for reason in range(1,6) if mask&(1<<reason)) for mask in meta['files'][0]['line_reason_masks']],
                        text_encoding='UTF-8; raw dynamic text bytes in *_bytes_hex',source_path_bytes_hex=os.fsencode(entry).hex(),original_root_spelling_bytes_hex=os.fsencode(entry).hex(),invocation_directory_bytes_hex=os.fsencode(ROOT).hex(),files=[],edges=[])
                    for f in meta['files']:
                        path=directory/f['relative_path']
                        data['files'].append(dict(id=f['file_id'],role=f['role'],source_sha256=f['content_sha256'],source_bytes=path.stat().st_size,
                            physical_line_count=f['physical_line_count'],exclusions=f['line_reason_masks'],declared_module=f['module'],
                            requested_module=None if f['file_id']==0 else f['module'],lookup_path_bytes_hex=os.fsencode(path).hex(),canonical_path_bytes_hex=os.fsencode(path.resolve()).hex()))
                    for edge in meta['edges']:
                        row={k:edge[k] for k in ('parent_file_id','child_file_id','line','col','ordinal','requested_module')}
                        row['lookup_path']=str(directory/edge['lookup_relative_to_entry_directory'])
                        for field in ('lookup_path','requested_module'):row[field+'_bytes_hex']=os.fsencode(row[field]).hex()
                        data['edges'].append(row)
                    support.validate_site_metadata(data['sites'],len(data['tests']));validate_metadata(data,name,entry)
                    for table in ('declarations','tests','anchors','sites','edges'):
                        if data[table]:
                            changed=copy.deepcopy(data);changed[table][0]['col']+=1
                            self.assertRaises(AssertionError,validate_metadata,changed,name,entry)
                    for field,value in (('eligible_lines',999),('coverage_applicable',not bool(pairs)),('lines',[999]),('exclusions',[5]),('text_encoding','guessed')):
                        changed=copy.deepcopy(data);changed[field]=value
                        self.assertRaises(AssertionError,validate_metadata,changed,name,entry)
                    changed=copy.deepcopy(data);changed['files'][0]['exclusions'][0]=63
                    self.assertRaises(AssertionError,validate_metadata,changed,name,entry)
        def test_encoding_known_answers(self):
            for name,kat in BUNDLE['protocol']['index']['encoding_known_answers'].items():
                raw=protocol_bytes(kat['byte_file']);self.assertEqual(len(raw),kat['size'])
                self.assertEqual(hashlib.sha256(raw).hexdigest(),kat['sha256'])
                fields=protocol_json(kat['field_index']);inputs=fields['inputs']
                if name.startswith('config-'):
                    digest,encoded=support.config_digest(inputs['compiler_sha256'],inputs['runtime_sha256'],inputs['optimization'],
                        enabled=inputs['enabled'],warn=int(inputs['warn_memleak']),maximum=inputs['max_leaked_bytes'],minimum=inputs['min_test_coverage'],debug=inputs['debug'],target=inputs['target'])
                    self.assertEqual(encoded,raw);self.assertEqual(digest,kat['sha256'])
                elif 'map' in name:
                    model=inputs.get('manifest',inputs)
                    self.assertEqual(support.recompute_map(model,version=3 if name=='preserved-map-v3' else 4),kat['sha256'])
                else: self.assertEqual(support.recompute_graph(inputs),kat['sha256'])
        def test_protocol_vectors(self):
            index=BUNDLE['protocol']['index']
            for vector in index['positive_vectors']:
                with self.subTest(vector=vector['id']):
                    data=protocol_json(vector['json']);validate_json_fields(data)
                    support.validate_raw_report(data,protocol_bytes(vector['raw']).decode('ascii'),require_complete=False)
                    self.assertEqual(data['overall_status'],vector['expected_overall_status'])
            base=protocol_json('vectors/mixed-retained.json');raw=protocol_bytes('vectors/mixed-retained.raw').decode('ascii')
            for vector in index['raw_rejection_vectors']:
                with self.subTest(vector=vector['id']):
                    self.assertRaises((AssertionError,KeyError,ValueError),support.validate_raw_report,base,protocol_bytes(vector['path']).decode('ascii'),require_complete=False)
            for vector in index['json_rejection_vectors']:
                with self.subTest(vector=vector['id']):
                    def check():
                        data=protocol_json(vector['path']);validate_json_fields(data);support.validate_raw_report(data,raw,require_complete=False)
                    self.assertRaises((AssertionError,KeyError,ValueError),check)
        def test_paired_incomplete_semantic_corruption(self):
            data=protocol_json('vectors/incomplete.json');raw=protocol_bytes('vectors/incomplete.raw').decode('ascii')
            data['allocations']=[dict(generation=1,domain=1,bytes=7,allocation_site=0,resize_site=4294967295,test=1)]
            data['live_allocations']=1;data['tracked_outstanding_bytes']=7
            data['allocation_domains']['explicit_user_heap']=dict(live_allocations=1,tracked_outstanding_bytes=7)
            bad=raw.replace('allocations 0 0\n','allocations 1 7\nallocation 1 1 7 0 4294967295 1\n')
            validate_json_fields(data)
            self.assertRaisesRegex(AssertionError,'unstarted test',support.validate_raw_report,data,bad,require_complete=False)
            data=protocol_json('vectors/incomplete.json')
            data['tests_started']=0;data['covered_lines']=1;data['coverage'][0]['hit']=True
            data['files'][0].update(hit_lines=[2],covered_lines=1)
            bad=raw.replace('tests 1 0','tests 0 0').replace('test 0 1','test 0 0').replace('hits 00','hits 01')
            validate_json_fields(data)
            self.assertRaisesRegex(AssertionError,'hit before',support.validate_raw_report,data,bad,require_complete=False)
            for field,counts in (('assertions_passed','1 0'),('assertions_failed','0 1')):
                data=protocol_json('vectors/incomplete.json');data['tests_started']=0;data[field]=1
                bad=raw.replace('tests 1 0','tests 0 0').replace('test 0 1','test 0 0').replace('assertions 0 0','assertions '+counts)
                validate_json_fields(data)
                self.assertRaisesRegex(AssertionError,'assertion before',support.validate_raw_report,data,bad,require_complete=False)
        def test_manifest_boundaries(self):
            base=protocol_json('synthetic-manifest-v4.json');controls=protocol_json('manifest-mutations.json')
            for mutation in controls['mutations']:
                model=copy.deepcopy(base)
                match=re.fullmatch(r'manifest.sites\[([0-9]+)\].([a-z_]+)',mutation['set_path']);self.assertIsNotNone(match)
                model['manifest']['sites'][int(match[1])][match[2]]=mutation['value']
                check=lambda:support.validate_site_metadata(model['manifest']['sites'],len(model['manifest']['tests']))
                if mutation['expected_manifest']=='accept':check()
                else:self.assertRaises((AssertionError,KeyError),check)
            for control in controls['missing_field_controls']:
                model=copy.deepcopy(base);del model['manifest']['sites'][6][control['site_field']]
                self.assertRaises((AssertionError,KeyError),support.validate_site_metadata,model['manifest']['sites'],2)
            for control in controls['aggregate_controls']:
                sites=copy.deepcopy(base['manifest']['sites'][6:8])
                for s,n in zip(sites,control['items']):s['slice_initializer_item_count']=n
                if control['expected']=='accept':support.validate_site_metadata(sites,2)
                else:self.assertRaises(AssertionError,support.validate_site_metadata,sites,2)
        def test_subset_cannot_dispatch(self):
            with mock.patch.dict(globals(), ordinary_runner=mock.Mock(side_effect=AssertionError('ordinary dispatch forbidden'))):
                for levels in ([0],[0,2,3],[3,2,1,0],[]):
                    self.assertRaisesRegex(AssertionError,'requires --opt',run_native,None,None,levels)
        def test_ordinary_failure_cannot_dispatch_observed(self):
            for error in (AssertionError('wrong value'),subprocess.TimeoutExpired('ordinary',120),OSError('missing image')):
                ordinary=mock.Mock();ordinary.run_ordinary.side_effect=error
                observed=mock.Mock()
                with mock.patch.dict(globals(), ordinary_runner=mock.Mock(return_value=ordinary), run_observed=observed):
                    self.assertRaises(type(error),run_native,None,None,NATIVE_LEVELS);observed.assert_not_called()
            ordinary=mock.Mock();ordinary.validate_ordinary_receipts.side_effect=AssertionError('changed ordinary image')
            observed=mock.Mock()
            with mock.patch.dict(globals(), ordinary_runner=mock.Mock(return_value=ordinary), run_observed=observed):
                self.assertRaises(AssertionError,run_native,None,None,NATIVE_LEVELS);observed.assert_not_called()
        def test_full_ordinary_barrier_order(self):
            order=[];ordinary=mock.Mock()
            ordinary.run_ordinary.side_effect=lambda *a:order.append('184 ordinary')
            ordinary.validate_ordinary_receipts.side_effect=lambda *a:order.append('verify184')
            with mock.patch.dict(globals(), ordinary_runner=mock.Mock(return_value=ordinary),run_observed=mock.Mock(side_effect=lambda *a:order.append('784 observed'))):
                run_native(None,None,NATIVE_LEVELS)
            self.assertEqual(order,['184 ordinary','verify184','784 observed'])
        def check_native_bitmap(self, name, mode_id, bits, wire, accepted):
            # Exercise run_native -> run_observed, not a copied comparison.
            # Stop immediately after that comparison; no image is executable.
            class AfterBitmap(Exception): pass
            class Evidence(dict):
                def __getitem__(self, key):
                    if key == 'allocations': raise AfterBitmap
                    return super().__getitem__(key)
            case=copy.deepcopy(CASES[name]);meta=copy.deepcopy(METADATA[name])
            mode=next(m for m in case['observed_modes'] if m['id']==mode_id)
            oracle=next(m for m in meta['mode_oracles'] if m['mode_id']==mode_id)
            case['observed_modes']=[mode];meta['mode_oracles']=[dict(oracle,bitmap_hex=wire)]
            with tempfile.TemporaryDirectory() as temp:
                directory=Path(temp);evidence=directory/'evidence';evidence.mkdir()
                dest=directory/'retained';dest.mkdir()
                (evidence/'driver.json').write_bytes(b'{}');(evidence/'report').write_bytes(b'pure mock\n')
                (directory/f'{name}-{mode_id}-O0.test').write_bytes(b'INERT; NEVER EXECUTED\n')
                sources={name:directory/'source-not-opened.sts'}
                session=mock.Mock(artifacts=directory,compiler=directory/'compiler-not-launched')
                session.command.return_value=(dest,subprocess.CompletedProcess([],mode['expected_exit'],b'',
                    b'observation: private evidence '+os.fsencode(evidence)+b'\n'))
                ordinary=mock.Mock(assert_output=mock.Mock());metadata=mock.Mock()
                with mock.patch.dict(globals(),ordinary_runner=mock.Mock(return_value=ordinary),
                        NATIVE_NAMES=(name,),CASES={name:case},METADATA={name:meta},
                        validate_json_fields=mock.Mock(),validate_metadata=metadata), \
                        mock.patch.object(support,'strict_json',return_value=Evidence(run='pure-run')), \
                        mock.patch.object(support,'validate_driver',return_value=bits) as driver:
                    with self.assertRaises(AfterBitmap if accepted else AssertionError):
                        run_native(session,sources,NATIVE_LEVELS)
                    ordinary.run_ordinary.assert_called_once_with(session,sources,NATIVE_LEVELS)
                    session.command.assert_called_once();driver.assert_called_once()
                    metadata.assert_called_once_with(mock.ANY,name,sources[name])
        def test_native_empty_bitmap_uses_wire_sentinel(self):
            self.assertEqual(METADATA['primitive_i8']['mode_oracles'][0]['bitmap_hex'],'-')
            self.assertNotEqual(b''.hex(),'-')  # The original literal assertion fails.
            self.check_native_bitmap('primitive_i8','warn',b'','-',True)
            for wire in ('','00','--','- '):
                with self.subTest(wire=wire):
                    self.check_native_bitmap('primitive_i8','warn',b'',wire,False)
            self.check_native_bitmap('primitive_i8','warn',b'\x00','-',False)
        def test_native_nonempty_bitmap_is_exact(self):
            for mode_id,wire,bits in (('warn','00',b'\x00'),('coverage-zero','0f',b'\x0f')):
                oracle=next(m for m in METADATA['coverage']['mode_oracles'] if m['mode_id']==mode_id)
                self.assertEqual(oracle['bitmap_hex'],wire)
                self.check_native_bitmap('coverage',mode_id,bits,wire,True)
                for wrong_bits in (b'',bytes([bits[0]^1]),bits+b'\x00'):
                    with self.subTest(mode=mode_id,bits=wrong_bits):
                        self.check_native_bitmap('coverage',mode_id,wrong_bits,wire,False)
            for wire in ('-','','0F','f','0f00','gg'):
                with self.subTest(wire=wire):
                    self.check_native_bitmap('coverage','coverage-zero',b'\x0f',wire,False)
        def test_no_parser_or_infrastructure_error_closes_rejection(self):
            name=next(iter(REJECTIONS));valid=('error: unsupported by observation profile: item\n  --> '+name+'.sts:3:1\n').encode()
            rejection_diagnostic(name,subprocess.CompletedProcess([],1,b'',valid))
            for error in (b'parse error',b'ordinary type error',valid+b'LLVM IR verification failed',valid+b'linking failed',valid+b'parse error',valid+b'error: ordinary type error'):
                self.assertRaises(AssertionError,rejection_diagnostic,name,subprocess.CompletedProcess([],1,b'',error))
        def test_captured_observer_abort_footer(self):
            captured = (
                b'[2026-10-12 05:43:59] generating code (test mode)\n'
                b'error: unsupported by observation profile: positional item requires a known nonpointer primitive scalar value\n'
                b'  --> /workspace/scratch/bbc57aabb09b/stasha-restoration/hardening-checkpoint/positional-slice-observation/nonlaunch-new-r1/run-60jt9tgn/sources/positional-slices/reject/item_pointer-called.sts:3:51\n'
                b'  |\n'
                b'1 | mod positional_reject;\n'
                b'2 | fn bad(void): void {\n'
                b'3 |     heap u8 *rw p = new.(1); heap []i32 s = make.{p};\n'
                b'  |                                                   ^ here\n'
                b'  |\n'
                b'(EE) [2026-10-12 05:43:59] code generation failed\n'
                b'observation: private evidence /tmp/stasha-observation-ICAKON\n'
                b'error: aborting due to 3 previous errors\n'
            )
            self.assertEqual(hashlib.sha256(captured).hexdigest(), '900fb1ecd6671524857a7d1aa3db179e7eac2f860ef1f400fb4b73938aadf281')
            name='item_pointer-called'
            rejection_diagnostic(name,subprocess.CompletedProcess([],1,b'',captured))
            summary=b'error: aborting due to 3 previous errors\n'
            prefix=captured[:-len(summary)]
            self.assertTrue(captured.endswith(summary))
            for footer in (b'error: aborting due to 1 previous error\n',
                           b'error: aborting due to 2 previous errors\n',
                           b'error: aborting due to 18446744073709551615 previous errors\n'):
                rejection_diagnostic(name,subprocess.CompletedProcess([],1,b'',prefix+footer))
            # The summary itself cannot establish observer rejection or ownership.
            for bad in (summary,prefix.replace(b'unsupported by observation profile:',b'ordinary type error:' )+summary,
                        prefix.replace(b'item_pointer-called.sts:3:51',b'other.sts:3:51')+summary):
                self.assertRaises(AssertionError,rejection_diagnostic,name,subprocess.CompletedProcess([],1,b'',bad))
            for error in (b'error: ordinary type mismatch\n',b'error[syntax]: expected expression\n',
                          b'parse error\n',b'LLVM IR verification failed\n'):
                for bad in (prefix+error+summary,captured+error):
                    self.assertRaises(AssertionError,rejection_diagnostic,name,subprocess.CompletedProcess([],1,b'',bad))
            for bad_footer in (b'error: aborting due to 0 previous errors\n',
                               b'error: aborting due to 03 previous errors\n',
                               b'error: aborting due to +3 previous errors\n',
                               b'error: aborting due to 1 previous errors\n',
                               b'error: aborting due to 3 previous error\n',
                               b'error: aborting due to 18446744073709551616 previous errors\n',
                               b'error: aborting due to 3 previous errors',
                               b'error: aborting due to 3 previous errors\r\n',
                               b'error: aborting due to 3 previous errors; 1 warning emitted\n',
                               b'error: aborting due to 3 previous errors (ignored)\n',
                               b'prefix '+summary,summary+summary,summary+b'after summary\n',summary+b'\n'):
                self.assertRaises(AssertionError,rejection_diagnostic,name,subprocess.CompletedProcess([],1,b'',prefix+bad_footer))
            self.assertRaises(AssertionError,rejection_diagnostic,name,subprocess.CompletedProcess([],1,summary,prefix))
            for spoof in (b'error: ordinary type error quoting unsupported by observation profile: item\n',
                          b"3 | 'error: unsupported by observation profile: item'\n",
                          b'  error: unsupported by observation profile: item\n',
                          b'item_pointer-called.sts:3:1: unsupported by observation profile: item\n'):
                bad=spoof+b'  --> item_pointer-called.sts:3:51\n'+summary
                self.assertRaises(AssertionError,rejection_diagnostic,name,subprocess.CompletedProcess([],1,b'',bad))
                # Even a genuine earlier observer headline must not hide a
                # separate ordinary error that quotes the marker phrase.
                if spoof.startswith(b'error: ordinary'):
                    self.assertRaises(AssertionError,rejection_diagnostic,name,subprocess.CompletedProcess([],1,b'',prefix+spoof+summary))
        def test_observed_nonlaunch_requires_complete_metadata(self):
            for resource,name in ((False,'coverage'),(True,'items_per_constructor_equal')):
                self.assertRaises(AssertionError,validate_observed_nonlaunch,None,name,Path('/inert'),resource=resource)
                self.assertRaises(AssertionError,validate_observed_nonlaunch,dict(manifest_valid=False),name,Path('/inert'),resource=resource)
            for name in ('items_per_constructor_equal','items_aggregate_equal'):
                meta=resource_metadata(name);tables=expected_tables(meta)
                support.validate_site_metadata(tables['sites'],1)
                self.assertEqual(sum(s['slice_initializer_item_count'] for s in tables['sites']),65536)
        def test_exact_promotions_and_preserved_counts(self):
            old=load_module('positional_old_slices',ROOT/'tests/observation/test_primitive_slices.py')
            self.assertEqual(len(old.REJECTIONS),174);self.assertEqual(len(old.COUNT_FAILURE_IDS),22)
            self.assertEqual(set(old.PROMOTED_REJECTIONS),{'inline_make-called','inline_make-dead','inline_make-uncalled'})
            for name in old.PROMOTED_REJECTIONS:
                self.assertNotIn(name,old.REJECTIONS)
                record=CASES['promoted_'+name]['sources'][0]
                self.assertEqual(record['text'],old.GENERATED_SOURCES['sources/primitive-slices/reject/'+name+'.sts'])
    with mock.patch('subprocess.run',side_effect=AssertionError('pure positional tests cannot spawn processes')):
        result=unittest.TextTestRunner(verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(PureTests))
    artifacts.mkdir(parents=True,exist_ok=True)
    (artifacts/'result.json').write_text(json.dumps(dict(schema=2,family='positional-slice-selftest',status='pass' if result.wasSuccessful() else 'fail',
        tests_run=result.testsRun,failures=len(result.failures),errors=len(result.errors),native_executions=0,corpus_sha256=CORPUS_SHA256),indent=2)+'\n')
    return 0 if result.wasSuccessful() else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path);parser.add_argument('--ir-compiler',type=Path)
    parser.add_argument('--artifacts',required=True,type=Path);parser.add_argument('--opt',default='0,1,2,3')
    parser.add_argument('--cc',default='cc');parser.add_argument('--selftest-only',action='store_true')
    parser.add_argument('--stage',choices=('all','prepare','compile-only','native'),default='all')
    args=parser.parse_args()
    if args.selftest_only:return selftest(args.artifacts)
    levels=support.parse_levels(args.opt,parser)
    if args.stage in ('all','native') and levels != NATIVE_LEVELS:parser.error('positional native qualification requires --opt 0,1,2,3 in frozen order')
    if args.stage!='prepare' and args.compiler is None:parser.error('--compiler required')
    if args.stage=='compile-only' and args.ir_compiler is None:parser.error('--ir-compiler required; native fallback forbidden')
    session=support.FamilySession(FAMILY,args.compiler,args.artifacts,levels,runtime=True)
    session.nonlaunch_required=nonlaunch_inventory(levels)['compile_only']
    session.qualification_contract=dict(corpus_sha256=CORPUS_SHA256,oracle_index_sha256=ORACLE_INDEX_SHA256,
        ordinary_required=184,observed_required=784,ordinary_before_observed=True,cumulative_timeout_seconds=3600,per_command_timeout_seconds=120,
        native_oom=NATIVE_OOM,source_ir_review='independent coordinator prerequisite; no local approval inferred')
    try:
        sources=materialize(session);inventory=nonlaunch_inventory(levels) if args.stage=='compile-only' else expected_cases(levels)
        session.write_inventory(inventory)
        if args.stage=='prepare':session.status='prepared'
        elif args.stage=='compile-only':run_nonlaunch(session,sources,levels,args.ir_compiler);session.finish(inventory)
        else:run_native(session,sources,levels);session.finish(inventory)
    except BaseException as error:
        session.status,session.failure='fail',repr(error);raise
    finally:session.receipt()
    print(f'{session.status.upper()}: positional-slice {args.stage}; evidence {session.artifacts}; native OOM {NATIVE_OOM}')
    return 0


if __name__=='__main__':raise SystemExit(main())
