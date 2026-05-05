import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { execSync } from 'child_process';

export function getCompilerPath(): string {
    const cfg = vscode.workspace.getConfiguration('stasha');
    const configured = cfg.get<string>('compilerPath');
    if (configured && configured.trim() !== '' && configured !== 'stasha') {
        return configured;
    }

    const folders = vscode.workspace.workspaceFolders;
    if (folders && folders.length > 0) {
        const candidate = path.join(folders[0].uri.fsPath, 'bin', 'stasha');
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }

    try {
        const which = process.platform === 'win32'
            ? 'where stasha'
            : `${process.env.SHELL ?? '/bin/zsh'} -l -c 'which stasha'`;
        const resolved = execSync(which, { encoding: 'utf8' }).trim().split('\n')[0].trim();
        if (resolved) return resolved;
    } catch {
        // not on PATH
    }

    return configured ?? 'stasha';
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
