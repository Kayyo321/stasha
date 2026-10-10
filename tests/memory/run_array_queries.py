#!/usr/bin/env python3
"""Bounded fixed-array len/cap regression qualification.

Only RuntimeCase instances can be built or executed. Rejections, huge extents,
legacy malformed projections and invalid-runtime bounds controls are CompileCase
instances and use the compiler's `ir` command only, even if wrongly accepted.
No historical characterization files are modified or imported as runtime cases.
"""
import argparse
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import tempfile

import run_access_contracts as access

ROOT = access.ROOT
capture = access.scalar_runner.capture
sha256 = access.scalar_runner.sha256
PLAN_SHA256 = '62692565f00592b9090be4384d431d868ef3f8e95aa103caa81787508a34a598'
BASELINE_SHA256 = 'c656109409bd2aa7935d2315f7cdb20107d6e79f4e19224968fd4a33d14eabf5'


@dataclass(frozen=True)
class RuntimeCase:
    name: str
    source: str
    ir_checks: tuple = ()
    outcome: str = field(default='runtime', init=False)


@dataclass(frozen=True)
class CompileCase:
    name: str
    source: str
    outcome: str
    diagnostic: str = ''
    ir_checks: tuple = ()

    def __post_init__(self):
        if self.outcome not in ('rejection', 'bounds_ir', 'legacy_ir', 'boundary_ir'):
            raise ValueError('compile-only cases cannot have a runtime outcome')
        if self.outcome == 'rejection' and not self.diagnostic:
            raise ValueError('rejections need an owned diagnostic oracle')


def program(name, body, declarations=''):
    return access.program('array_query_' + name, body, declarations)


def checks(expression, length, capacity=None, code=1):
    """Independent assertions; never accept a sum-only numerical oracle."""
    capacity = length if capacity is None else capacity
    return (f'if (len.({expression}) != {length}) {{ ret {code}; }}\n'
            f'if (cap.({expression}) != {capacity}) {{ ret {code + 1}; }}\n')


BOX = 'type Box: struct { ext i32 values[3]; };'
BOX_INIT = 'stack Box b = .{.values = .{11,22,33}};\n'
INTERFACE = '''type I: interface { m(void): i32; };
type S: struct.[I] { ext i32 n; ext fn m(void): i32 { ret this.n; } };
type T: struct { ext i32 n; };
type Row: struct { ext S values[2]; };
fn consume(stack I value): i32 { ret value.m(); }
'''


