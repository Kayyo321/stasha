import * as vscode from 'vscode';
import { getSymbols, StashaSymbol } from './symbolProvider';
import * as path from 'path';

const KIND_MAP: Record<string, vscode.SymbolKind> = {
    function:   vscode.SymbolKind.Function,
    method:     vscode.SymbolKind.Method,
    struct:     vscode.SymbolKind.Struct,
    enum:       vscode.SymbolKind.Enum,
    enumMember: vscode.SymbolKind.EnumMember,
    type:       vscode.SymbolKind.TypeParameter,
    variable:   vscode.SymbolKind.Variable,
    field:      vscode.SymbolKind.Field,
    interface:  vscode.SymbolKind.Interface,
    module:     vscode.SymbolKind.Module,
};

export class StashaWorkspaceSymbolProvider implements vscode.WorkspaceSymbolProvider {
    async provideWorkspaceSymbols(query: string): Promise<vscode.SymbolInformation[]> {
        const files = await vscode.workspace.findFiles('**/*.sts', '**/node_modules/**');
        const results: vscode.SymbolInformation[] = [];
        const lq = query.toLowerCase();

        for (const file of files) {
            const syms = getSymbols(file.fsPath);
            for (const s of syms) {
                if (query && !s.name.toLowerCase().includes(lq)) continue;
                const kind = KIND_MAP[s.kind] ?? vscode.SymbolKind.Variable;
                const loc = new vscode.Location(
                    file,
                    new vscode.Position(Math.max(0, s.line), Math.max(0, s.col))
                );
                results.push(new vscode.SymbolInformation(
                    s.name,
                    kind,
                    s.detail ?? path.basename(file.fsPath),
                    loc
                ));
            }
        }
        return results;
    }
}
