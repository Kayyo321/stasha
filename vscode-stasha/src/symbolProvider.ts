import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

export interface StashaSymbol {
    name: string;
    kind: string;
    line: number;
    col: number;
    detail?: string;
}

const KIND_MAP: Record<string, vscode.SymbolKind> = {
    fn:        vscode.SymbolKind.Function,
    method:    vscode.SymbolKind.Method,
    struct:    vscode.SymbolKind.Struct,
    enum:      vscode.SymbolKind.Enum,
    variant:   vscode.SymbolKind.EnumMember,
    type:      vscode.SymbolKind.TypeParameter,
    var:       vscode.SymbolKind.Variable,
    const:     vscode.SymbolKind.Constant,
    field:     vscode.SymbolKind.Field,
    interface: vscode.SymbolKind.Interface,
    module:    vscode.SymbolKind.Module,
    test:      vscode.SymbolKind.Event,
};

export function getSymbols(filePath: string): StashaSymbol[] {
    const bin = getCompilerPath();
    const result = spawnSync(bin, ['symbols', filePath], { encoding: 'utf8' });
    if (result.error || result.status !== 0) return [];
    try {
        return JSON.parse(result.stdout.trim()) as StashaSymbol[];
    } catch {
        return [];
    }
}

export class StashaDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
    provideDocumentSymbols(doc: vscode.TextDocument): vscode.DocumentSymbol[] {
        const syms = getSymbols(doc.uri.fsPath);
        return syms.map(s => {
            const kind = KIND_MAP[s.kind] ?? vscode.SymbolKind.Variable;
            const line = Math.max(0, s.line);
            const col = Math.max(0, s.col);
            const range = new vscode.Range(line, col, line, col + s.name.length);
            return new vscode.DocumentSymbol(s.name, s.detail ?? '', kind, range, range);
        });
    }
}