def runtime_cases():
    out = []
    def add(name, body, declarations='', ir=()):
        out.append(RuntimeCase(name, program(name, body + '\nret 0;', declarations), tuple(ir)))
    for name, declaration, count in (
        ('direct', 'stack i32 a[2] = .{17,24};', 2),
        ('named_extent', 'const i32 N=3; stack i32 a[N] = .{1,2,3};', 3),
        ('inferred_extent', 'stack i32 a[] = .{1,2,3};', 3),
        ('empty_extent', 'stack i32 a[0] = .{};', 0),
        ('empty_inferred', 'stack i32 a[] = .{};', 0),
        ('const_array', 'const i32 a[2] = .{17,24};', 2),
        ('final_array', 'final i32 a[2] = .{17,24};', 2),
    ):
        tail = 'if (a[len.(a)-1] != 24) { ret 3; }' if name == 'direct' else ''
        add(name, declaration + checks('a', count) + tail)
    add('matrix', 'stack i32 a[2][3]=.{}; stack i32 i=0;' + checks('a', 2) +
        'if (len.(a[i++]) != 3) { ret 3; } if (i != 1) { ret 4; }\n'
        'if (cap.(a[i++]) != 3) { ret 5; } if (i != 2) { ret 6; }')
    add('deep_rank', 'stack i32 a[2][3][4]=.{};' + checks('a', 2) + checks('a[0]', 3, code=3) + checks('a[0][0]', 4, code=5))
    add('single_element_alias', 'stack E a[2]=.{17,24};' + checks('a', 2), 'type E:i32;')
    add('owner_alias', 'stack Alias b=.{.values=.{11,22,33}};' + checks('b.values', 3), BOX + 'type Alias:Box;')
    for name, perm in [('array_pointer_elements', 'r'), ('array_of_writeonly_pointers', 'w')]:
        add(name, f'stack i32 x=17; stack i32 y=24; stack i32 *{perm} a[2]=.{{&x,&y}};' + checks('a', 2), ir=('no_pointer_element_load',))
    add('array_slice_elements', 'stack i32 a[4]=.{1,2,3,4}; stack []i32 s=a[1:3]; stack []i32 t=a[2:3];\n'
        'stack []i32 views[2]=.{s,t};' + checks('views', 2) + checks('views[0]', 2, 3, 3) + checks('views[1]', 1, 2, 5))
    add('field', BOX_INIT + checks('b.values', 3), BOX)
    add('nested_field', 'stack Holder h=.{.box=.{.values=.{11,22,33}}};' + checks('h.box.values', 3), BOX + 'type Holder:struct {ext Box box;};')
    add('indexed_owner', 'stack Box b[2]=.{Box {.values=.{11,22,33}},Box {.values=.{44,55,66}}}; stack i32 i=0;\n'
        'if (len.(b[i++].values)!=3) {ret 1;} if(i!=1){ret 2;}\n'
        'if (cap.(b[i++].values)!=3) {ret 3;} if(i!=2){ret 4;}', BOX)
    for name, expr in [('readonly_owner', 'p.values'), ('explicit_deref_owner', '(*.(p)).values')]:
        add(name, BOX_INIT + 'stack Box *r p=&b;' + checks(expr, 3), BOX)
    named_box = 'type Box:struct {ext i32 len[3];};'
    for name, setup, expr in (
        ('plain_owner_named_len', 'stack Box b=.{.len=.{1,2,3}};', 'b.len'),
        ('indexed_owner_named_len', 'stack Box a[2]=.{Box {.len=.{1,2,3}},Box {.len=.{4,5,6}}}; if(a.len!=2){ret 5;}', 'a[0].len'),
        ('array_owner_full_index', 'stack Box a[2][4]=.{};', 'a[0][0].len'),
        ('array_owner_indexed_pointer', 'stack Box b=.{.len=.{1,2,3}}; stack Box *r a[2]=.{&b,&b};', 'a[0].len'),
    ):
        add(name, setup + checks(expr, 3), named_box)
    for name, fields, expr, init in (
        ('self_field', 'ext i32 values[3];', 'this.values', '.{.values=.{1,2,3}}'),
        ('self_subarray', 'ext i32 rows[2][3];', 'this.rows[0]', '.{}'),
        ('self_nested', 'ext Inner nested;', 'this.nested.rows[0]', '.{}'),
    ):
        decl = ('type Inner:struct {ext i32 rows[2][3];};' if name == 'self_nested' else '')
        decl += 'type Box:struct {' + fields + 'ext fn query(void):i32 {' + checks(expr, 3) + f'ret len.({expr})+cap.({expr});' + '}};'
        add(name, f'stack Box b={init}; if(b.query()!=6){{ret 3;}}', decl, ('self_no_element_load',))
    add('pointer_call_owner', BOX_INIT + 'stack i32 calls=0;\n'
        'if(len.(get(&b,&calls).values)!=3){ret 1;} if(calls!=1){ret 2;}\n'
        'if(cap.(get(&b,&calls).values)!=3){ret 3;} if(calls!=2){ret 4;}',
        BOX + 'fn get(stack Box *r p,stack i32 *rw calls):stack Box *r {calls[0]+=1;ret p;}', ('owner_calls_twice',))
    add('global_array', checks('a', 3) + '{ stack i32 a[2]=.{17,24};' + checks('a', 2, code=3) + '}\n' + checks('a', 3, code=5), 'stack i32 a[3]=.{1,2,3};')
    backing = 'stack i32 a[4]=.{1,2,3,4};'
    add('slice', backing + 'stack []i32 s=a[1:3];' + checks('s', 2, 3) + 'stack []i32 empty=s[2:2];' + checks('empty', 0, 1, 3) + 'stack []i32 nil_view=nil;' + checks('nil_view', 0, 0, 5))
    add('single_slice_alias', backing + 'stack View s=a[1:3];' + checks('s', 2, 3), 'type View:[]i32;')
    add('slice_field', backing + 'stack []i32 s=a[1:3];stack Holder h=.{.view=s};' + checks('h.view', 2, 3), 'type Holder:struct {ext []i32 view;};')
    add('slice_call', backing + 'stack View s=a[1:3];stack i32 calls=0;\n'
        'if(len.(get(s,&calls))!=2){ret 1;}if(calls!=1){ret 2;}\n'
        'if(cap.(get(s,&calls))!=3){ret 3;}if(calls!=2){ret 4;}',
        'type View:[]i32;fn get(stack View s,stack i32 *rw calls):View {calls[0]+=1;ret s;}', ('owner_calls_twice',))
    for name, decl, setup, expression in (
        ('raw_pointer', '', 'stack i32 *r p=&a[0];', 'p'),
        ('pointer_alias', 'type Ptr:i32 *r;', 'stack Ptr p=&a[0];', 'p'),
        ('pointer_field', 'type Holder:struct {ext i32 *r p;};', 'stack Holder h=.{.p=&a[0]};', 'h.p'),
        ('let_array_decay', '', 'stack let b=a;', 'b'),
    ):
        add(name, 'stack i32 a[2]=.{17,24};' + setup + checks(expression, 0), decl)
    add('raw_pointer_effects', 'stack i32 a[2]=.{17,24};stack i32 calls=0;\n'
        'if(len.(get(&a[0],&calls))!=0){ret 1;}if(calls!=1){ret 2;}\n'
        'if(cap.(get(&a[0],&calls))!=0){ret 3;}if(calls!=2){ret 4;}',
        'fn get(stack i32 *r p,stack i32 *rw calls):stack i32 *r {calls[0]+=1;ret p;}', ('owner_calls_twice',))
    # Parent-approved plan clarification: inline query arithmetic in a concrete
    # array producer remains excluded by the unchanged I1b.2 source-fact rules.
    # A separately typed scalar index keeps the positive within that contract.
    for op in ('len', 'cap'):
        add('interface_last_element_' + op, 'stack S a[2]=.{S {.n=17},S {.n=24}};' + checks('a', 2) +
            'stack i32 last=' + op + '.(a)-1;stack S x=a[last];if(consume(x)!=24){ret 3;}', INTERFACE)
    order_decl = ('type Holder:struct {ext i32 rows[2][3];};'
        'fn get(stack Holder *r p,stack i32 *rw trace):stack Holder *r {trace[0]=trace[0]*10+1;ret p;}'
        'fn index(stack i32 *rw trace):i32 {trace[0]=trace[0]*10+2;ret 0;}')
    add('order_trace', 'stack Holder h=.{};stack i32 trace=0;\n'
        'if(len.(get(&h,&trace).rows[index(&trace)])!=3){ret 1;}if(trace!=12){ret 2;}trace=0;\n'
        'if(cap.(get(&h,&trace).rows[index(&trace)])!=3){ret 3;}if(trace!=12){ret 4;}', order_decl, ('order_trace',))
    add('unchecked_valid_subarray', 'stack i32 a[2][3]=.{};stack i32 i=0;unsafe {\n'
        'if(len.(a[unchecked:i++])!=3){ret 1;}if(i!=1){ret 2;}\n'
        'if(cap.(a[unchecked:i++])!=3){ret 3;}if(i!=2){ret 4;}}')
    return out


