'use strict';

// Runs emitted JavaScript with mocked VS Code, child process and target-only I/O.
// No editor host, compiler, generated program or filesystem discovery is started.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');
const providerPath = process.env.STASHA_DEFINITION_PROVIDER ||
    path.join(__dirname, '..', 'out', 'definitionProvider.js');
const providerCode = fs.readFileSync(providerPath, 'utf8');
const compilerPath = '/compiler with spaces/bin/stasha';
const sourcePath = '/project/coordinates.sts';
const sourceText = 'mod coordinates;\nfn target(void): i32 { ret 7; }\nfn caller(void): i32 {\n    ret target();\n}\n';
const unicodeLines = ['mod coordinates;', '/* é😀 */ fn target(void): i32 { ret 7; }',
    'fn caller(void): i32 {', '    /* é😀 */ ret target();', '}'];
const targetPath = '/project/helper.sts';
const targetText = 'mod helper;\n/* π */ fn target(void): i32 { ret 7; }\n';
const definition = { path: sourcePath, line: 1, col: 3, name: 'target', kind: 'function' };
const response = (changes = {}) => JSON.stringify({ definition: { ...definition, ...changes } });
const crossResponse = (changes = {}) => response({ path: targetPath, col: 12, ...changes });

function fileUri(filePath, scheme = 'file') {
    return { fsPath: filePath, scheme, toString() { return scheme + ':' + filePath; } };
}
function document(filePath, text, changes = {}) {
    return { uri: fileUri(filePath), text, version: 1, isDirty: false, isClosed: false,
        isUntitled: false, getText() { return this.text; },
        save() { throw new Error('must never save a document'); }, ...changes };
}

