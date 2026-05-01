import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

interface DefResult {
    file: string;
    line: number;
    col: number;
    len: number;
}

export class StashaDefinitionProvider implements vscode.DefinitionProvider {
    provideDefinition(
        doc: vscode.TextDocument,
        pos: vscode.Position,
    ): vscode.Location | undefined {
        const bin = getCompilerPath();
        const result = spawnSync(bin, [
            'definition', doc.uri.fsPath,
            '--line', String(pos.line),
            '--col', String(pos.character),
        ], { encoding: 'utf8' });

        if (result.error || result.status !== 0) return undefined;
        try {
            const def: DefResult = JSON.parse(result.stdout.trim());
            if (!def.file) return undefined;
            const range = new vscode.Range(
                Math.max(0, def.line), Math.max(0, def.col),
                Math.max(0, def.line), Math.max(0, def.col) + Math.max(1, def.len),
            );
            return new vscode.Location(vscode.Uri.file(def.file), range);
        } catch {
            return undefined;
        }
    }
}
