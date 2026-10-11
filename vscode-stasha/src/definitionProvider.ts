import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { promises as fs } from 'fs';
import * as path from 'path';
import { getCompilerPath } from './compilerPath';

interface DefResult {
    path: string;
    line: number;
    col: number;
    name: string;
}

interface DocumentSnapshot {
    document: vscode.TextDocument;
    uri: vscode.Uri;
    uriText: string;
    fsPath: string;
    version: number;
    originalText: string;
}

interface CoordinateSource {
    text: string;
    lines: readonly string[];
    role: 'caller' | 'target';
}

interface CallerSnapshot extends DocumentSnapshot {
    compiler: CoordinateSource;
    input: Buffer;
}

interface AllowedRoot {
    folder: vscode.WorkspaceFolder | undefined;
    uriText: string | undefined;
    fsPath: string;
}

function isRecord(value: unknown): value is Record<string, unknown> {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function isCoordinate(value: unknown): value is number {
    return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
}

function isScalarText(text: string): boolean {
    for (let i = 0; i < text.length; i++) {
        const unit = text.charCodeAt(i);
        if (unit >= 0xd800 && unit <= 0xdbff) {
            const next = text.charCodeAt(++i);
            if (!(next >= 0xdc00 && next <= 0xdfff)) return false;
        } else if (unit >= 0xdc00 && unit <= 0xdfff) {
            return false;
        }
    }
    return true;
}

function isDefResult(value: unknown): value is DefResult {
    if (!isRecord(value)) return false;
    return typeof value.path === 'string' && isFilePath(value.path)
        && isCoordinate(value.line) && isCoordinate(value.col)
        && typeof value.name === 'string' && value.name.length > 0
        && !value.name.includes('\0') && isScalarText(value.name);
}

function isFilePath(value: string): boolean {
    return value.length > 0 && !value.includes('\0') && isScalarText(value)
        && path.isAbsolute(value);
}

// Admission only, not a parser or semantic resolver. The optional offset must
// occur in code, so a matching word inside a comment/string is not a token.
function validateCoordinateSource(
    text: string, role: 'caller' | 'target', tokenOffset?: number,
): boolean {
    if (!isScalarText(text) || text.startsWith('\ufeff') || /[\0\x1a]/.test(text)
        || /\r(?!\n)/.test(text) || /\\\n/.test(text)
        || (role === 'caller' && /\\\r\n/.test(text))) return false;

    let quote = '';
    let commentDepth = 0;
    let lineComment = false;
    let inCode = tokenOffset === undefined;
    for (let i = 0; i < text.length; i++) {
        const char = text[i];
        const next = text[i + 1];
        if (lineComment) {
            if (char === '\n') lineComment = false;
        } else if (commentDepth > 0) {
            if (char === '/' && next === '*') { commentDepth++; i++; }
            else if (char === '*' && next === '/') { commentDepth--; i++; }
        } else if (quote) {
            if ((quote === '`' && (char === '\r' || char === '\n'))
                || (role === 'caller' && char === '\r')) return false;
            if (char === '\\') {
                // Escaped physical LF was rejected above. A raw target CRLF is
                // not normalized: skipping CR still leaves LF to be counted.
                i++;
                if (quote === '`' && (text[i] === '\r' || text[i] === '\n')) return false;
                if (i < text.length && text.codePointAt(i)! > 0xffff) i++;
            } else if (char === quote) {
                quote = '';
            }
        } else {
            if (i === tokenOffset) inCode = true;
            if (char === '/' && next === '/') { lineComment = true; i++; }
            else if (char === '/' && next === '*') { commentDepth = 1; i++; }
            else if (char === '"' || char === "'" || char === '`') quote = char;
        }
    }
    return inCode && !quote && commentDepth === 0;
}

function coordinateSource(text: string, role: 'caller' | 'target'): CoordinateSource {
    return { text, role, lines: text.split('\n').map(line => line.endsWith('\r') ? line.slice(0, -1) : line) };
}

// Both directions accept only exact Unicode scalar boundaries, including EOL.
function convertColumn(line: string, offset: number, fromBytes: boolean): number | undefined {
    if (!isCoordinate(offset)) return undefined;
    let units = 0;
    let bytes = 0;
    for (const scalar of line) {
        if ((fromBytes ? bytes : units) === offset) return fromBytes ? units : bytes;
        const code = scalar.codePointAt(0)!;
        units += scalar.length;
        bytes += code <= 0x7f ? 1 : code <= 0x7ff ? 2 : code <= 0xffff ? 3 : 4;
        if ((fromBytes ? bytes : units) > offset) return undefined;
    }
    return (fromBytes ? bytes : units) === offset ? (fromBytes ? units : bytes) : undefined;
}

function captureDocument(doc: vscode.TextDocument): DocumentSnapshot | undefined {
    if (doc.isClosed || doc.isUntitled || doc.uri.scheme !== 'file'
        || !isFilePath(doc.uri.fsPath) || !isCoordinate(doc.version)
        || !vscode.workspace.textDocuments.includes(doc)) return undefined;
    return {
        document: doc, uri: doc.uri, uriText: doc.uri.toString(), fsPath: doc.uri.fsPath,
        version: doc.version, originalText: doc.getText(),
    };
}

function captureCaller(doc: vscode.TextDocument): CallerSnapshot | undefined {
    const original = captureDocument(doc);
    if (!original || !validateCoordinateSource(original.originalText, 'caller')) return undefined;
    // Keep originalText for freshness: an EOL-only edit must invalidate even if
    // the resulting compilerText would be identical. Never read/save the caller.
    const compilerText = original.originalText.replace(/\r\n/g, '\n');
    return { ...original, compiler: coordinateSource(compilerText, 'caller'), input: Buffer.from(compilerText, 'utf8') };
}

function snapshotStillCurrent(snapshot: DocumentSnapshot, clean = false): boolean {
    const doc = snapshot.document;
    return vscode.workspace.textDocuments.includes(doc) && !doc.isClosed && !doc.isUntitled
        && (!clean || !doc.isDirty) && doc.uri === snapshot.uri && doc.uri.scheme === 'file'
        && doc.uri.fsPath === snapshot.fsPath && doc.uri.toString() === snapshot.uriText
        && doc.version === snapshot.version && doc.getText() === snapshot.originalText;
}

function contains(root: string, file: string): boolean {
    const relative = path.relative(root, file);
    return relative !== '..' && !relative.startsWith('..' + path.sep) && !path.isAbsolute(relative);
}

function allowedRoot(caller: DocumentSnapshot): AllowedRoot {
    const folder = vscode.workspace.getWorkspaceFolder(caller.uri);
    if (folder && folder.uri.scheme === 'file' && isFilePath(folder.uri.fsPath)
        && contains(folder.uri.fsPath, caller.fsPath)) {
        return { folder, uriText: folder.uri.toString(), fsPath: folder.uri.fsPath };
    }
    return { folder: undefined, uriText: undefined, fsPath: path.dirname(caller.fsPath) };
}

function rootStillCurrent(root: AllowedRoot, caller: DocumentSnapshot): boolean {
    const current = allowedRoot(caller);
    return current.folder === root.folder && current.uriText === root.uriText && current.fsPath === root.fsPath;
}

function targetSpan(source: CoordinateSource, def: DefResult): [number, number] | undefined {
    const line = source.lines[def.line];
    if (line === undefined) return undefined;
    const start = convertColumn(line, def.col, true);
    if (start === undefined || start === line.length) return undefined;
    let lineStart = 0;
    for (let i = 0; i < def.line; i++) {
        lineStart = source.text.indexOf('\n', lineStart) + 1;
    }
    if (!validateCoordinateSource(source.text, source.role, lineStart + start)) return undefined;

    if (line[start] === '$' && (line[start + 1] === '"' || line[start + 1] === "'")) {
        const quote = line[start + 1];
        for (let end = start + 2; end < line.length; end++) {
            if (line[end] === '\\') {
                end++;
                if (end < line.length && line.codePointAt(end)! > 0xffff) end++;
            } else if (line[end] === quote) {
                return line.slice(start + 2, end) === def.name ? [start, end + 1] : undefined;
            }
        }
        return undefined;
    }
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(def.name)) return undefined;
    const end = start + def.name.length;
    if (line.slice(start, end) !== def.name
        || (start > 0 && /[A-Za-z0-9_]/.test(line[start - 1]))
        || (end < line.length && /[A-Za-z0-9_]/.test(line[end]))) return undefined;
    return [start, end];
}