async function invoke(stdout = response(), options = {}) {
    const calls = [], reads = [], opens = [], apiCalls = [];
    const doc = document(options.sourcePath ?? sourcePath, options.text ?? sourceText, options.doc);
    const target = document(options.targetPath ?? targetPath, options.targetText ?? targetText, options.target);
    const documents = [doc, target];
    const token = { isCancellationRequested: false, ...options.token };
    const state = { doc, target, documents, token,
        folder: options.root === undefined ? undefined : { uri: fileUri(options.root), name: 'project', index: 0 } };
    if (options.folder) state.folder = options.folder;
    const vscode = {
        workspace: {
            get textDocuments() { return state.documents; },
            getWorkspaceFolder(uri) {
                assert.equal(uri, doc.uri);
                return state.folder;
            },
            async openTextDocument(uri) {
                opens.push(uri.fsPath);
                assert.equal(arguments.length, 1, 'never override encoding');
                if (options.openError) throw new Error('open failed');
                if (options.onOpen) await options.onOpen(state);
                return state.target;
            },
        },
        Range: class Range {
            constructor(startLine, startCharacter, endLine, endCharacter) {
                apiCalls.push('Range');
                for (const coordinate of [startLine, startCharacter, endLine, endCharacter]) {
                    assert.ok(Number.isSafeInteger(coordinate) && coordinate >= 0);
                }
                this.start = { line: startLine, character: startCharacter };
                this.end = { line: endLine, character: endCharacter };
            }
        },
        Uri: { file(filePath) { apiCalls.push('Uri.file'); return fileUri(options.uriPath ?? filePath); } },
        Location: class Location {
            constructor(uri, range) { apiCalls.push('Location'); this.uri = uri; this.range = range; }
        },
    };
    const module = { exports: {} };
    vm.runInNewContext(providerCode, {
        module, exports: module.exports, Buffer,
        require(name) {
            if (name === 'vscode') return vscode;
            if (name === 'path') return options.hostPath ?? path;
            if (name === 'fs') return { promises: { async readFile(filePath) {
                reads.push(filePath);
                assert.equal(arguments.length, 1, 'read raw bytes only');
                if (options.onRead) await options.onRead(state, reads.length);
                if (options.readError === reads.length) throw new Error('read failed');
                const bytes = options.bytes?.[Math.min(reads.length - 1, options.bytes.length - 1)];
                return Buffer.from(bytes ?? target.text, typeof bytes === 'string' || bytes === undefined ? 'utf8' : undefined);
            } } };
            if (name === './compilerPath') return { getCompilerPath: () => compilerPath };
            if (name === 'child_process') return { spawnSync(bin, args, spawnOptions) {
                calls.push({ bin, args: Array.from(args), options: { ...spawnOptions } });
                if (options.onSpawn) options.onSpawn(state);
                if (options.spawnError) throw new Error('spawn failed');
                return { status: 0, stdout, ...options.process };
            } };
            throw new Error('Unexpected dependency: ' + name);
        },
    }, { filename: providerPath });
    const original = doc.text;
    const pos = options.position ?? { line: 3, character: 8 };
    const location = await new module.exports.StashaDefinitionProvider().provideDefinition(doc, pos, token);
    for (const call of calls) {
        assert.equal(call.bin, compilerPath);
        assert.deepEqual(call.args.slice(0, 4), ['definition', '--stdin', '--path', options.sourcePath ?? sourcePath]);
        assert.deepEqual(call.args.slice(4, 7), ['--line', String(pos.line), '--col']);
        assert.equal(call.options.encoding, 'utf8');
        assert.equal(call.options.shell, undefined);
        assert.ok(Buffer.isBuffer(call.options.input));
        assert.deepEqual(call.options.input, Buffer.from(original.replace(/\r\n/g, '\n')));
        assert.deepEqual(Object.keys(call.options).sort(), ['encoding', 'input']);
    }
    assert.ok(calls.length <= 1, 'no retry or query snapping');
    for (const p of reads) assert.equal(p, options.uriPath ?? options.targetPath ?? targetPath, 'read only returned target');
    return { location, calls, reads, opens, apiCalls, state };
}
function expectLocation(result, filePath, line, start, end) {
    assert.ok(result.location, 'expected a definition location');
    assert.equal(result.location.uri.fsPath, filePath);
    assert.deepEqual(result.location.range.start, { line, character: start });
    assert.deepEqual(result.location.range.end, { line, character: end });
    assert.equal(result.calls.length, 1);
}
async function expectAbsent(stdout, options = {}, spawned = 1) {
    const result = await invoke(stdout, options);
    assert.equal(result.location, undefined);
    assert.ok(!result.apiCalls.includes('Range') && !result.apiCalls.includes('Location'));
    assert.equal(result.calls.length, spawned);
    return result;
}

// Exact previous producer fixture, whose relative source path is outside the
// host-absolute snapshot contract. The stdin/path equivalent retains its JSON.
test('preserves producer JSON and maps the ordinary target/caller fixture', async () => {
    const exact = '{"definition":{"path":"coordinates.sts","line":1,"col":3,"name":"target","kind":"function"}}';
    await expectAbsent(exact);
    const result = await invoke(response());
    expectLocation(result, sourcePath, 1, 3, 9);
    assert.equal(result.location.uri, result.state.doc.uri);
    assert.deepEqual(result.reads, []);
    assert.deepEqual(result.opens, []);
    assert.equal(result.calls[0].args.at(-1), '8');
});

test('zero coordinates, contextual identifier, final newline and no final newline', async () => {
    for (const text of ['target', 'target\n', 'target\n\n']) {
        expectLocation(await invoke(response({ line: 0, col: 0 }), { text, position: { line: 0, character: 0 } }),
            sourcePath, 0, 0, 6);
    }
    expectLocation(await invoke(response({ line: 0, col: 0, name: 'match' }), {
        text: 'match', position: { line: 0, character: 5 },
    }), sourcePath, 0, 0, 5);
});

test('preserves spaces and Unicode in exact caller/compiler paths', async () => {
    const filePath = '/project é😀/caller file.sts';
    expectLocation(await invoke(response({ path: filePath }), { sourcePath: filePath }), filePath, 1, 3, 9);
});

