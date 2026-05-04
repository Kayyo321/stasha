"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.registerProjectCommands = registerProjectCommands;
const vscode = __importStar(require("vscode"));
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
const compilerPath_1 = require("./compilerPath");
const MAIN_STS = (name) => `mod ${name};

ext fn main(void): i32 {
    print.('Hello, world!\\n');
    ret 0;
}
`;
const SPROJ = (name) => `main   = "src/main.sts"
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
const LAUNCH_JSON = (srcFile, binFile) => `{
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
function writeFile(filePath, content) {
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, content, 'utf8');
}
function scaffold(root, name) {
    writeFile(path.join(root, 'src', 'main.sts'), MAIN_STS(name));
    writeFile(path.join(root, 'sts.sproj'), SPROJ(name));
    writeFile(path.join(root, '.vscode', 'tasks.json'), TASKS_JSON);
    writeFile(path.join(root, '.vscode', 'launch.json'), LAUNCH_JSON('${workspaceFolder}/src/main.sts', '${workspaceFolder}/bin/debug_out'));
    fs.mkdirSync(path.join(root, 'bin'), { recursive: true });
}
function registerProjectCommands(ctx) {
    ctx.subscriptions.push(vscode.commands.registerCommand('stasha.newProject', async () => {
        const name = await vscode.window.showInputBox({
            prompt: 'Project name',
            validateInput: v => /^[a-z_][a-z0-9_]*$/.test(v) ? null : 'Use lowercase letters, digits, underscores',
        });
        if (!name)
            return;
        const folders = await vscode.window.showOpenDialog({
            canSelectFiles: false,
            canSelectFolders: true,
            canSelectMany: false,
            openLabel: 'Select parent folder',
        });
        if (!folders || folders.length === 0)
            return;
        const root = path.join(folders[0].fsPath, name);
        if (fs.existsSync(root)) {
            vscode.window.showErrorMessage(`Folder already exists: ${root}`);
            return;
        }
        scaffold(root, name);
        await vscode.commands.executeCommand('vscode.openFolder', vscode.Uri.file(root));
    }), vscode.commands.registerCommand('stasha.initProject', async () => {
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
        if (!name)
            return;
        scaffold(root, name);
        vscode.window.showInformationMessage(`Stasha project '${name}' initialized.`);
    }), vscode.commands.registerCommand('stasha.build', () => vscode.tasks.executeTask(new vscode.Task({ type: 'stasha', task: 'build' }, vscode.workspace.workspaceFolders[0], 'build', 'stasha', new vscode.ShellExecution('stasha build'), ['$stasha']))), vscode.commands.registerCommand('stasha.buildRelease', () => vscode.tasks.executeTask(new vscode.Task({ type: 'stasha', task: 'buildRelease' }, vscode.workspace.workspaceFolders[0], 'build (release)', 'stasha', new vscode.ShellExecution('stasha build release'), ['$stasha']))), vscode.commands.registerCommand('stasha.test', () => vscode.tasks.executeTask(new vscode.Task({ type: 'stasha', task: 'test' }, vscode.workspace.workspaceFolders[0], 'test', 'stasha', new vscode.ShellExecution('stasha test'), ['$stasha']))), vscode.commands.registerCommand('stasha.run', async (filePath) => {
        const folder = vscode.workspace.workspaceFolders?.[0];
        const bin = (0, compilerPath_1.getCompilerPath)();
        const terminal = vscode.window.createTerminal('Stasha Run');
        terminal.show();
        const sprojPath = folder ? path.join(folder.uri.fsPath, 'sts.sproj') : null;
        if (sprojPath && fs.existsSync(sprojPath)) {
            const content = fs.readFileSync(sprojPath, 'utf8');
            const match = content.match(/^binary\s*=\s*"([^"]+)"/m);
            const binary = match ? match[1] : 'bin/out';
            terminal.sendText(`cd "${folder.uri.fsPath}" && "${bin}" build && ./${binary}`);
        }
        else {
            const file = filePath ?? vscode.window.activeTextEditor?.document.uri.fsPath;
            if (!file)
                return;
            const outDir = path.join(path.dirname(file), 'bin');
            const outFile = path.join(outDir, 'out');
            terminal.sendText(`mkdir -p "${outDir}" && "${bin}" build "${file}" -o "${outFile}" && "${outFile}"`);
        }
    }), vscode.commands.registerCommand('stasha.startDebug', async (filePath) => {
        const file = filePath ?? vscode.window.activeTextEditor?.document.uri.fsPath;
        if (!file)
            return;
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
    }), vscode.commands.registerCommand('stasha.runTestFile', async (filePath) => {
        const file = filePath ?? vscode.window.activeTextEditor?.document.uri.fsPath;
        if (!file)
            return;
        const bin = (0, compilerPath_1.getCompilerPath)();
        const terminal = vscode.window.createTerminal('Stasha Test');
        terminal.show();
        terminal.sendText(`"${bin}" test "${file}"`);
    }));
}
//# sourceMappingURL=projectCommands.js.map