def compile_cases():
    out = []
    def add(name, body, declarations='', outcome='rejection', diagnostic='', ir=()):
        out.append(CompileCase(name, program(name, body, declarations), outcome, diagnostic, tuple(ir)))
    for op in ('len', 'cap'):
        q = lambda e: f'{op}.({e})'
        for name, setup, expr, decl, diagnostic in (
            ('huge_extent', 'stack Empty a[2147483648];', 'a', 'type Empty:struct {};', rf'fixed-array {op} extent exceeds INT32_MAX'),
            ('huge_selected_inner', 'stack Empty a[1][2147483648];', 'a[0]', 'type Empty:struct {};', rf'fixed-array {op} extent exceeds INT32_MAX'),
            ('writeonly_owner', BOX_INIT + 'stack Box *w p=&b;', 'p.values', BOX, 'memory access requires a readable pointer'),
            ('multi_pointer_owner', BOX_INIT + 'stack Box *rw p=&b;stack Box *rw *rw q=&p;', 'q.values', BOX, 'member access through multiple pointer levels requires an explicit dereference'),
            ('noninteger_index', 'stack i32 _a[2][3]=.{};', '_a[0.5]', '', 'array index must have integer type'),
            ('unchecked_outside_unsafe', 'stack i32 _a[2][3]=.{};', '_a[unchecked:0]', '', 'unchecked indexing requires an unsafe block'),
            ('value_call_owner', 'stack i32 _calls=0;', 'get(&_calls).values', BOX + 'fn get(stack i32 *rw calls):Box {calls[0]+=1;stack Box b=.{.values=.{1,2,3}};ret b;}', 'cannot resolve member|readable.*addressable.*array|addressable.*array'),
            ('element_alias', 'stack F a[2]=.{17,24};', 'a', 'type E:i32;type F:E;', "unknown type 'E'"),
            ('slice_alias', 'stack i32 a[4]=.{1,2,3,4};stack T s=a[1:3];', 's', 'type S:[]i32;type T:S;', "unknown type 'S'"),
            ('array_ternary_decay', 'stack i32 a[2]=.{1,2};stack i32 b[3]=.{1,2,3};stack bool c=true;', 'c?a:b', '', 'implicit pointer-to-integer conversion requires an explicit cast'),
        ):
            add(name + '_' + op, setup + 'ret ' + q(expr) + ';', decl, diagnostic=diagnostic)
        add('zero_field_' + op, 'stack Box b=.{};ret ' + q('b.values') + ';', 'type Box:struct {ext i32 values[0];};', diagnostic='array field needs a positive literal extent')
        for name, field, expr, prefix in (
            ('static_self_direct', 'ext i32 values[3];', 'this.values', ''),
            ('static_self_subarray', 'ext i32 rows[2][3];', 'this.rows[0]', ''),
            ('static_self_nested', 'ext Inner nested;', 'this.nested.rows[0]', 'type Inner:struct {ext i32 rows[2][3];};'),
        ):
            add(name + '_' + op, 'ret 0;', prefix + 'type Box:struct {' + field + '};fn Box.query(void):i32 {ret ' + q(expr) + ';}', diagnostic="this.*outside.*struct body method|invalid.*self.*receiver|missing.*self.*receiver|cannot resolve member")
        add('array_parameter_call_' + op, 'stack i32 a[3]=.{1,2,3};ret query(a);', 'fn query(stack i32 a[3]):i32 {ret ' + q('a') + ';}', diagnostic='implicit pointer-to-integer conversion requires an explicit cast')
        for qualifier in ('const', 'final'):
            add(qualifier + '_mutation_' + op, qualifier + ' i32 a[2]=.{17,24};stack i32 _n=' + q('a') + ';a[0]=9;ret 0;', diagnostic='read-only|readonly|writable|const|final')
        add('readonly_owner_mutation_' + op, BOX_INIT + 'stack Box *r p=&b;stack i32 _n=' + q('p.values') + ';p.values[0]=9;ret 0;', BOX, diagnostic='read-only|readonly|writable')
        add('interface_missing_initializer_' + op, 'stack S a[2]=.{S {.n=17}};stack i32 _n=' + q('a') + ';stack S s=a[0];ret consume(s);', INTERFACE, diagnostic='interface local transfer.*every selected concrete-array element needs an explicit checked initializer')
        add('interface_array_decay_' + op, 'stack S a[2]=.{S {.n=17},S {.n=24}};stack i32 _n=' + q('a') + ';stack S *rw p=a;p[0].n=1;stack S s=a[1];ret consume(s);', INTERFACE, diagnostic='interface local transfer.*array-valued occurrence exposes containing storage')
        add('interface_inline_query_index_' + op, 'stack S a[2]=.{S {.n=17},S {.n=24}};' + checks('a', 2) + 'stack S x=a[' + op + '.(a)-1];ret consume(x)-24;', INTERFACE, diagnostic='interface local transfer.*concrete local transfer lacks an independent source contract')
        add('interface_wrong_writer_' + op, 'stack Row h=.{.values=.{S {.n=17},S {.n=24}}};stack i32 _n=' + q('h.values') + ';stack T t=.{.n=9};h.values[0]=t;stack S s=h.values[0];ret consume(s);', INTERFACE, diagnostic='interface local transfer.*concrete source identity does not match')
        for name, setup, expression, extent in (
            ('max_extent', 'stack Empty a[2147483647];', 'a', 2147483647),
            ('max_selected_inner', 'stack Empty a[1][2147483647];', 'a[0]', 2147483647),
            ('huge_outer_small_inner', 'stack Empty a[2147483648][3];stack u64 i=0;', 'a[i]', 3),
        ):
            ir = [('return', 'main', extent)]
            if name == 'huge_outer_small_inner': ir.append(('guard_extent', 2147483648))
            add(name + '_' + op, setup + 'ret ' + q(expression) + ';', 'type Empty:struct {};', 'boundary_ir', ir=ir)
        add('array_parameter_body_' + op, 'ret 0;', 'fn query(stack i32 a[3]):i32 {ret ' + q('a') + ';}', 'boundary_ir', ir=(('return', 'query', 3),))
        for name, setup, expression, decl in (
            ('array_owner_field', 'stack Box a[2]=.{};', 'a.values', BOX),
            ('array_owner_legacy_len', 'stack Box a[2]=.{};', 'a.len', 'type Box:struct {ext i32 len[3];};'),
            ('array_owner_pointer_field', BOX_INIT + 'stack Box *r a[2]=.{&b,&b};', 'a.values', BOX),
            ('array_owner_partial_index', 'stack Box a[2][4]=.{};', 'a[0].values', BOX),
            ('array_owner_dereference', BOX_INIT + 'stack Box *r a[2]=.{&b,&b};', '(*.(a)).values', BOX),
            ('array_owner_nested', 'stack Holder h=.{};', 'h.boxes.values', BOX + 'type Holder:struct {ext Box boxes[2];};'),
            ('array_owner_nested_partial', 'stack Holder h=.{};', 'h.boxes[0].values', BOX + 'type Holder:struct {ext Box boxes[2][4];};'),
        ):
            add(name + '_' + op, setup + 'ret ' + q(expression) + ';', decl, 'legacy_ir', ir=(('return', 'main', 0),))
        for name, index, unsafe, effect in (
            ('selected_upper', '2', False, False), ('selected_negative', '-1', False, False),
            ('selected_unsafe_upper', '2', True, False), ('selected_effectful_upper', 'index(&calls)', False, True),
        ):
            decl = 'fn index(stack i32 *rw calls):i32 {calls[0]+=1;ret 2;}' if effect else ''
            body = 'stack i32 a[2][3]=.{};' + ('stack i32 calls=0;' if effect else '')
            body += ('unsafe {' if unsafe else '') + 'ret ' + q('a[' + index + ']') + ';' + ('}' if unsafe else '')
            ir = [('panic', 2, effect)]
            if effect: ir += ['index_call_once']
            add(name + '_' + op, body, decl, 'bounds_ir', ir=ir)
        dependent = 'len.(a)-3' if op == 'len' else 'cap.(a)'
        add('query_dependent_' + op, 'stack i32 a[2]=.{17,24};ret a[' + dependent + '];', outcome='bounds_ir', ir=(('panic', 2, False),))
    # Independent bounds matrix: every invalid form remains IR-only.
    for label, typ, value in [('negative', 'i32', '-1'), ('upper', 'i32', '2'), ('wide', 'u64', '18446744073709551615')]:
        for variable in (False, True):
            for unsafe in (False, True):
                for operation in ('read', 'store', 'compound'):
                    name = '_'.join(('bounds', label, 'variable' if variable else 'literal', 'unsafe' if unsafe else 'checked', operation))
                    idx = 'i' if variable else value
                    setup = 'stack i32 a[2]=.{17,24};' + (f'stack {typ} i={value};' if variable else '')
                    statement = {'read': f'ret a[{idx}];', 'store': f'a[{idx}]=7;ret 0;', 'compound': f'a[{idx}]+=1;ret 0;'}[operation]
                    add(name, setup + ('unsafe {' if unsafe else '') + statement + ('}' if unsafe else ''), outcome='bounds_ir', ir=(('panic', 2, variable),))
    add('zero_extent_access', 'stack i32 a[0]=.{};ret a[0];', outcome='bounds_ir', ir=(('panic', 0, False),))
    add('recursive_one_past', 'stack i32 a[2][3]=.{};stack i32 *rw _p=&a[2][0];ret 0;', outcome='bounds_ir', ir=(('panic', 2, False),))
    add('terminal_one_past', 'stack i32 a[2]=.{17,24};stack i32 *rw _p=&a[2];ret 0;', outcome='boundary_ir', ir=('no_bounds_panic',))
    return out