test('accepts surrounding whitespace/unused metadata, ignores obsolete len/file', async () => {
    expectLocation(await invoke(' \n' + response({ kind: 'future-kind', extra: true, len: 100, file: 'wrong.sts' }) + '\n\t'),
        sourcePath, 1, 3, 9);
});

for (const [eol, text] of [['LF', unicodeLines.join('\n')], ['CRLF', unicodeLines.join('\r\n')],
    ['mixed', unicodeLines[0] + '\r\n' + unicodeLines.slice(1).join('\n')]]) {
    for (const host of ['POSIX', 'Windows mock']) {
        test(`${eol} ${host}: exact dirty same-file 18/21 query and 16/13 target oracle`, async () => {
            const filePath = host === 'POSIX' ? sourcePath : 'C:\\project é😀\\coordinates.sts';
            const result = await invoke(response({ path: filePath, col: 16 }), { text, sourcePath: filePath,
                hostPath: host === 'POSIX' ? path.posix : path.win32, doc: { isDirty: true },
                position: { line: 3, character: 18 } });
            expectLocation(result, filePath, 1, 13, 19);
            assert.equal(result.calls[0].args.at(-1), '21');
            assert.equal(result.calls[0].options.input.toString(), unicodeLines.join('\n'));
            assert.equal(result.state.doc.text, text);
            assert.equal(result.state.doc.isDirty, true);
            assert.deepEqual(result.reads, []);
            assert.deepEqual(result.opens, []);
        });
    }
}

test('dirty updated caller text is the sole same-file source, without disk reads', async () => {
    for (const prefix of ['/* saved */ ', '/* unsaved é😀 */ ', '/* changed again π */ ']) {
        const text = prefix + 'fn target(void): i32 { ret 7; }';
        const result = await invoke(response({ line: 0, col: Buffer.byteLength(prefix + 'fn ') }),
            { text, doc: { isDirty: true, version: 10 }, position: { line: 0, character: 0 } });
        expectLocation(result, sourcePath, 0, prefix.length + 3, prefix.length + 9);
        assert.deepEqual(result.reads, []);
    }
});

for (const [name, text] of [
    ['CRLF code between single-line strings', '"one";\r\n\'two\';\r\n'],
    ['nested comments with quotes/backticks', '/* "\'`\r\n /* nested */ end */\r\n'],
    ['line comments with quotes/backticks', '// "\'`\r\n'],
    ['LF multiline ordinary string', '"one\ntwo";\n'],
    ['comment delimiters inside string', '"// /* */";\r\n'],
    ['escaped quote', '"a\\"b";\r\n'],
    ['even backslash parity', '"a\\\\";\r\n'],
    ['literal backslash n', '"a\\nb";\r\n'],
    ['backtick escaped quote', '`\\\'`;\r\n'],
]) {
    test('caller admission accepts ' + name, async () => {
        const line = text.split('\n').length - 1;
        expectLocation(await invoke(response({ line, col: 3 }), { text: text + 'fn target(void): i32 { ret 7; }',
            position: { line, character: 3 } }), sourcePath, line, 3, 9);
    });
}
for (const [name, text] of [
    ['lone CR', '// comment\rfn target'], ['NUL', '// \0'], ['Ctrl-Z', '// \x1a'],
    ['initial BOM', '\ufeff' + sourceText], ['unpaired high surrogate', '// \ud800'],
    ['unpaired low surrogate', '// \udc00'], ['bad pair', '// \ud800a'],
    ['escaped physical LF', '// backslash\\\n'], ['escaped physical CRLF', '// backslash\\\r\n'],
    ['double string CRLF', '"one\r\ntwo"'], ['single string CRLF', "'one\r\ntwo'"],
    ['double dollar CRLF', '$"one\r\ntwo"'], ['single dollar CRLF', "$'one\r\ntwo'"],
    ['backtick CRLF', '`\r\n`'], ['backtick LF', '`\n`'],
    ['unterminated quote', '"a'], ['unterminated escaped quote', '"a\\"'],
    ['unterminated nested comment', '/* outer /* inner */'],
    ['unterminated backtick', '`a'], ['literal hides fake comment closing', '"/*\r\n*/"'],
]) {
    test('caller rejects ' + name + ' before spawn', async () => {
        const result = await expectAbsent(response(), { text }, 0);
        assert.deepEqual(result.reads, []); assert.deepEqual(result.opens, []);
    });
}

