import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

// Must match the legend declared in package.json contributions.semanticTokenTypes
export const SEMANTIC_LEGEND = new vscode.SemanticTokensLegend(
    ['keyword', 'type', 'function', 'variable', 'parameter', 'property',
     'string', 'number', 'comment', 'operator', 'decorator', 'enumMember'],
    ['defaultLibrary', 'declaration', 'definition']
);

interface RawToken {
    line: number;
    col: number;
    len: number;
    kind: string;
    text: string;
}

function kindToIndex(kind: string, text: string): { type: number; modifiers: number } | null {
    switch (kind) {
        case 'keyword':    return { type: 0, modifiers: 0 };
        case 'type':       return { type: 1, modifiers: 0 };
        case 'identifier': return null; // let TextMate handle plain identifiers
        case 'number':     return { type: 7, modifiers: 0 };
        case 'string':     return { type: 6, modifiers: 0 };
        case 'comment':    return { type: 8, modifiers: 0 };
        case 'operator':   return { type: 9, modifiers: 0 };
        case 'delimiter':  return null;
        case 'eof':        return null;
        case 'error':      return null;
        default:           return null;
    }
}

export class StashaSemanticTokensProvider implements vscode.DocumentSemanticTokensProvider {
    provideDocumentSemanticTokens(doc: vscode.TextDocument): vscode.SemanticTokens | undefined {
        const cfg = vscode.workspace.getConfiguration('stasha');
        if (!cfg.get<boolean>('enableSemanticTokens', true)) return undefined;

        const bin = getCompilerPath();
        const result = spawnSync(bin, ['tokens', '--stdin', '--path', doc.uri.fsPath], {
            input: doc.getText(),
            encoding: 'utf8',
            timeout: 5000,
        });
        if (result.error || result.status !== 0) return undefined;

        let raw: { tokens: RawToken[] };
        try {
            raw = JSON.parse(result.stdout.trim());
        } catch {
            return undefined;
        }

        const builder = new vscode.SemanticTokensBuilder(SEMANTIC_LEGEND);
        for (const tok of raw.tokens ?? []) {
            const mapped = kindToIndex(tok.kind, tok.text);
            if (!mapped) continue;
            try {
                builder.push(tok.line, tok.col, tok.len, mapped.type, mapped.modifiers);
            } catch {
                // out-of-range token — skip
            }
        }
        return builder.build();
    }
}