def cases():
    result = runtime_cases() + compile_cases()
    if len(result) != len({c.name for c in result}):
        raise ValueError('duplicate case names')
    return result


def function_body(ir, name):
    match = re.search(r'^define\b[^\n]*@' + re.escape(name) + r'\([^\n]*\)[^\n]*\{\n(.*?)^}', ir, re.M | re.S)
    if not match:
        raise ValueError('missing IR function ' + name)
    return match.group(1)


def blocks(body):
    matches = list(re.finditer(r'^([\w.$-]+):[^\n]*\n', body, re.M))
    return {m.group(1): body[m.end():matches[i+1].start() if i+1 < len(matches) else len(body)] for i, m in enumerate(matches)}


def cfg_edges(cfg, feasible=False):
    result = {}
    for name, text in cfg.items():
        conditional = re.search(r'br i1 ([^,]+), label %([\w.$-]+), label %([\w.$-]+)', text)
        if conditional:
            condition, yes, no = conditional.groups()
            result[name] = [yes] if feasible and condition == 'true' else [no] if feasible and condition == 'false' else [yes, no]
        else:
            jump = re.search(r'br label %([\w.$-]+)', text)
            result[name] = [jump.group(1)] if jump else []
    return result


def reachable_nodes(edges, entry):
    seen, pending = set(), [entry]
    while pending:
        node = pending.pop()
        if node in seen: continue
        seen.add(node)
        pending.extend(edges.get(node, []))
    return seen


