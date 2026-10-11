'use strict';

// No VS Code host or compiler process is started by these decoder tests.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

const providerPath = process.env.STASHA_DEFINITION_PROVIDER ||
    path.join(__dirname, '..', 'out', 'definitionProvider.js');
const providerCode = fs.readFileSync(providerPath, 'utf8');
const compilerPath = '/compiler with spaces/bin/stasha';

function invoke(stdout, options = {}) {
    const calls = [];
    const apiCalls = [];
    const vscode = {
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
        Uri: {
            file(filePath) {
                apiCalls.push('Uri.file');
                assert.equal(typeof filePath, 'string');
                return { fsPath: filePath };
            },
        },
        Location: class Location {
            constructor(uri, range) {
                apiCalls.push('Location');
                this.uri = uri;
                this.range = range;
            }
        },
    };
    const module = { exports: {} };
    vm.runInNewContext(providerCode, {
        module,
        exports: module.exports,
        require(name) {
            if (name === 'vscode') return vscode;
            if (name === './compilerPath') {
                return { getCompilerPath: () => compilerPath };
            }
            if (name === 'child_process') {
                return {
                    spawnSync(bin, args, spawnOptions) {
                        calls.push({ bin, args: Array.from(args), options: { ...spawnOptions } });
                        return { status: 0, stdout, ...options.process };
                    },
                };
            }
            throw new Error('Unexpected dependency: ' + name);
        },
    }, { filename: providerPath });
    const doc = {
        uri: { fsPath: options.sourcePath || 'coordinates.sts' },
        // The CLI reads the saved file. This test must not pretend otherwise.
        getText() { throw new Error('Unsaved-buffer support is outside this decoder'); },
    };
    const position = options.position || { line: 3, character: 8 };
    const provider = new module.exports.StashaDefinitionProvider();
    const location = provider.provideDefinition(doc, position);
    assert.deepEqual(calls, [{
        bin: compilerPath,
        args: ['definition', doc.uri.fsPath, '--line', String(position.line),
            '--col', String(position.character)],
        options: { encoding: 'utf8' },
    }]);
    return { location, apiCalls };
}

// Exact named-definition response from tests/driver/editor_coordinates.py:
// fn target(void): i32 { ret 7; }
// fn caller(void): i32 {\n    ret target();\n}
const definition = {
    path: 'coordinates.sts', line: 1, col: 3, name: 'target', kind: 'function',
};
const response = (changes = {}) => JSON.stringify({ definition: { ...definition, ...changes } });

function expectLocation(result, filePath, line, start, end) {
    assert.ok(result.location, 'expected a definition location');
    assert.equal(result.location.uri.fsPath, filePath);
    assert.deepEqual(result.location.range.start, { line, character: start });
    assert.deepEqual(result.location.range.end, { line, character: end });
    assert.deepEqual(result.apiCalls, ['Range', 'Uri.file', 'Location']);
}

function expectAbsent(stdout, options) {
    const result = invoke(stdout, options);
    assert.equal(result.location, undefined);
    assert.deepEqual(result.apiCalls, [], 'invalid output must not reach the VS Code API');
}

test('decodes the exact target/caller producer fixture and preserves zero-based cursor arguments', () => {
    expectLocation(invoke(response()), 'coordinates.sts', 1, 3, 9);
});

test('accepts zero line and column for both request and response', () => {
    expectLocation(invoke(response({ line: 0, col: 0 }), {
        position: { line: 0, character: 0 },
    }), 'coordinates.sts', 0, 0, 6);
});

test('preserves source, compiler and destination paths containing spaces', () => {
    const destination = '/workspace with spaces/library file.sts';
    expectLocation(invoke(response({ path: destination }), {
        sourcePath: '/workspace with spaces/caller file.sts',
    }), destination, 1, 3, 9);
});

test('preserves Unicode and Windows-style destination paths', () => {
    const destination = 'C:\\project é😀\\library file.sts';
    expectLocation(invoke(response({ path: destination })), destination, 1, 3, 9);
});

test('accepts surrounding JSON whitespace and unused producer metadata', () => {
    expectLocation(invoke(' \n' + response({ kind: 'future-kind', extra: true }) + '\n\t'),
        'coordinates.sts', 1, 3, 9);
});

test('derives range length from name, ignoring obsolete len and file fields', () => {
    expectLocation(invoke(response({ name: 'x', len: 100, file: 'wrong.sts' })),
        'coordinates.sts', 1, 3, 4);
});

test('measures name span in UTF-16 units; this does not convert compiler byte columns', () => {
    expectLocation(invoke(response({ name: 'é😀' })), 'coordinates.sts', 1, 3, 6);
});

