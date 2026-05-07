import * as vscode from 'vscode';
import { getSymbols, StashaSymbol } from './symbolProvider';

// Cache per document version
const cache = new Map<string, { version: number; symbols: StashaSymbol[] }>();
vscode.workspace.onDidChangeTextDocument(e => cache.delete(e.document.uri.toString()));

function getSymsCached(doc: vscode.TextDocument): StashaSymbol[] {
    const key = doc.uri.toString();
    const hit = cache.get(key);
    if (hit && hit.version === doc.version) return hit.symbols;
    const syms = getSymbols(doc.uri.fsPath);
    cache.set(key, { version: doc.version, symbols: syms });
    return syms;
}

// Walk backward from position to find callee name and active param index
function parseCallContext(doc: vscode.TextDocument, pos: vscode.Position): {
    callee: string;
    paramIndex: number;
} | undefined {
    const lineText = doc.lineAt(pos.line).text.slice(0, pos.character);

    // Find the opening '(' that started the current call
    let depth = 0;
    let parenPos = -1;
    for (let i = lineText.length - 1; i >= 0; i--) {
        const ch = lineText[i];
        if (ch === ')') depth++;
        else if (ch === '(') {
            if (depth === 0) { parenPos = i; break; }
            depth--;
        }
    }
    if (parenPos === -1) return undefined;

    // Count commas at depth 0 after the opening paren
    let paramIndex = 0;
    let d2 = 0;
    for (let i = parenPos + 1; i < lineText.length; i++) {
        const ch = lineText[i];
        if (ch === '(' || ch === '[') d2++;
        else if (ch === ')' || ch === ']') d2--;
        else if (ch === ',' && d2 === 0) paramIndex++;
    }

    // Extract callee: scan backward from parenPos for identifier (with optional .<method>)
    const before = lineText.slice(0, parenPos);
    const m = before.match(/([a-zA-Z_][a-zA-Z0-9_]*)(?:\.([a-zA-Z_][a-zA-Z0-9_]*))*\s*$/);
    if (!m) return undefined;

    return { callee: m[0].trim(), paramIndex };
}

// Build a human-readable signature from a symbol
function buildSignature(sym: StashaSymbol): vscode.SignatureInformation | undefined {
    if (sym.kind !== 'function' && sym.kind !== 'method') return undefined;

    // detail may hold something like "fn add(stack i32 a, b): i32" from hover
    // For now derive from what symbols gives us (name + kind)
    const sig = new vscode.SignatureInformation(`fn ${sym.name}(…)`);
    sig.documentation = sym.detail ? `${sym.kind} ${sym.name} — ${sym.detail}` : undefined;
    return sig;
}

export class StashaSignatureHelpProvider implements vscode.SignatureHelpProvider {
    provideSignatureHelp(
        doc: vscode.TextDocument,
        pos: vscode.Position
    ): vscode.SignatureHelp | undefined {
        const ctx = parseCallContext(doc, pos);
        if (!ctx) return undefined;

        const syms = getSymsCached(doc);
        // Match callee: last segment of "foo.bar" is the method name
        const calleeName = ctx.callee.split('.').pop() ?? ctx.callee;
        const matching = syms.filter(s =>
            s.name === calleeName && (s.kind === 'function' || s.kind === 'method')
        );
        if (matching.length === 0) return undefined;

        const help = new vscode.SignatureHelp();
        help.signatures = matching.map(s => {
            const sig = new vscode.SignatureInformation(`fn ${s.name}(…)`);
            if (s.detail) sig.documentation = s.detail;
            return sig;
        }).filter((s): s is vscode.SignatureInformation => s !== undefined);

        help.activeSignature = 0;
        help.activeParameter = ctx.paramIndex;
        return help;
    }
}