def dominators(edges, entry):
    reachable = reachable_nodes(edges, entry)
    dom = {node: {entry} if node == entry else set(reachable) for node in reachable}
    changed = True
    while changed:
        changed = False
        for node in reachable - {entry}:
            predecessors = [p for p in reachable if node in edges.get(p, [])]
            common = set.intersection(*(dom[p] for p in predecessors)) if predecessors else set()
            value = {node} | common
            if dom[node] != value: dom[node] = value; changed = True
    return dom


def all_paths_panic(edges, entry, panic):
    """Every feasible path must end at a reason-6 nonreturning block, not loop."""
    done, visiting = {}, set()
    def visit(node):
        if node in panic: return True
        if node in visiting or node not in edges: return False
        if node in done: return done[node]
        visiting.add(node)
        children = edges[node]
        ok = bool(children) and all(visit(child) for child in children)
        visiting.remove(node)
        done[node] = ok
        return ok
    return visit(entry)


def protected_array_geps(text, extent):
    # Fixtures deliberately name the protected live object `a`. Compound
    # initializer temporaries are independent storage, so their pre-guard GEPs
    # do not constitute the operation under test.
    return re.findall(r'^.*getelementptr(?: (?:inbounds|nuw|nusw))* \[' + str(extent) + r' x[^\n]*, ptr %a,[^\n]*$', text, re.M)


