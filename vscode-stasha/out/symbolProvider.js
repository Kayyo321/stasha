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
exports.StashaDocumentSymbolProvider = void 0;
exports.getSymbols = getSymbols;
const vscode = __importStar(require("vscode"));
const child_process_1 = require("child_process");
const compilerPath_1 = require("./compilerPath");
const KIND_MAP = {
    fn: vscode.SymbolKind.Function,
    method: vscode.SymbolKind.Method,
    struct: vscode.SymbolKind.Struct,
    enum: vscode.SymbolKind.Enum,
    variant: vscode.SymbolKind.EnumMember,
    type: vscode.SymbolKind.TypeParameter,
    var: vscode.SymbolKind.Variable,
    const: vscode.SymbolKind.Constant,
    field: vscode.SymbolKind.Field,
    interface: vscode.SymbolKind.Interface,
    module: vscode.SymbolKind.Module,
    test: vscode.SymbolKind.Event,
};
function getSymbols(filePath) {
    const bin = (0, compilerPath_1.getCompilerPath)();
    const result = (0, child_process_1.spawnSync)(bin, ['symbols', filePath], { encoding: 'utf8' });
    if (result.error || result.status !== 0)
        return [];
    try {
        return JSON.parse(result.stdout.trim());
    }
    catch {
        return [];
    }
}
class StashaDocumentSymbolProvider {
    provideDocumentSymbols(doc) {
        const syms = getSymbols(doc.uri.fsPath);
        return syms.map(s => {
            const kind = KIND_MAP[s.kind] ?? vscode.SymbolKind.Variable;
            const line = Math.max(0, s.line);
            const col = Math.max(0, s.col);
            const range = new vscode.Range(line, col, line, col + s.name.length);
            return new vscode.DocumentSymbol(s.name, s.detail ?? '', kind, range, range);
        });
    }
}
exports.StashaDocumentSymbolProvider = StashaDocumentSymbolProvider;
//# sourceMappingURL=symbolProvider.js.map