test('all scalar boundaries convert query UTF-16 exactly; target uses own byte boundary', async () => {
    const prefix = '/*\té€😀e\u0301� */ fn ';
    const text = prefix + 'target';
    let units = 0, bytes = 0;
    for (const scalar of [...text, '']) {
        const result = await invoke(response({ line: 0, col: Buffer.byteLength(prefix) }),
            { text, position: { line: 0, character: units } });
        expectLocation(result, sourcePath, 0, prefix.length, prefix.length + 6);
        assert.equal(result.calls[0].args.at(-1), String(bytes));
        units += scalar.length; bytes += Buffer.byteLength(scalar);
    }
});
for (const col of [4, 6, 7, 8]) {
    test('rejects target inside multibyte scalar at byte ' + col, async () => {
        await expectAbsent(response({ col }), { text: unicodeLines.join('\n'), position: { line: 3, character: 18 } });
    });
}
for (const prefix of ['/* é */ fn ', '/* € */ fn ', '/* 😀 */ fn ', '/* e\u0301 */ fn ', '\tfn ']) {
    test('maps verified target after scalar prefix ' + JSON.stringify(prefix), async () => {
        expectLocation(await invoke(response({ line: 0, col: Buffer.byteLength(prefix) }),
            { text: prefix + 'target', position: { line: 0, character: 0 } }), sourcePath, 0, prefix.length, prefix.length + 6);
    });
}
for (const position of [{ line: 1, character: 5 }, { line: -1, character: 0 }, { line: 0.5, character: 0 },
    { line: 999, character: 0 }, { line: 0, character: -1 }, { line: 0, character: 0.5 },
    { line: 0, character: Number.MAX_SAFE_INTEGER }, { line: Number.MAX_SAFE_INTEGER + 1, character: 0 },
    { line: 0, character: Infinity }, { line: 0, character: NaN }]) {
    test('rejects invalid query boundary ' + JSON.stringify(position), async () => {
        await expectAbsent(response(), { text: unicodeLines.join('\n'), position }, 0);
    });
}
test('blank line and trailing final empty line query are valid EOL boundaries', async () => {
    for (const position of [{ line: 1, character: 0 }, { line: 2, character: 0 }]) {
        const result = await invoke(response({ line: 0, col: 0 }), { text: 'target\n\n', position });
        expectLocation(result, sourcePath, 0, 0, 6);
    }
});
for (const [name, text, changes] of [
    ['name mismatch', 'fn other()', {}], ['name prefix', 'fn target_more()', {}],
    ['name suffix', 'fn mytarget()', { col: 5 }], ['Unicode ordinary name', 'fn π()', { name: 'π' }],
    ['inside comment', '/* target */', {}], ['inside string', '"  target"', {}],
    ['target EOL', 'fn ', {}], ['missing target line', sourceText, { line: 1000 }],
    ['large safe target column', sourceText, { col: Number.MAX_SAFE_INTEGER }],
]) {
    test('rejects unverified token: ' + name, async () => {
        await expectAbsent(response({ line: 0, col: 3, ...changes }), { text, position: { line: 0, character: 0 } });
    });
}
for (const quote of ['"', "'"]) {
    for (const body of ['π😀 x', 'space name', 'a\\' + quote + 'b', 'a\\\\b', 'a\\😀b']) {
        test('full dollar token range ' + quote + body, async () => {
            const token = '$' + quote + body + quote;
            expectLocation(await invoke(response({ line: 0, col: 3, name: body }), {
                text: 'fn ' + token + '(void): i32 { ret 7; }', position: { line: 0, character: 3 },
            }), sourcePath, 0, 3, 3 + token.length);
        });
    }
}
test('exact quoted oracle has range [3,11)', async () => {
    expectLocation(await invoke(response({ line: 0, col: 3, name: 'π😀 x' }), {
        text: 'fn $"π😀 x"(void): i32 { ret 7; }', position: { line: 0, character: 3 },
    }), sourcePath, 0, 3, 11);
});
for (const [text, name] of [['fn $"other"()', 'target'], ['fn $"multi\nline"()', 'multi\nline']]) {
    test('rejects mismatching/multiline dollar target ' + JSON.stringify(name), async () => {
        await expectAbsent(response({ line: 0, col: 3, name }), { text, position: { line: 0, character: 0 } });
    });
}
test('short quoted-name body compiler-null is preserved without query snapping', async () => {
    const result = await expectAbsent('{"definition":null}', { text: 'fn $"π"()', position: { line: 0, character: 5 } });
    assert.equal(result.calls[0].args.at(-1), '5');
});

