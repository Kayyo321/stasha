import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';
import { getCompilerPath } from './compilerPath';

const MAIN_STS = (name: string) =>
`mod ${name};

ext fn main(void): i32 {
    print.('Hello, world!\\n');
    ret 0;
}
`;

const SPROJ = (name: string) =>
`main   = "src/main.sts"
binary = "bin/${name}"
ext_libs = []

[debug]
output   = "bin/${name}_debug"
debug    = true
optimize = 0

[release]
output   = "bin/${name}"
optimize = 3

[test]
type   = "test"
output = "bin/${name}_test"
`;

const TASKS_JSON = `{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "stasha: build",
      "type": "stasha",
      "task": "build",
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": ["$stasha"]
    },
    {
      "label": "stasha: build (release)",
      "type": "stasha",
      "task": "buildRelease",
      "group": "build",
      "problemMatcher": ["$stasha"]
    },
    {
      "label": "stasha: test",
      "type": "stasha",
      "task": "test",
      "group": { "kind": "test", "isDefault": true },
      "problemMatcher": ["$stasha"]
    }
  ]
}
`;

const LAUNCH_JSON = (srcFile: string, binFile: string) =>
`{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "stasha",
      "request": "launch",
      "name": "Debug Stasha Program",
      "program": "${srcFile}",
      "output": "${binFile}",
      "args": [],
      "cwd": "\${workspaceFolder}"
    }
  ]
}
`;

function writeFile(filePath: string, content: string) {
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, content, 'utf8');
}

function scaffold(root: string, name: string) {
    writeFile(path.join(root, 'src', 'main.sts'), MAIN_STS(name));
    writeFile(path.join(root, 'sts.sproj'), SPROJ(name));
    writeFile(path.join(root, '.vscode', 'tasks.json'), TASKS_JSON);
    writeFile(
        path.join(root, '.vscode', 'launch.json'),
        LAUNCH_JSON('${workspaceFolder}/src/main.sts', '${workspaceFolder}/bin/debug_out'),
    );
    fs.mkdirSync(path.join(root, 'bin'), { recursive: true });
}

export function registerProjectCommands(ctx: vscode.ExtensionContext) {
    ctx.subscriptions.push(
        vscode.commands.registerCommand('stasha.newProject', async () => {
            const name = await vscode.window.showInputBox({
                prompt: 'Project name',
                validateInput: v => /^[a-z_][a-z0-9_]*$/.test(v) ? null : 'Use lowercase letters, digits, underscores',
            });
            if (!name) return;

            const folders = await vscode.window.showOpenDialog({
                canSelectFiles: false,
                canSelectFolders: true,
                canSelectMany: false,
                openLabel: 'Select parent folder',
            });
            if (!folders || folders.length === 0) return;

            const root = path.join(folders[0].fsPath, name);
            if (fs.existsSync(root)) {
                vscode.window.showErrorMessage(`Folder already exists: ${root}`);
                return;
            }

            scaffold(root, name);
            await vscode.commands.executeCommand('vscode.openFolder', vscode.Uri.file(root));
        }),

        vscode.commands.registerCommand('stasha.initProject', async () => {
            const folder = vscode.workspace.workspaceFolders?.[0];
            if (!folder) {
                vscode.window.showErrorMessage('Open a folder first.');
                return;
            }
            const root = folder.uri.fsPath;
            const rawName = path.basename(root).replace(/[^a-z0-9_]/gi, '_').toLowerCase();
            const name = await vscode.window.showInputBox({
                prompt: 'Project name',
                value: rawName,
                validateInput: v => /^[a-z_][a-z0-9_]*$/.test(v) ? null : 'Use lowercase letters, digits, underscores',
            });
            if (!name) return;
            scaffold(root, name);
            vscode.window.showInformationMessage(`Stasha project '${name}' initialized.`);
        }),

        vscode.commands.registerCommand('stasha.build', () => {
            const bin = getCompilerPath();
            return vscode.tasks.executeTask(new vscode.Task(
                { type: 'stasha', task: 'build' },
                vscode.workspace.workspaceFolders![0],
                'build', 'stasha',
                new vscode.ShellExecution(`"${bin}" build`),
                ['$stasha'],
            ));
        }),

        vscode.commands.registerCommand('stasha.buildRelease', () => {
            const bin = getCompilerPath();
            return vscode.tasks.executeTask(new vscode.Task(
                { type: 'stasha', task: 'buildRelease' },
                vscode.workspace.workspaceFolders![0],
                'build (release)', 'stasha',
                new vscode.ShellExecution(`"${bin}" build release`),
                ['$stasha'],
            ));
        }),

        vscode.commands.registerCommand('stasha.test', () => {
            const bin = getCompilerPath();
            return vscode.tasks.executeTask(new vscode.Task(
                { type: 'stasha', task: 'test' },
                vscode.workspace.workspaceFolders![0],
                'test', 'stasha',
                new vscode.ShellExecution(`"${bin}" test`),
                ['$stasha'],
            ));
        }),

        vscode.commands.registerCommand('stasha.run', async (filePath?: string) => {
            const folder = vscode.workspace.workspaceFolders?.[0];
            const bin = getCompilerPath();
            const terminal = vscode.window.createTerminal('Stasha Run');
            terminal.show();

            const sprojPath = folder ? path.join(folder.uri.fsPath, 'sts.sproj') : null;
            if (sprojPath && fs.existsSync(sprojPath)) {
                const content = fs.readFileSync(sprojPath, 'utf8');
                const match = content.match(/^binary\s*=\s*"([^"]+)"/m);
                const binary = match ? match[1] : 'bin/out';
                terminal.sendText(`cd "${folder!.uri.fsPath}" && "${bin}" build && ./${binary}`);
            } else {
                const file = filePath ?? vscode.window.activeTextEditor?.document.uri.fsPath;
                if (!file) return;
                const outDir = path.join(path.dirname(file), 'bin');
                const outFile = path.join(outDir, 'out');
                terminal.sendText(`mkdir -p "${outDir}" && "${bin}" build "${file}" -o "${outFile}" && "${outFile}"`);
            }
        }),

        vscode.commands.registerCommand('stasha.startDebug', async (filePath?: string) => {
            const file = filePath ?? vscode.window.activeTextEditor?.document.uri.fsPath;
            if (!file) return;
            const folder = vscode.workspace.workspaceFolders?.[0];
            const outDir = folder ? path.join(folder.uri.fsPath, 'bin') : path.join(path.dirname(file), 'bin');
            await vscode.debug.startDebugging(folder, {
                type: 'stasha',
                request: 'launch',
                name: 'Debug Stasha',
                program: file,
                output: path.join(outDir, 'debug_out'),
                args: [],
                cwd: folder?.uri.fsPath ?? path.dirname(file),
            });
        }),

        vscode.commands.registerCommand('stasha.runTestFile', async (filePath?: string) => {
            const file = filePath ?? vscode.window.activeTextEditor?.document.uri.fsPath;
            if (!file) return;
            const bin = getCompilerPath();
            const terminal = vscode.window.createTerminal('Stasha Test');
            terminal.show();
            terminal.sendText(`"${bin}" test "${file}"`);
        }),
    );
}
