import * as vscode from 'vscode';
import { execSync } from 'child_process';

export function getCompilerPath(): string {
    const cfg = vscode.workspace.getConfiguration('stasha');
    return cfg.get<string>('compilerPath') ?? 'stasha';
}

export function isCompilerAvailable(): boolean {
    const bin = getCompilerPath();
    try {
        execSync(`"${bin}" --version`, { stdio: 'ignore' });
        return true;
    } catch {
        return false;
    }
}