for (const [name, doc] of [['dirty allowed', { isDirty: true }], ['no text save', {}]]) {
    test('caller supports ' + name, async () => {
        expectLocation(await invoke(response(), { doc, onSpawn(s) { s.doc.isDirty = false; } }), sourcePath, 1, 3, 9);
    });
}
for (const [name, options] of [
    ['closed', { doc: { isClosed: true } }], ['untitled', { doc: { isUntitled: true } }],
    ['non-file', { doc: { uri: fileUri(sourcePath, 'vscode-remote') } }],
    ['relative path', { sourcePath: 'coordinates.sts' }], ['empty path', { sourcePath: '' }],
    ['NUL path', { sourcePath: '/project/a\0.sts' }], ['foreign path', { sourcePath: 'C:\\project\\a.sts' }],
    ['bad version', { doc: { version: NaN } }], ['initial cancellation', { token: { isCancellationRequested: true } }],
]) {
    test('rejects caller ' + name + ' before spawn', () => expectAbsent(response(), options, 0));
}

const mutations = {
    'changed version': s => s.doc.version++,
    'changed text same version': s => { s.doc.text += ' '; },
    'edit and undo': s => { const old = s.doc.text; s.doc.text += 'x'; s.doc.version++; s.doc.text = old; s.doc.version++; },
    'closed caller': s => { s.doc.isClosed = true; },
    'reopened caller object': s => { s.documents = [document(sourcePath, s.doc.text), s.target]; },
    'changed URI identity': s => { s.doc.uri = fileUri(sourcePath); },
    'changed URI path': s => { s.doc.uri.fsPath += '.other'; },
    'cancellation': s => { s.token.isCancellationRequested = true; },
};
for (const [name, mutate] of Object.entries(mutations)) {
    test('discards after synchronous process: ' + name, () => expectAbsent(response(), { onSpawn: mutate }));
    for (const boundary of ['open', 'first read', 'final read']) {
        test(`discards caller ${name} at ${boundary}`, async () => {
            await expectAbsent(crossResponse(), { onOpen: boundary === 'open' ? mutate : undefined,
                onRead: (s, n) => { if (n === (boundary === 'first read' ? 1 : boundary === 'final read' ? 2 : 0)) mutate(s); } });
        });
    }
}
for (const boundary of ['open', 'first read', 'final read']) {
    test('original EOL-only change invalidates despite equal derived text at ' + boundary, async () => {
        const mutate = s => { s.doc.text = s.doc.text.replace(/\r\n/g, '\n'); };
        await expectAbsent(crossResponse(), { text: sourceText.replace(/\n/g, '\r\n'),
            onOpen: boundary === 'open' ? mutate : undefined,
            onRead: (s, n) => { if (n === (boundary === 'first read' ? 1 : boundary === 'final read' ? 2 : 0)) mutate(s); } });
    });
}

