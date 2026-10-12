'use strict';

// Run the checked-in provider's public Run profile with mocked workspace,
// compiler/symbol lookup, processes and VS Code. No compiler or editor is launched.
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test: nodeTest } = require('node:test');
// Lost process completion must fail a test instead of leaving the suite pending.
const test = (name, callback) => nodeTest(name, { timeout: 5000 }, callback);
const controllerPath = process.env.STASHA_TEST_CONTROLLER || path.join(__dirname, '..', 'out', 'testController.js');
const controllerCode = fs.readFileSync(controllerPath, 'utf8');
const compilerPath = '/compiler with spaces/bin/stasha';
const sourcePath = '/project with spaces/tests.sts';
const tick = () => new Promise(resolve => setImmediate(resolve));

function collection(parent) {
    const items = new Map();
    return {
        add(item) { item.parent = parent; items.set(item.id, item); },
        delete(id) { items.delete(id); },
        forEach(callback) { items.forEach(callback); },
        values() { return Array.from(items.values()); },
    };
}

async function harness(options = {}) {
    const files = options.files ?? [{ path: sourcePath, names: ['alpha', 'beta'] }];
    const calls = [], procs = [], results = [], started = [], subscriptions = [];
    const listeners = new Set();
    let handler, ended = 0, resolved = 0, disposed = 0;
    const run = {
        started(item) { started.push(item); },
        passed(item) { assert.equal(ended, 0); results.push({ item, status: 'passed' }); },
        failed(item, message) { assert.equal(ended, 0); results.push({ item, status: 'failed', message: message.message }); },
        end() { ended++; },
    };
    const ctrl = {
        items: collection(),
        createTestItem(id, label, uri) {
            const item = { id, label, uri }; item.children = collection(item); return item;
        },
        createRunProfile(label, kind, callback) {
            assert.equal(label, 'Run'); assert.equal(kind, 1); handler = callback;
        },
        createTestRun(req) { assert.equal(req, request); return run; },
        dispose() {},
    };
    const uri = filePath => ({ fsPath: filePath, toString() { return 'file:' + filePath; } });
    const noopSubscription = () => ({ dispose() {} });
    const vscode = {
        tests: { createTestController(id, label) {
            assert.equal(id, 'stasha'); assert.equal(label, 'Stasha Tests'); return ctrl;
        } },
        workspace: {
            async findFiles(include, exclude) {
                assert.equal(include, '**/*.sts'); assert.equal(exclude, '**/node_modules/**');
                return files.map(file => uri(file.path));
            },
            onDidOpenTextDocument: noopSubscription,
            onDidSaveTextDocument: noopSubscription,
            onDidDeleteFiles: noopSubscription,
        },
        TestRunProfileKind: { Run: 1 }, Range: class Range {},
        TestMessage: class TestMessage { constructor(message) { this.message = message; } },
    };
    const token = {
        onCancellationRequested(callback) {
            listeners.add(callback);
            if (options.cancelDuringSubscribe) callback();
            let active = true;
            return { dispose() {
                assert.ok(active, 'cancellation subscription disposed exactly once');
                active = false; disposed++; listeners.delete(callback);
            } };
        },
    };
    const module = { exports: {} };
    vm.runInNewContext(controllerCode, {
        module, exports: module.exports, Buffer,
        require(name) {
            if (name === 'vscode') return vscode;
            if (name === './compilerPath') return { getCompilerPath: () => compilerPath };
            if (name === './symbolProvider') return { getSymbols(filePath) {
                const file = files.find(file => file.path === filePath);
                assert.ok(file, 'only discover mocked files');
                return file.names.map((name, line) => ({ kind: 'test', name, line, col: 0 }));
            } };
            if (name === 'child_process') return { spawn(bin, args, spawnOptions) {
                const call = { bin, args: Array.from(args), options: { ...spawnOptions } }; calls.push(call);
                assert.equal(bin, compilerPath);
                assert.deepEqual(call.args, ['test', files[calls.length - 1].path], 'no extra flags or second binary launch');
                assert.deepEqual(call.options, { stdio: 'pipe' });
                if (Object.hasOwn(options, 'throwSpawn') && calls.length === (options.throwAt ?? 1)) throw options.throwSpawn;
                const proc = new EventEmitter();
                proc.stdout = new EventEmitter(); proc.stderr = new EventEmitter(); proc.kills = 0;
                proc.kill = () => { proc.kills++; if (options.onKill) options.onKill(proc); return true; };
                procs.push(proc); return proc;
            } };
            throw new Error('Unexpected dependency: ' + name);
        },
    }, { filename: controllerPath });
    assert.equal(module.exports.createTestController({ subscriptions }), ctrl);
    await tick(); // Finish mocked discovery before invoking Run.
    const request = options.select ? { include: options.select(ctrl.items.values()) } : {};
    const completion = handler(request, token);
    assert.ok(completion && typeof completion.then === 'function', 'Run exposes its completion promise');
    completion.then(() => { resolved++; });
    return {
        calls, procs, results, started, completion,
        get ended() { return ended; }, get resolved() { return resolved; }, get disposed() { return disposed; },
        cancel() { for (const callback of [...listeners]) callback(); },
        async done(statuses) {
            await completion; await tick();
            assert.equal(ended, 1, 'run ends exactly once'); assert.equal(resolved, 1, 'completion observed exactly once');
            assert.deepEqual(results.map(result => result.status), statuses);
            assert.equal(results.length, started.length, 'each started item gets exactly one terminal status');
            assert.equal(new Set(results.map(result => result.item)).size, results.length);
            assert.equal(listeners.size, 0, 'terminal processes release cancellation listeners');
        },
    };
}
function output(proc, stdout = '', stderr = '') {
    if (stdout) proc.stdout.emit('data', Buffer.from(stdout));
    if (stderr) proc.stderr.emit('data', Buffer.from(stderr));
}
const pass = 'PASS alpha\nPASS beta\n';

