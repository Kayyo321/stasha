import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

interface RawHint {
    line: number;
    col: number;
    label: string;
    kind: 'type' | 'param';
}

interface HintsResponse {
    hints: RawHint[];
}

export class StashaInlayHintProvider implements vscode.InlayHintsProvider {
    provideInlayHints(
        doc: vscode.TextDocument,
        _range: vscode.Range
    ): vscode.InlayHint[] | undefined {
        const cfg = vscode.workspace.getConfiguration('stasha');
        if (!cfg.get<boolean>('enableInlayHints', true)) return undefined;

        const bin = getCompilerPath();
        const result = spawnSync(bin, ['hints', '--stdin', '--path', doc.uri.fsPath], {
            input: doc.getText(),
            encoding: 'utf8',
            timeout: 5000,
        });
        if (result.error || result.status === null) return undefined;

        let response: HintsResponse;
        try {
            response = JSON.parse(result.stdout.trim());
        } catch {
            return undefined;
        }

        return (response.hints ?? []).map(h => {
            const pos = new vscode.Position(Math.max(0, h.line), Math.max(0, h.col));
            const hint = new vscode.InlayHint(pos, h.label);
            hint.kind = h.kind === 'param'
                ? vscode.InlayHintKind.Parameter
                : vscode.InlayHintKind.Type;
            hint.paddingLeft = true;
            return hint;
        });
    }
}