test('cross-file uses target-own 12/11 oracle and reads only the admitted file twice', async () => {
    const result = await invoke(crossResponse(), { text: unicodeLines.join('\n'), position: { line: 3, character: 18 }, doc: { isDirty: true } });
    expectLocation(result, targetPath, 1, 11, 17);
    assert.equal(result.calls[0].args.at(-1), '21');
    assert.equal(result.location.uri, result.state.target.uri);
    assert.deepEqual(result.opens, [targetPath]); assert.deepEqual(result.reads, [targetPath, targetPath]);
});

test('normal dotted-import response supports nested target under caller directory', async () => {
    const dest = '/project/helpers/math.sts';
    const result = await invoke(crossResponse({ path: dest }), { text: sourceText.replace('mod coordinates;', 'mod coordinates; imp helpers.math;'), targetPath: dest });
    expectLocation(result, dest, 1, 11, 17);
});
for (const dest of ['/project/helper.sts', '/project/lib/helper.sts', '/project/app/nested/helper.sts']) {
    test('containing workspace admits parent/sibling/nested returned target ' + dest, async () => {
        expectLocation(await invoke(crossResponse({ path: dest }), { sourcePath: '/project/app/main.sts', root: '/project', targetPath: dest }),
            dest, 1, 11, 17);
    });
}
for (const [name, folder] of [['non-file folder', { uri: fileUri('/project', 'vscode-remote') }],
    ['relative folder', { uri: fileUri('project') }], ['non-containing folder', { uri: fileUri('/elsewhere') }]]) {
    test('explicit caller-directory fallback for ' + name, async () => {
        expectLocation(await invoke(crossResponse(), { folder }), targetPath, 1, 11, 17);
    });
}
for (const dest of ['/outside/helper.sts', '/project2/helper.sts', '/other-workspace/helper.sts',
    '/project/../outside.sts', '/project/header.h', '/project/HELPER.STS', 'relative.sts', '../helper.sts',
    'C:\\project\\helper.sts', 'file:///project/helper.sts', '/project/helper.sts\0']) {
    test('rejects out-of-scope target before I/O ' + JSON.stringify(dest), async () => {
        const result = await expectAbsent(crossResponse({ path: dest }), { root: '/project' });
        assert.deepEqual(result.opens, []); assert.deepEqual(result.reads, []);
    });
}
test('standalone fallback excludes parent/sibling outside caller directory', async () => {
    const result = await expectAbsent(crossResponse(), { sourcePath: '/project/app/main.sts' });
    assert.deepEqual(result.opens, []); assert.deepEqual(result.reads, []);
});
test('requested file URI native spelling, not raw path, identifies target', async () => {
    expectLocation(await invoke(crossResponse({ path: '/project/app/../helper.sts' }), { uriPath: targetPath }), targetPath, 1, 11, 17);
});
for (const boundary of ['open', 'first read', 'final read']) {
    for (const kind of ['removed', 'changed', 'same-path replacement', 'fallback becomes workspace']) {
        test(`root ${kind} at ${boundary} invalidates`, async () => {
            const mutate = s => { s.folder = kind === 'removed' ? undefined : {
                uri: fileUri(kind === 'changed' ? '/project/subdir' : '/project'), name: 'new', index: 0 }; };
            await expectAbsent(crossResponse(), { root: kind === 'fallback becomes workspace' ? undefined : '/project',
                onOpen: boundary === 'open' ? mutate : undefined,
                onRead: (s, n) => { if (n === (boundary === 'first read' ? 1 : boundary === 'final read' ? 2 : 0)) mutate(s); } });
        });
    }
}
for (const [name, prefix] of [
    ['raw CRLF ordinary lines', 'mod helper;\r\n'],
    ['mixed raw EOL', '// first\r\nmod helper;\n'],
    ['ordinary multiline CRLF literal', 'mod helper;\r\n"one\r\ntwo";\r\n'],
    ['raw backslash CRLF before declaration', '// continuation\\\r\n'],
    ['raw backslash CRLF inside ordinary literal', '"one\\\r\ntwo";\r\n'],
]) {
    test('target role keeps ' + name + ' unchanged', async () => {
        const line = prefix.split('\n').length - 1;
        const text = prefix + '/* π */ fn target(void): i32 { ret 7; }\r\n';
        const result = await invoke(crossResponse({ line }), { targetText: text, bytes: [Buffer.from(text)] });
        expectLocation(result, targetPath, line, 11, 17);
        assert.equal(result.state.target.text, text);
    });
}
for (const [name, options] of [
    ['dirty', { target: { isDirty: true } }], ['closed', { target: { isClosed: true } }],
    ['untitled', { target: { isUntitled: true } }], ['non-file', { target: { uri: fileUri(targetPath, 'remote') } }],
    ['wrong document path', { target: { uri: fileUri('/project/wrong.sts') } }],
    ['invalid UTF-8', { bytes: [Buffer.from([0xc0, 0xaf])] }],
    ['legacy bytes', { bytes: [Buffer.from([0xe9])] }], ['UTF-16 bytes', { bytes: [Buffer.from(targetText, 'utf16le')] }],
    ['raw BOM', { bytes: [Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from(targetText)])] }],
    ['text BOM', { targetText: '\ufeff' + targetText }], ['disk text mismatch', { bytes: [Buffer.from('different')] }],
    ['changed final disk bytes', { bytes: [Buffer.from(targetText), Buffer.from(targetText + ' ')] }],
    ['normalized editor CRLF mismatch', { bytes: [Buffer.from(targetText.replace(/\n/g, '\r\n'))] }],
    ['bare CR', { targetText: '//\r' + targetText }], ['NUL', { targetText: targetText + '\0' }],
    ['Ctrl-Z', { targetText: targetText + '\x1a' }], ['unpaired surrogate', { targetText: targetText + '\ud800' }],
    ['escaped LF', { targetText: '//\\\n' + targetText }], ['physical LF backtick', { targetText: '`\n`\n' + targetText }],
    ['physical CRLF backtick', { targetText: '`\r\n`\n' + targetText }],
    ['open failure', { openError: true }], ['first read failure', { readError: 1 }], ['final read failure', { readError: 2 }],
]) {
    test('rejects target ' + name, () => expectAbsent(crossResponse(), options));
}
const targetMutations = {
    'version': s => s.target.version++, 'text': s => { s.target.text += ' '; },
    'dirty': s => { s.target.isDirty = true; }, 'closed': s => { s.target.isClosed = true; },
    'URI': s => { s.target.uri = fileUri(targetPath); },
    'reopened object': s => { s.documents = [s.doc, document(targetPath, s.target.text)]; },
    'edit and undo': s => { const old = s.target.text; s.target.text += 'x'; s.target.version++; s.target.text = old; s.target.version++; },
};
for (const [name, mutate] of Object.entries(targetMutations)) {
    for (const count of [1, 2]) {
        test(`target ${name} change during read ${count} invalidates`, () => expectAbsent(crossResponse(), {
            onRead: (s, n) => { if (n === count) mutate(s); },
        }));
    }
}
test('cross-file current agreement is not proof of historical imported bytes (ABA limitation)', async () => {
    // The mock producer gives no imported-source identity; an unobserved older
    // target may have had the same token at this coordinate. Current agreement
    // necessarily passes and must not be described as historical validation.
    expectLocation(await invoke(crossResponse(), { bytes: [Buffer.from(targetText), Buffer.from(targetText)] }), targetPath, 1, 11, 17);
});

