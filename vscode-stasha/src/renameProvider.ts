import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

interface EditEntry {
    file: string;
    line: number;
    col: number;
    len: number;
    newText: string;
}

interface RenameResponse {
    edits: EditEntry[];
}

export class StashaRenameProvider implements vscode.RenameProvider {
    prepareRename(doc: vscode.TextDocument, pos: vscode.Position): vscode.Range | undefined {
        const wordRange = doc.getWordRangeAtPosition(pos, /[a-zA-Z_][a-zA-Z0-9_]*/);
        return wordRange;
    }

    provideRenameEdits(
        doc: vscode.TextDocument,
        pos: vscode.Position,
        newName: string
    ): vscode.WorkspaceEdit | undefined {
        const bin = getCompilerPath();
        const result = spawnSync(bin, [
            'rename', '--stdin', '--path', doc.uri.fsPath,
            '--line', String(pos.line),
            '--col', String(pos.character),
            '--name', newName,
        ], {
            input: doc.getText(),
            encoding: 'utf8',
            timeout: 10000,
        });
        if (result.error || result.status === null) return undefined;

        let response: RenameResponse;
        try {
            response = JSON.parse(result.stdout.trim());
        } catch {
            return undefined;
        }

        const wsEdit = new vscode.WorkspaceEdit();
        for (const e of response.edits ?? []) {
            const uri = vscode.Uri.file(e.file);
            const range = new vscode.Range(
                Math.max(0, e.line), Math.max(0, e.col),
                Math.max(0, e.line), Math.max(0, e.col + e.len)
            );
            wsEdit.replace(uri, range, e.newText);
        }
        return wsEdit;
    }
}
