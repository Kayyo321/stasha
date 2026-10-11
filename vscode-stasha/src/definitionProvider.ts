import * as vscode from 'vscode';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

interface DefResult {
    path: string;
    line: number;
    col: number;
    name: string;
}

function isRecord(value: unknown): value is Record<string, unknown> {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function isDefResult(value: unknown): value is DefResult {
    if (!isRecord(value)) return false;
    return typeof value.path === 'string' && value.path.length > 0 && !value.path.includes('\0')
        && typeof value.line === 'number' && Number.isSafeInteger(value.line) && value.line >= 0
        && typeof value.col === 'number' && Number.isSafeInteger(value.col) && value.col >= 0
        && typeof value.name === 'string' && value.name.length > 0 && !value.name.includes('\0')
        && Number.isSafeInteger(value.col + value.name.length);
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
            const response: unknown = JSON.parse(result.stdout.trim());
            if (!isRecord(response) || !isDefResult(response.definition)) return undefined;
            const def = response.definition;
            // The CLI uses byte columns; VS Code uses UTF-16 units. This decoder
            // preserves the existing request/response columns, so non-ASCII text
            // before either position remains unsupported. name.length supplies
            // a UTF-16 span, not a full token extent for dollar-quoted names.
            const range = new vscode.Range(
                def.line, def.col,
                def.line, def.col + def.name.length,
            );
            return new vscode.Location(vscode.Uri.file(def.path), range);
        } catch {
            return undefined;
        }
    }
}
