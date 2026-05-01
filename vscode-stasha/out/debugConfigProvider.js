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
exports.StashaDebugConfigProvider = void 0;
const vscode = __importStar(require("vscode"));
const path = __importStar(require("path"));
const fs = __importStar(require("fs"));
const child_process_1 = require("child_process");
const compilerPath_1 = require("./compilerPath");
class StashaDebugConfigProvider {
    resolveDebugConfiguration(folder, config) {
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
                config.cwd = folder?.uri.fsPath ?? path.dirname(config.program);
            }
        }
        return config;
    }
    resolveDebugConfigurationWithSubstitutedVariables(_folder, config) {
        const program = config.program;
        const output = config.output ?? path.join(path.dirname(program), 'debug_out');
        // Ensure output directory exists
        const outDir = path.dirname(output);
        if (!fs.existsSync(outDir))
            fs.mkdirSync(outDir, { recursive: true });
        // Compile with -g
        const bin = (0, compilerPath_1.getCompilerPath)();
        const result = (0, child_process_1.spawnSync)(bin, ['build', program, '-g', '-o', output], {
            encoding: 'utf8',
            cwd: config.cwd,
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
exports.StashaDebugConfigProvider = StashaDebugConfigProvider;
//# sourceMappingURL=debugConfigProvider.js.map