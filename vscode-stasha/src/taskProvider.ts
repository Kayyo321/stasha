import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { getCompilerPath } from './compilerPath';

interface StashaTaskDef extends vscode.TaskDefinition {
    task: string;
    file?: string;
}

function hasSproj(folder: vscode.WorkspaceFolder): boolean {
    return fs.existsSync(path.join(folder.uri.fsPath, 'sts.sproj'));
}

function makeShellTask(
    folder: vscode.WorkspaceFolder,
    label: string,
    def: StashaTaskDef,
    cmd: string,
    group?: vscode.TaskGroup,
): vscode.Task {
    const task = new vscode.Task(
        def, folder, label, 'stasha',
        new vscode.ShellExecution(cmd, { cwd: folder.uri.fsPath }),
        ['$stasha'],
    );
    if (group) task.group = group;
    return task;
}

export class StashaTaskProvider implements vscode.TaskProvider {
    provideTasks(): vscode.Task[] {
        const folders = vscode.workspace.workspaceFolders;
        if (!folders) return [];

        const tasks: vscode.Task[] = [];
        const bin = getCompilerPath();

        for (const folder of folders) {
            const proj = hasSproj(folder);
            const base = proj ? `"${bin}"` : `"${bin}" build`;

            tasks.push(makeShellTask(folder, 'build', { type: 'stasha', task: 'build' },
                proj ? `"${bin}" build` : `"${bin}" build`,
                vscode.TaskGroup.Build));

            tasks.push(makeShellTask(folder, 'build (release)', { type: 'stasha', task: 'buildRelease' },
                proj ? `"${bin}" build release` : `"${bin}" build`,
                vscode.TaskGroup.Build));

            tasks.push(makeShellTask(folder, 'test', { type: 'stasha', task: 'test' },
                `"${bin}" test`,
                vscode.TaskGroup.Test));

            void base; // suppress unused warning — used in non-proj path below
        }
        return tasks;
    }

    resolveTask(task: vscode.Task): vscode.Task | undefined {
        const def = task.definition as StashaTaskDef;
        const folder = vscode.workspace.workspaceFolders?.[0];
        if (!folder) return undefined;
        const bin = getCompilerPath();

        let cmd: string;
        switch (def.task) {
            case 'build':        cmd = def.file ? `"${bin}" build "${def.file}"` : `"${bin}" build`; break;
            case 'buildRelease': cmd = `"${bin}" build release`; break;
            case 'test':         cmd = def.file ? `"${bin}" test "${def.file}"` : `"${bin}" test`; break;
            default: return undefined;
        }

        task.execution = new vscode.ShellExecution(cmd, { cwd: folder.uri.fsPath });
        return task;
    }
}
