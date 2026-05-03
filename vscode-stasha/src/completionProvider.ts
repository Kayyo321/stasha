import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

interface CompletionItem {
    label: string;
    kind: string;
    detail: string;
    documentation?: string;
}

interface CompletionsResponse {
    completions: CompletionItem[];
}

const KIND_MAP: Record<string, vscode.CompletionItemKind> = {
    function:   vscode.CompletionItemKind.Function,
    method:     vscode.CompletionItemKind.Method,
    type:       vscode.CompletionItemKind.Class,
    variable:   vscode.CompletionItemKind.Variable,
    field:      vscode.CompletionItemKind.Field,
    const:      vscode.CompletionItemKind.Constant,
    enumMember: vscode.CompletionItemKind.EnumMember,
    module:     vscode.CompletionItemKind.Module,
};

export class StashaCompletionProvider implements vscode.CompletionItemProvider {
    provideCompletionItems(
        doc: vscode.TextDocument,
        pos: vscode.Position
    ): vscode.CompletionItem[] | undefined {
        const cfg = vscode.workspace.getConfiguration('stasha');
        if (!cfg.get<boolean>('enableCompletion', true)) return undefined;

        const bin = getCompilerPath();
        const result = spawnSync(bin, [
            'complete', '--stdin', '--path', doc.uri.fsPath,
            '--line', String(pos.line),
            '--col', String(pos.character),
        ], {
            input: doc.getText(),
            encoding: 'utf8',
            timeout: 5000,
        });

        if (result.error || result.status === null) return undefined;

        let response: CompletionsResponse;
        try {
            response = JSON.parse(result.stdout.trim());
        } catch {
            return undefined;
        }

        return (response.completions ?? []).map(item => {
            const c = new vscode.CompletionItem(item.label,
                KIND_MAP[item.kind] ?? vscode.CompletionItemKind.Variable);
            if (item.detail) c.detail = item.detail;
            if (item.documentation) c.documentation = item.documentation;
            return c;
        });
    }
}
