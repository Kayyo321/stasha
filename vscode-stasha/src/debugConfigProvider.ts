import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { spawnSync } from 'child_process';
import { getCompilerPath } from './compilerPath';

export class StashaDebugConfigProvider implements vscode.DebugConfigurationProvider {
    resolveDebugConfiguration(
        folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration,
    ): vscode.DebugConfiguration | undefined {
        // Fill defaults if config is empty (user pressed F5 with no launch.json)
        if (!config.type && !config.request && !config.name) {
            const editor = vscode.window.activeTextEditor;
            if (editor && editor.document.languageId === 'stasha') {
                config.type = 'stasha';
                config.request = 'launch';
                config.name = 'Debug Stasha Program';
                config.program = editor.document.uri.fsPath;
                config.output = path.join(folder?.uri.fsPath ?? '.', 'bin', 'debug_out');
                config.args = [];
                config.cwd = folder?.uri.fsPath ?? path.dirname(config.program as string);
            }
        }
        return config;
    }

    resolveDebugConfigurationWithSubstitutedVariables(
        _folder: vscode.WorkspaceFolder | undefined,
        config: vscode.DebugConfiguration,
    ): vscode.DebugConfiguration | undefined {
        const program = config.program as string;
        const output = config.output as string ?? path.join(path.dirname(program), 'debug_out');

        // Ensure output directory exists
        const outDir = path.dirname(output);
        if (!fs.existsSync(outDir)) fs.mkdirSync(outDir, { recursive: true });

        // Compile with -g
        const bin = getCompilerPath();
        const result = spawnSync(bin, ['build', program, '-g', '-o', output], {
            encoding: 'utf8',
            cwd: config.cwd as string | undefined,
        });

        if (result.status !== 0) {
            vscode.window.showErrorMessage(`Stasha build failed:\n${result.stderr ?? result.stdout}`);
            return undefined;
        }

        // Hand off to codelldb
        return {
            type: 'lldb',
            request: 'launch',
            name: config.name,
            program: output,
            args: config.args ?? [],
            cwd: config.cwd,
            env: config.env ?? {},
            sourceMap: {},
        };
    }
}