def panic_guard_problems(body, extent, variable, opt):
    """Validate CFG reachability, full-width failure edge and access dominance."""
    problems = []
    cfg = blocks(body)
    if not cfg: return ['missing main CFG']
    entry = next(iter(cfg))
    panic = {name for name, text in cfg.items() if re.search(r'call void @__stasha_panic\(i32 6\)', text) and 'unreachable' in text}
    if not panic: return ['missing nonreturning reason-6 panic block']
    feasible = cfg_edges(cfg, True)
    reached = reachable_nodes(feasible, entry)
    if not (panic & reached): problems.append('reason-6 panic is disconnected from entry')
    if opt:
        if not all_paths_panic(feasible, entry, panic): problems.append('optimized entry does not necessarily reach nonreturning reason-6 panic')
        if any(protected_array_geps(cfg.get(node, ''), extent) for node in reached):
            problems.append('optimized invalid body retains reachable protected array GEP')
        return problems
    guarded = []
    for name, text in cfg.items():
        branch = re.search(r'br i1 ([^,]+), label %([\w.$-]+), label %([\w.$-]+)', text)
        if branch and branch.group(2) in panic:
            condition, _, ok = branch.groups()
            comparison = re.search(re.escape(condition) + r' = icmp uge i64 (%[\w.$-]+|-?[0-9]+), ' + str(extent) + r'\b', text)
            if comparison or (not variable and condition == 'true'):
                guarded.append((name, ok, comparison.group(1) if comparison else None))
    if not guarded:
        problems.append('missing unsigned full-width extent guard and failing CFG edge' if variable else 'missing constant-failing or full-width guarded edge')
    graph = cfg_edges(cfg)
    dom = dominators(graph, entry)
    protected = [(name, gep) for name, text in cfg.items() for gep in protected_array_geps(text, extent)]
    if not protected: problems.append('missing protected array address under test')
    for block, gep in protected:
        index = re.search(r', ptr %a, i32 0, i64 (%[\w.$-]+|-?[0-9]+)(?:\s*$|,)', gep)
        dominating = [(guard, ok, operand) for guard, ok, operand in guarded
                      if guard in dom.get(block, set()) and ok in dom.get(block, set()) and guard != block]
        if not dominating:
            problems.append('protected array address is not dominated by successful guard edge')
            continue
        def same_index(operand):
            if not index: return False
            selected = index.group(1)
            if operand is not None: return operand == selected
            # A literal-invalid comparison can fold to `br i1 true` even at O0.
            # Accept only an i64 constant that independently fails that extent.
            return bool(re.fullmatch(r'-?[0-9]+', selected)) and (int(selected) & ((1 << 64) - 1)) >= extent
        if not any(same_index(operand) for _, _, operand in dominating):
            problems.append('protected array index does not match the dominating full-width guard operand')
    return problems


def exact_return_problems(body, expected):
    """Every feasible reachable normal return must be the expected i32 constant."""
    cfg = blocks(body)
    if not cfg: return ['missing return-value CFG']
    reachable = reachable_nodes(cfg_edges(cfg, True), next(iter(cfg)))
    returns = [line.strip() for name in reachable for line in cfg.get(name, '').splitlines()
               if re.match(r'\s*ret\b', line)]
    if not returns: return ['no feasible reachable normal return']
    for instruction in returns:
        if not re.fullmatch(r'ret i32 ' + str(expected) + r'(?:\s*,\s*![^\n]+)?', instruction):
            return [f'feasible normal return differs from exact i32 {expected}: {instruction}']
    return []


def inspect_ir(case, ir, opt):
    problems = []
    try:
        main = function_body(ir, 'main')
        for check in case.ir_checks:
            if isinstance(check, tuple):
                if check[0] == 'return':
                    body = function_body(ir, check[1])
                    problems.extend(check[1] + ': ' + problem for problem in exact_return_problems(body, check[2]))
                elif check[0] == 'guard_extent' and opt == 0:
                    if not re.search(r'icmp uge i64 [^,]+, ' + str(check[1]) + r'\b', main):
                        problems.append('missing full-width containing extent guard')
                    if not re.search(r'call void @__stasha_panic\(i32 6\)', main):
                        problems.append('missing containing extent reason-6 failure')
                elif check[0] == 'panic':
                    problems.extend(panic_guard_problems(main, check[1], check[2], opt))
                continue
            if check == 'no_bounds_panic':
                cfg = blocks(main)
                reachable = reachable_nodes(cfg_edges(cfg, True), next(iter(cfg))) if cfg else set()
                if any(re.search(r'call void @__stasha_panic\(i32 6\)', cfg[n]) for n in reachable):
                    problems.append('terminal one-past address can reach bounds panic')
                if not any(re.search(r'ret i32 0\b', cfg[n]) for n in reachable):
                    problems.append('terminal one-past address has no successful return')
            if opt != 0: continue
            if check == 'self_no_element_load':
                methods = re.findall(r'^define\b[^\n]*@[^\n]*query[^\n]*\{\n(.*?)^}', ir, re.M | re.S)
                if not methods: problems.append('missing query method for first-element-load inspection')
                if any(re.search(r'\bload i32\b', body) for body in methods):
                    problems.append('query method introduces an i32 first-element load')
            if check == 'no_pointer_element_load' and re.search(r'\bload ptr\b', main):
                problems.append('array query loads a stored pointer element')
            if check == 'owner_calls_twice' and len(re.findall(r'\bcall\b[^\n]*@get\(', main)) != 2:
                problems.append('owner/descriptor/pointer call not evaluated exactly once per query')
            if check == 'index_call_once' and len(re.findall(r'\bcall i32 @index\(', main)) != 1:
                problems.append('index expression not evaluated exactly once')
            if check == 'order_trace':
                events = re.findall(r'\bcall (?:ptr|i32) @(get|index)\(', main)
                if events != ['get', 'index', 'get', 'index']:
                    problems.append('owner/index call count or evaluation order mismatch')
                if len(re.findall(r'icmp uge i64 [^,]+, 2\b', main)) != 2:
                    problems.append('order fixture lacks separate full-width outer guards')
    except ValueError as error:
        problems.append(str(error))
    return problems


