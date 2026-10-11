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
function isRecord(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
}
function isDefResult(value) {
    if (!isRecord(value))
        return false;
    return typeof value.path === 'string' && value.path.length > 0 && !value.path.includes('\0')
        && typeof value.line === 'number' && Number.isSafeInteger(value.line) && value.line >= 0
        && typeof value.col === 'number' && Number.isSafeInteger(value.col) && value.col >= 0
        && typeof value.name === 'string' && value.name.length > 0 && !value.name.includes('\0')
        && Number.isSafeInteger(value.col + value.name.length);
}
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
            const response = JSON.parse(result.stdout.trim());
            if (!isRecord(response) || !isDefResult(response.definition))
                return undefined;
            const def = response.definition;
            // The CLI uses byte columns; VS Code uses UTF-16 units. This decoder
            // preserves the existing request/response columns, so non-ASCII text
            // before either position remains unsupported. name.length supplies
            // a UTF-16 span, not a full token extent for dollar-quoted names.
            const range = new vscode.Range(def.line, def.col, def.line, def.col + def.name.length);
            return new vscode.Location(vscode.Uri.file(def.path), range);
        }
        catch {
            return undefined;
        }
    }
}
exports.StashaDefinitionProvider = StashaDefinitionProvider;
//# sourceMappingURL=definitionProvider.js.map