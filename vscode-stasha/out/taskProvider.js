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
exports.StashaTaskProvider = void 0;
const vscode = __importStar(require("vscode"));
const path = __importStar(require("path"));
const fs = __importStar(require("fs"));
const compilerPath_1 = require("./compilerPath");
function hasSproj(folder) {
    return fs.existsSync(path.join(folder.uri.fsPath, 'sts.sproj'));
}
function makeShellTask(folder, label, def, cmd, group) {
    const task = new vscode.Task(def, folder, label, 'stasha', new vscode.ShellExecution(cmd, { cwd: folder.uri.fsPath }), ['$stasha']);
    if (group)
        task.group = group;
    return task;
}
class StashaTaskProvider {
    provideTasks() {
        const folders = vscode.workspace.workspaceFolders;
        if (!folders)
            return [];
        const tasks = [];
        const bin = (0, compilerPath_1.getCompilerPath)();
        for (const folder of folders) {
            const proj = hasSproj(folder);
            const base = proj ? `"${bin}"` : `"${bin}" build`;
            tasks.push(makeShellTask(folder, 'build', { type: 'stasha', task: 'build' }, proj ? `"${bin}" build` : `"${bin}" build`, vscode.TaskGroup.Build));
            tasks.push(makeShellTask(folder, 'build (release)', { type: 'stasha', task: 'buildRelease' }, proj ? `"${bin}" build release` : `"${bin}" build`, vscode.TaskGroup.Build));
            tasks.push(makeShellTask(folder, 'test', { type: 'stasha', task: 'test' }, `"${bin}" test`, vscode.TaskGroup.Test));
            void base; // suppress unused warning — used in non-proj path below
        }
        return tasks;
    }
    resolveTask(task) {
        const def = task.definition;
        const folder = vscode.workspace.workspaceFolders?.[0];
        if (!folder)
            return undefined;
        const bin = (0, compilerPath_1.getCompilerPath)();
        let cmd;
        switch (def.task) {
            case 'build':
                cmd = def.file ? `"${bin}" build "${def.file}"` : `"${bin}" build`;
                break;
            case 'buildRelease':
                cmd = `"${bin}" build release`;
                break;
            case 'test':
                cmd = def.file ? `"${bin}" test "${def.file}"` : `"${bin}" test`;
                break;
            default: return undefined;
        }
        task.execution = new vscode.ShellExecution(cmd, { cwd: folder.uri.fsPath });
        return task;
    }
}
exports.StashaTaskProvider = StashaTaskProvider;
//# sourceMappingURL=taskProvider.js.map