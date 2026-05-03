import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

export class StashaFormatProvider implements vscode.DocumentFormattingEditProvider {
    provideDocumentFormattingEdits(doc: vscode.TextDocument): vscode.TextEdit[] | undefined {
        const cfg = vscode.workspace.getConfiguration('stasha');
        if (!cfg.get<boolean>('enableFormat', true)) return undefined;

        const bin = getCompilerPath();
        const result = spawnSync(bin, ['format', '--stdin', '--path', doc.uri.fsPath], {
            input: doc.getText(),
            encoding: 'utf8',
            timeout: 10000,
        });
        if (result.error || result.status !== 0 || !result.stdout) return undefined;

        const fullRange = new vscode.Range(
            doc.lineAt(0).range.start,
            doc.lineAt(doc.lineCount - 1).range.end
        );
        return [vscode.TextEdit.replace(fullRange, result.stdout)];
    }
}