def diagnostic_problems(compiled, source, expected=None):
    text = compiled['stderr'] + '\n' + compiled['stdout']
    p = []
    if access.scalar_runner.INTERNAL_FAILURE.search(text): p.append('compiler internal/verifier/sanitizer failure')
    if access.scalar_runner.WARNING.search(text): p.append('unexpected compiler warning')
    if expected is not None:
        if compiled['category'] != 'exit' or compiled['returncode'] != 1: p.append('expected normal diagnostic exit 1')
        if not access.scalar_runner.ERROR.search(compiled['stderr']): p.append('missing stderr error diagnostic')
        if not re.search(expected, compiled['stderr'], re.I | re.M): p.append('missing owned diagnostic: ' + expected)
        if not re.search(re.escape(source.name) + r':[1-9][0-9]*(?::[1-9][0-9]*)?(?:\s|$)', compiled['stderr']): p.append('missing source span')
        if re.search(r'linking|archiving|link_object|archive_object', text, re.I): p.append('rejected source reached linker/archive stage')
    else:
        if compiled['category'] != 'exit' or compiled['returncode'] != 0: p.append('compiler did not exit successfully')
        if access.scalar_runner.ERROR.search(text): p.append('unexpected compiler diagnostic')
    return p


def start_receipt(case, opt, work):
    # Unique caller-owned directories prevent stale artifacts from passing.
    work.mkdir(parents=True, exist_ok=False)
    source = work / (case.name + '.sts')
    source.write_text(case.source)
    return source, dict(name=case.name, optimization=opt, outcome=case.outcome,
        source=str(source), source_sha256=sha256(source), never_executed=type(case) is CompileCase,
        expected_stdout='', expected_stderr='', ir_checks=case.ir_checks, problems=[])


