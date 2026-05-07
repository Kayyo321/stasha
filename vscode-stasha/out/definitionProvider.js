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
exports.StashaDefinitionProvider = void 0;
const vscode = __importStar(require("vscode"));
const child_process_1 = require("child_process");
const compilerPath_1 = require("./compilerPath");
class StashaDefinitionProvider {
    provideDefinition(doc, pos) {
        const bin = (0, compilerPath_1.getCompilerPath)();
        const result = (0, child_process_1.spawnSync)(bin, [
            'definition', doc.uri.fsPath,
            '--line', String(pos.line),
            '--col', String(pos.character),
        ], { encoding: 'utf8' });
        if (result.error || result.status !== 0)
            return undefined;
        try {
            const def = JSON.parse(result.stdout.trim());
            if (!def.file)
                return undefined;
            const range = new vscode.Range(Math.max(0, def.line), Math.max(0, def.col), Math.max(0, def.line), Math.max(0, def.col) + Math.max(1, def.len));
            return new vscode.Location(vscode.Uri.file(def.file), range);
        }
        catch {
            return undefined;
        }
    }
}
exports.StashaDefinitionProvider = StashaDefinitionProvider;
//# sourceMappingURL=definitionProvider.js.map