for (const text of [pass, '', 'compiler completed without per-test output\n']) {
    test('zero exit retains successful result behavior: ' + JSON.stringify(text), async () => {
        const h = await harness(); output(h.procs[0], text);
        assert.equal(h.results.length, 0); assert.equal(h.ended, 0);
        h.procs[0].emit('close', 0, null); await h.done(['passed', 'passed']); assert.equal(h.disposed, 1);
    });
}
test('explicit FAIL dominates PASS on zero exit', async () => {
    const h = await harness(); output(h.procs[0], pass + 'FAIL alpha: expected true\n');
    h.procs[0].emit('close', 0, null); await h.done(['failed', 'passed']);
    assert.match(h.results[0].message, /expected true/);
});
for (const code of [1, 2, 127, 255, -1]) {
    test('nonzero exit ' + code + ' dominates passing assertion lines for every item', async () => {
        const h = await harness(); output(h.procs[0], pass, 'leak gate failed: 1 live allocation\n');
        h.procs[0].emit('close', code, null); await h.done(['failed', 'failed']);
        for (const result of h.results) {
            assert.match(result.message, new RegExp('exited with code ' + code));
            assert.match(result.message, /PASS alpha/); assert.match(result.message, /leak gate failed/);
        }
    });
}
for (const [code, signal, diagnostic] of [
    [null, null, /did not exit normally/], [null, 'SIGTERM', /signal SIGTERM/],
    [null, 'SIGKILL', /signal SIGKILL/], [null, 'SIGABRT', /signal SIGABRT/],
    [0, 'SIGTERM', /signal SIGTERM/], [1, 'SIGTERM', /signal SIGTERM/],
]) {
    test(`abnormal exit ${code}/${signal} dominates PASS`, async () => {
        const h = await harness(); output(h.procs[0], pass); h.procs[0].emit('close', code, signal);
        await h.done(['failed', 'failed']);
        for (const result of h.results) assert.match(result.message, diagnostic);
    });
}
test('nonzero without output has an actionable failure', async () => {
    const h = await harness(); h.procs[0].emit('close', 1, null);
    await h.done(['failed', 'failed']); assert.match(h.results[0].message, /exited with code 1/);
});
for (const subsequentClose of [undefined, 0, 1, null]) {
    test('spawn error completes once even with later close ' + subsequentClose, async () => {
        const h = await harness(); const proc = h.procs[0]; output(proc, pass);
        proc.emit('error', new Error('ENOENT: compiler missing'));
        if (subsequentClose !== undefined) proc.emit('close', subsequentClose, null);
        proc.emit('error', new Error('repeated error')); proc.emit('close', 0, null);
        output(proc, 'PASS alpha\n', 'late output');
        await h.done(['failed', 'failed']); assert.equal(h.disposed, 1);
        for (const result of h.results) {
            assert.match(result.message, /Unable to run Stasha tests: ENOENT/);
            assert.match(result.message, /PASS alpha/); assert.doesNotMatch(result.message, /repeated|late/);
        }
    });
}
for (const text of ['', pass]) {
    test('spawn error without close still ends the run: ' + JSON.stringify(text), async () => {
        const h = await harness(); output(h.procs[0], text); h.procs[0].emit('error', new Error('EACCES'));
        await h.done(['failed', 'failed']); assert.match(h.results[0].message, /EACCES/);
    });
}
for (const thrown of [new Error('spawn threw'), 'bad executable', undefined]) {
    test('synchronous spawn throw becomes failed results: ' + String(thrown), async () => {
        const h = await harness({ throwSpawn: thrown }); await h.done(['failed', 'failed']);
        assert.equal(h.procs.length, 0); assert.equal(h.disposed, 0);
        assert.match(h.results[0].message, /Unable to run Stasha tests:/);
    });
}
for (const code of [0, 1, null]) {
    test('repeated terminal events do not duplicate results or completion: ' + code, async () => {
        const h = await harness(); const proc = h.procs[0]; output(proc, pass);
        proc.emit('close', code, null); proc.emit('close', 0, null); proc.emit('close', 1, 'SIGTERM');
        proc.emit('error', new Error('late event after close'));
        await h.done(code === 0 ? ['passed', 'passed'] : ['failed', 'failed']); assert.equal(h.disposed, 1);
    });
}
test('partial lines wait for close and cannot hide later process failure', async () => {
    const h = await harness(); const proc = h.procs[0];
    for (const chunk of ['PA', 'SS al', 'pha\nPA', 'SS be', 'ta']) output(proc, chunk);
    await tick(); assert.equal(h.results.length, 0); assert.equal(h.resolved, 0); assert.equal(h.ended, 0);
    output(proc, '', 'runtime gate failed'); proc.emit('close', 1, null);
    await h.done(['failed', 'failed']); assert.match(h.results[0].message, /PASS alpha\nPASS beta/);
});
test('partial FAIL lines and UTF-8 scalars decode across byte chunks', async () => {
    const name = 'é😀.[quoted](test)+?';
    const h = await harness({ files: [{ path: sourcePath, names: [name] }] });
    for (const byte of Buffer.from('PASS ' + name + '\nFAIL ' + name + ': split failure')) {
        h.procs[0].stdout.emit('data', Buffer.from([byte]));
    }
    h.procs[0].emit('close', 0, null); await h.done(['failed']); assert.match(h.results[0].message, /split failure/);
});
for (const stream of ['stdout', 'stderr']) {
    test('interleaved streams preserve a partial FAIL line in ' + stream, async () => {
        const h = await harness(); const proc = h.procs[0]; const other = stream === 'stdout' ? 'stderr' : 'stdout';
        proc[stream].emit('data', Buffer.from('FAIL al')); proc[other].emit('data', Buffer.from(pass));
        proc[stream].emit('data', Buffer.from('pha: interleaved failure')); proc.emit('close', 0, null);
        await h.done(['failed', 'passed']); assert.match(h.results[0].message, /interleaved failure/);
    });
}
test('multiple files wait for every process and keep statuses local', async () => {
    const h = await harness({ files: [
        { path: sourcePath, names: ['alpha', 'beta'] }, { path: '/project/second.sts', names: ['gamma'] },
    ] });
    output(h.procs[0], pass); h.procs[0].emit('close', 1, null);
    await tick(); assert.equal(h.ended, 0); assert.equal(h.resolved, 0);
    output(h.procs[1], 'PASS gamma'); h.procs[1].emit('close', 0, null);
    await h.done(['failed', 'failed', 'passed']); assert.equal(h.calls.length, 2); assert.equal(h.disposed, 2);
});
test('synchronous spawn failure in one file does not abandon another', async () => {
    const h = await harness({ throwSpawn: new Error('spawn unavailable'), files: [
        { path: sourcePath, names: ['alpha', 'beta'] }, { path: '/project/second.sts', names: ['gamma'] },
    ] });
    await tick(); assert.equal(h.ended, 0); h.procs[0].emit('close', 0, null);
    await h.done(['failed', 'failed', 'passed']); assert.equal(h.calls.length, 2);
});
test('selected leaves in one file use one unchanged compiler command', async () => {
    const h = await harness({ select: files => files[0].children.values() });
    output(h.procs[0], pass); h.procs[0].emit('close', 0, null);
    await h.done(['passed', 'passed']); assert.equal(h.calls.length, 1);
});
test('selected single leaf reports only that leaf', async () => {
    const h = await harness({ select: files => [files[0].children.values()[1]] });
    output(h.procs[0], pass); h.procs[0].emit('close', 1, null);
    await h.done(['failed']); assert.equal(h.results[0].item.label, 'beta');
});
test('empty discovery completes without spawning', async () => {
    const h = await harness({ files: [] }); await h.done([]); assert.equal(h.calls.length, 0);
});
test('cancellation-induced signal failure releases the listener', async () => {
    const h = await harness({ onKill: proc => proc.emit('close', null, 'SIGTERM') });
    output(h.procs[0], pass); h.cancel(); await h.done(['failed', 'failed']);
    assert.equal(h.procs[0].kills, 1); h.cancel(); assert.equal(h.procs[0].kills, 1);
});
test('synchronous cancellation terminal event releases newly returned listener once', async () => {
    const h = await harness({ cancelDuringSubscribe: true, onKill: proc => proc.emit('close', null, 'SIGTERM') });
    await h.done(['failed', 'failed']); assert.equal(h.disposed, 1);
});