def finish_receipt(work, result):
    result['artifacts'] = {str(p.relative_to(work)): sha256(p) for p in sorted(work.rglob('*'))
                           if p.is_file() and p.name != 'receipt.json'}
    result['status'] = 'FAIL' if result['problems'] else 'PASS'
    (work / 'receipt.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def run_compile_case(case, opt, args, work):
    if type(case) is not CompileCase: raise TypeError('IR-only dispatcher requires CompileCase')
    source, result = start_receipt(case, opt, work)
    ir_file = work / 'DO-NOT-RUN.ll'
    result['compile'] = capture([args.compiler, 'ir', source, '-o=' + str(opt), '-o', ir_file], work, work / 'compile', args.compile_timeout)
    result['problems'] += diagnostic_problems(result['compile'], source, case.diagnostic if case.outcome == 'rejection' else None)
    artifacts = [p for p in work.iterdir() if p.suffix in ('.ll', '.o', '.a', '.so', '.dylib', '.exe') or p.name == 'program']
    if case.outcome == 'rejection':
        if artifacts: result['problems'].append('rejected compilation left requested IR/object/archive/executable')
    elif not ir_file.is_file() or ir_file.is_symlink() or not ir_file.stat().st_size:
        result['problems'].append('fresh IR missing')
    elif not result['problems']:
        result['problems'] += inspect_ir(case, ir_file.read_text(), opt)
    return finish_receipt(work, result)


def run_runtime_case(case, opt, args, work):
    if type(case) is not RuntimeCase: raise TypeError('execution requires an explicitly admitted RuntimeCase')
    source, result = start_receipt(case, opt, work)
    binary, ir_file = work / 'program', work / 'query.ll'
    result['compile'] = capture([args.compiler, 'build', source, '-o=' + str(opt), '-o', binary], work, work / 'compile', args.compile_timeout, {'STS_DUMP_IR': str(ir_file)})
    result['problems'] += diagnostic_problems(result['compile'], source)
    if not binary.is_file() or binary.is_symlink() or not os.access(binary, os.X_OK): result['problems'].append('fresh executable missing')
    if not ir_file.is_file(): result['problems'].append('runtime query IR missing')
    elif not result['problems']: result['problems'] += inspect_ir(case, ir_file.read_text(), opt)
    if not result['problems']:
        executed = capture([binary], work, work / 'execute', args.run_timeout)
        result['run'] = executed
        if executed['category'] != 'exit' or executed['returncode'] != 0: result['problems'].append('runtime did not exit normally with 0')
        if executed['stdout'] != '': result['problems'].append('runtime stdout is not exactly empty')
        if executed['stderr'] != '': result['problems'].append('runtime stderr is not exactly empty')
    return finish_receipt(work, result)


def run_case(case, opt, args, work):
    # Type, not a mutable expected-outcome string, is the execution capability.
    if type(case) is RuntimeCase: return run_runtime_case(case, opt, args, work)
    if type(case) is CompileCase: return run_compile_case(case, opt, args, work)
    raise TypeError('unknown fixture type')


def source_provenance():
    paths = ['src/codegen/cg_expr.c', 'src/codegen/cg_access.c', 'src/codegen/cg_scalar.c',
        'src/codegen/cg_iface_locals.c', 'src/codegen/cg_iface_sources.c', 'src/codegen/cg_stmt.c',
        'src/codegen/cg_registry.c', 'src/codegen/codegen.c', 'src/common/abi.h',
        'src/parser/parse_expr.c', 'src/parser/parse_stmt.c', 'src/ast/ast.h',
        'tests/memory/run_array_queries.py', 'tests/memory/test_array_queries.py',
        'tests/memory/run_access_contracts.py', 'tests/scalars/run.py']
    return {'working_tree_sha256': {p: sha256(ROOT / p) for p in paths if (ROOT / p).is_file()},
            'note': 'Working-tree hashes are evidence inputs, not an assertion of compiler build identity.'}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', type=Path)
    p.add_argument('--opt', default='0,1,2,3')
    p.add_argument('--artifacts', type=Path)
    p.add_argument('--case', action='append', default=[])
    p.add_argument('--list', action='store_true')
    p.add_argument('--compile-only', action='store_true', help='select only structurally IR-only cases')
    args = p.parse_args(argv)
    all_cases = cases()
    selected = [c for c in all_cases if (not args.case or c.name in args.case) and (not args.compile_only or type(c) is CompileCase)]
    if not selected or set(args.case) - {c.name for c in selected}: p.error('empty or unknown case selection')
    try: opts = [int(v) for v in args.opt.split(',')]
    except ValueError: p.error('optimization levels must be distinct values in 0,1,2,3')
    if not opts or len(opts) != len(set(opts)) or any(v not in range(4) for v in opts): p.error('optimization levels must be distinct values in 0,1,2,3')
    if args.list:
        print(json.dumps([{'name': c.name, 'outcome': c.outcome, 'can_execute': type(c) is RuntimeCase, 'source_sha256': hashlib.sha256(c.source.encode()).hexdigest()} for c in selected], indent=2))
        return 0
    if not args.compiler: p.error('--compiler is required unless --list is used')
    args.compiler = args.compiler.resolve(strict=True)
    args.compile_timeout, args.run_timeout = 30, 5
    if args.artifacts: args.artifacts.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='array-queries-', dir=args.artifacts)).resolve()
    before = sha256(args.compiler)
    summary = {'utc': datetime.now(timezone.utc).isoformat(), 'host': platform.platform(),
        'compiler': str(args.compiler), 'compiler_sha256': before, 'baseline_compiler_sha256': BASELINE_SHA256,
        'plan_sha256': PLAN_SHA256, 'source': source_provenance(), 'optimizations': opts,
        'manifest_counts': dict(Counter(c.outcome for c in all_cases)),
        'selected_counts': dict(Counter(c.outcome for c in selected)),
        'qualification_scope': 'newly compiled fixed-array query bodies; compile-only controls never execute',
        'plan_adjustments': ['Approved: I1b.2 query-derived concrete-array indexes use a typed scalar temporary; original inline len/cap indexes remain expected compile-only rejections under unchanged source-fact rules.'],
        'independent_gates': ['Exact original-AST hooks and guard review', 'Genuine retained and rebuilt ABI9 provider experiment', 'Existing pointer-permission, slice-view, A1 and I1b.2 suites'],
        'results': []}
    for case in selected:
        for opt in opts:
            result = run_case(case, opt, args, work / (case.name + '-O' + str(opt)))
            summary['results'].append(result)
            print(result['status'], case.name, 'O' + str(opt), '; '.join(result['problems']), flush=True)
    summary['compiler_sha256_after'] = sha256(args.compiler)
    summary['compiler_unchanged'] = before == summary['compiler_sha256_after']
    summary['counts'] = dict(Counter(r['status'] for r in summary['results']))
    summary['status'] = 'PASS' if summary['compiler_unchanged'] and all(r['status'] == 'PASS' for r in summary['results']) else 'FAIL'
    summary['full_matrix'] = len(selected) == len(all_cases) and sorted(opts) == [0,1,2,3]
    (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(summary['status'], summary['counts'], work / 'summary.json')
    return int(summary['status'] != 'PASS')


if __name__ == '__main__':
    raise SystemExit(main())
