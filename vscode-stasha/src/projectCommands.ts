import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';

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

        vscode.commands.registerCommand('stasha.build', () =>
            vscode.tasks.executeTask(new vscode.Task(
                { type: 'stasha', task: 'build' },
                vscode.workspace.workspaceFolders![0],
                'build', 'stasha',
                new vscode.ShellExecution('stasha build'),
                ['$stasha'],
            ))
        ),

        vscode.commands.registerCommand('stasha.buildRelease', () =>
            vscode.tasks.executeTask(new vscode.Task(
                { type: 'stasha', task: 'buildRelease' },
                vscode.workspace.workspaceFolders![0],
                'build (release)', 'stasha',
                new vscode.ShellExecution('stasha build release'),
                ['$stasha'],
            ))
        ),

        vscode.commands.registerCommand('stasha.test', () =>
            vscode.tasks.executeTask(new vscode.Task(
                { type: 'stasha', task: 'test' },
                vscode.workspace.workspaceFolders![0],
                'test', 'stasha',
                new vscode.ShellExecution('stasha test'),
                ['$stasha'],
            ))
        ),

        vscode.commands.registerCommand('stasha.run', async () => {
            const folder = vscode.workspace.workspaceFolders?.[0];
            if (!folder) return;
            const terminal = vscode.window.createTerminal('Stasha Run');
            terminal.show();
            terminal.sendText('stasha build && ./bin/debug_out');
        }),
    );
}