export class StashaDefinitionProvider implements vscode.DefinitionProvider {
    async provideDefinition(
        doc: vscode.TextDocument,
        pos: vscode.Position,
        token: vscode.CancellationToken,
    ): Promise<vscode.Location | undefined> {
        try {
            if (token.isCancellationRequested) return undefined;
            const caller = captureCaller(doc);
            if (!caller || !isCoordinate(pos.line) || !isCoordinate(pos.character)) return undefined;
            const root = allowedRoot(caller);
            const current = () => !token.isCancellationRequested
                && snapshotStillCurrent(caller) && rootStillCurrent(root, caller);
            const line = caller.compiler.lines[pos.line];
            if (line === undefined) return undefined;
            const col = convertColumn(line, pos.character, false);
            if (col === undefined || !current()) return undefined;
            const result = spawnSync(getCompilerPath(), [
                'definition', '--stdin', '--path', caller.fsPath,
                '--line', String(pos.line), '--col', String(col),
            ], { encoding: 'utf8', input: caller.input });
            if (!current() || result.error || result.status !== 0) return undefined;
            const response: unknown = JSON.parse(result.stdout.trim());
            if (!isRecord(response) || !isDefResult(response.definition)) return undefined;
            const def = response.definition;
            if (def.path === caller.fsPath) {
                const span = targetSpan(caller.compiler, def);
                if (!span || !current()) return undefined;
                return new vscode.Location(caller.uri, new vscode.Range(def.line, span[0], def.line, span[1]));
            }

            // Lexical project scope, not a realpath/symlink identity guarantee.
            // Read/open only the returned target; never discover alternate paths.
            if (path.extname(def.path) !== '.sts' || !contains(root.fsPath, def.path)) return undefined;
            const uri = vscode.Uri.file(def.path);
            const targetDocument = await vscode.workspace.openTextDocument(uri);
            if (!current()) return undefined;
            const target = captureDocument(targetDocument);
            if (!target || targetDocument.isDirty || target.uriText !== uri.toString()
                || target.fsPath !== uri.fsPath || !validateCoordinateSource(target.originalText, 'target')) return undefined;
            const targetCurrent = () => current() && snapshotStillCurrent(target, true);
            if (!targetCurrent()) return undefined;
            const bytes = await fs.readFile(uri.fsPath);
            if (!targetCurrent()) return undefined;
            const text = bytes.toString('utf8');
            if (!Buffer.from(text, 'utf8').equals(bytes) || text !== target.originalText) return undefined;
            const span = targetSpan(coordinateSource(target.originalText, 'target'), def);
            if (!span || !targetCurrent()) return undefined;
            // This proves present consistency only. JSON carries no identity of
            // imported bytes the compiler read; historical ABA remains possible.
            const finalBytes = await fs.readFile(uri.fsPath);
            if (!targetCurrent() || !bytes.equals(finalBytes)) return undefined;
            return new vscode.Location(target.uri, new vscode.Range(def.line, span[0], def.line, span[1]));
        } catch {
            return undefined;
        }
    }
}