test('keeps the name-only range fallback for dollar-quoted names with spaces', () => {
    // The compiler returns the token's opening '$' column but omits '$"' and
    // the closing quote from name. This range does not cover the full token.
    const name = 'quoted π';
    expectLocation(invoke(response({ name })), 'coordinates.sts', 1, 3, 3 + name.length);
});

test('characterizes existing Unicode-column limitation, not Unicode navigation accuracy', () => {
    // lexer.c make_token uses byte pointer subtraction; VS Code Position uses
    // UTF-16 units. A multibyte comment before an ASCII name makes these differ.
    const declarationPrefix = '/* é😀 */ fn ';
    const callPrefix = '    /* é😀 */ ret ';
    const byteColumn = Buffer.byteLength(declarationPrefix, 'utf8');
    const utf16Column = declarationPrefix.length;
    const requestCharacter = callPrefix.length;
    assert.notEqual(byteColumn, utf16Column);
    assert.notEqual(Buffer.byteLength(callPrefix, 'utf8'), requestCharacter);
    // A query at the UTF-16 name start misses in the byte-based compiler.
    // Request columns intentionally remain unchanged in this shape-only patch.
    expectAbsent(JSON.stringify({ definition: null }), {
        position: { line: 3, character: requestCharacter },
    });
    // An otherwise successful query still returns an unconverted byte column.
    const result = invoke(response({ col: byteColumn }), {
        position: { line: 3, character: Buffer.byteLength(callPrefix, 'utf8') },
    });
    expectLocation(result, 'coordinates.sts', 1, byteColumn, byteColumn + 6);
    assert.notEqual(result.location.range.start.character, utf16Column);
});

test('accepts a safe large coordinate without truncation', () => {
    const col = Number.MAX_SAFE_INTEGER - definition.name.length;
    expectLocation(invoke(response({ line: Number.MAX_SAFE_INTEGER, col })),
        'coordinates.sts', Number.MAX_SAFE_INTEGER, col, Number.MAX_SAFE_INTEGER);
});

for (const [name, value] of [
    ['explicit no result', { definition: null }],
    ['missing definition', {}],
    ['top-level null', null],
    ['top-level array', []],
    ['top-level string', 'definition'],
    ['top-level number', 5],
    ['top-level boolean', true],
    ['definition array', { definition: [] }],
    ['definition string', { definition: 'target' }],
    ['definition number', { definition: 1 }],
    ['definition boolean', { definition: true }],
    ['definition empty object', { definition: {} }],
    ['old flat file/len protocol', { file: 'coordinates.sts', line: 1, col: 3, len: 6 }],
]) {
    test('returns undefined for ' + name, () => expectAbsent(JSON.stringify(value)));
}

for (const field of ['path', 'line', 'col', 'name']) {
    test('rejects missing ' + field, () => {
        const incomplete = { ...definition };
        delete incomplete[field];
        expectAbsent(JSON.stringify({ definition: incomplete }));
    });
}

for (const field of ['path', 'name']) {
    for (const value of ['', '\0', 'embedded\0nul', null, 7, true, [], {}]) {
        test('rejects invalid ' + field + ': ' + JSON.stringify(value), () => {
            expectAbsent(response({ [field]: value }));
        });
    }
}

for (const field of ['line', 'col']) {
    for (const value of [-1, 1.5, '1', null, true, [], {}, Number.MAX_SAFE_INTEGER + 1]) {
        test('rejects invalid ' + field + ': ' + JSON.stringify(value), () => {
            expectAbsent(response({ [field]: value }));
        });
    }
    test('rejects non-finite ' + field, () => {
        const raw = response().replace('"' + field + '":' + definition[field],
            '"' + field + '":1e400');
        expectAbsent(raw);
    });
}

test('rejects an unsafe range end', () => {
    expectAbsent(response({ col: Number.MAX_SAFE_INTEGER }));
});

for (const [name, stdout] of [
    ['empty output', ''],
    ['whitespace output', ' \n\t'],
    ['truncated JSON', '{"definition":'],
    ['unquoted JSON key', '{definition:null}'],
    ['NaN', '{"definition":{"path":"a","line":NaN,"col":0,"name":"x"}}'],
    ['multiple JSON objects', response() + '\n' + response()],
    ['diagnostic before JSON', 'warning: example\n' + response()],
    ['diagnostic after JSON', response() + '\nwarning: example'],
]) {
    test('rejects malformed output: ' + name, () => expectAbsent(stdout));
}

for (const [name, process] of [
    ['spawn error', { error: new Error('ENOENT'), status: null }],
    ['error despite zero status', { error: new Error('failed'), status: 0 }],
    ['nonzero status', { status: 1 }],
    ['terminated process', { status: null, signal: 'SIGTERM' }],
]) {
    test('ignores apparently valid output after ' + name, () => {
        expectAbsent(response(), { process });
    });
}