for (const [name, value] of [
    ['explicit no result', { definition: null }], ['missing definition', {}], ['top-level null', null],
    ['top-level array', []], ['top-level string', 'definition'], ['top-level number', 5], ['top-level boolean', true],
    ['definition array', { definition: [] }], ['definition string', { definition: 'target' }],
    ['definition number', { definition: 1 }], ['definition boolean', { definition: true }],
    ['definition empty object', { definition: {} }],
    ['old flat file/len protocol', { file: 'coordinates.sts', line: 1, col: 3, len: 6 }],
]) {
    test('returns undefined for ' + name, () => expectAbsent(JSON.stringify(value)));
}
for (const field of ['path', 'line', 'col', 'name']) {
    test('rejects missing ' + field, () => {
        const incomplete = { ...definition }; delete incomplete[field];
        return expectAbsent(JSON.stringify({ definition: incomplete }));
    });
}
for (const field of ['path', 'name']) {
    for (const value of ['', '\0', 'embedded\0nul', null, 7, true, [], {}, '\ud800', '\udc00']) {
        test('rejects invalid ' + field + ': ' + JSON.stringify(value), () => expectAbsent(response({ [field]: value })));
    }
}
for (const field of ['line', 'col']) {
    for (const value of [-1, 1.5, '1', null, true, [], {}, Number.MAX_SAFE_INTEGER + 1]) {
        test('rejects invalid ' + field + ': ' + JSON.stringify(value), () => expectAbsent(response({ [field]: value })));
    }
    test('rejects non-finite ' + field, () => expectAbsent(response().replace('"' + field + '":' + definition[field], '"' + field + '":1e400')));
}
for (const [name, stdout] of [
    ['empty output', ''], ['whitespace output', ' \n\t'], ['truncated JSON', '{"definition":'],
    ['unquoted key', '{definition:null}'], ['NaN', '{"definition":{"path":"a","line":NaN,"col":0,"name":"x"}}'],
    ['multiple objects', response() + '\n' + response()], ['diagnostic before', 'warning\n' + response()],
    ['diagnostic after', response() + '\nwarning'],
]) {
    test('rejects malformed output: ' + name, () => expectAbsent(stdout));
}
for (const [name, process] of [
    ['spawn error', { error: new Error('ENOENT'), status: null }],
    ['error despite zero', { error: new Error('failed'), status: 0 }], ['nonzero', { status: 1 }],
    ['terminated', { status: null, signal: 'SIGTERM' }], ['non-string stdout', { stdout: null }],
]) {
    test('ignores output after ' + name, () => expectAbsent(response(), { process }));
}
test('catches thrown process invocation', () => expectAbsent(response(), { spawnError: true }));


