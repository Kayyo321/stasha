import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

interface RefLoc {
    file: string;
    line: number;
    col: number;
    len: number;
}

interface RefsResponse {
    refs: RefLoc[];
}

export class StashaReferencesProvider implements vscode.ReferenceProvider {
    provideReferences(
        doc: vscode.TextDocument,
        pos: vscode.Position,
        context: vscode.ReferenceContext
    ): vscode.Location[] | undefined {
        const bin = getCompilerPath();
        const result = spawnSync(bin, [
            'refs', '--stdin', '--path', doc.uri.fsPath,
            '--line', String(pos.line),
            '--col', String(pos.character),
        ], {
            input: doc.getText(),
            encoding: 'utf8',
            timeout: 10000,
        });
        if (result.error || result.status === null) return undefined;

        let response: RefsResponse;
        try {
            response = JSON.parse(result.stdout.trim());
        } catch {
            return undefined;
        }

        return (response.refs ?? []).map(r => {
            const uri = vscode.Uri.file(r.file);
            const range = new vscode.Range(
                Math.max(0, r.line), Math.max(0, r.col),
                Math.max(0, r.line), Math.max(0, r.col + r.len)
            );
            return new vscode.Location(uri, range);
        });
    }
}