for (const scalar of ['é', '€', '😀', '�', '\u0301']) {
    for (let interior = 1; interior < Buffer.byteLength(scalar); interior++) {
        test(`rejects every continuation byte in ${scalar}: +${interior}`, () => expectAbsent(
            response({ line: 0, col: 3 + interior }), {
                text: '/* ' + scalar + ' */ fn target', position: { line: 0, character: 0 },
            }));
    }
}
test('target valid U+FFFD round-trips as UTF-8, rather than being mistaken for decoding error', async () => {
    const prefix = '/* � */ fn ';
    expectLocation(await invoke(crossResponse({ line: 0, col: Buffer.byteLength(prefix) }), {
        targetText: prefix + 'target(void): i32 { ret 7; }',
    }), targetPath, 0, prefix.length, prefix.length + 6);
});
test('cross-file no-text-change caller save leaves all guarded evidence valid', async () => {
    expectLocation(await invoke(crossResponse(), { doc: { isDirty: true }, onOpen(s) { s.doc.isDirty = false; } }),
        targetPath, 1, 11, 17);
});
for (const [name, mutate] of [['removed', s => { s.folder = undefined; }],
    ['path mutation', s => { s.folder.uri.fsPath = '/elsewhere'; }]]) {
    test('workspace root ' + name + ' during compiler invalidates before target I/O', async () => {
        const result = await expectAbsent(crossResponse(), { root: '/project', onSpawn: mutate });
        assert.deepEqual(result.opens, []); assert.deepEqual(result.reads, []);
    });
}
