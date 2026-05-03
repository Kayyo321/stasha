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
exports.StashaWorkspaceSymbolProvider = void 0;
const vscode = __importStar(require("vscode"));
const symbolProvider_1 = require("./symbolProvider");
const path = __importStar(require("path"));
const KIND_MAP = {
    function: vscode.SymbolKind.Function,
    method: vscode.SymbolKind.Method,
    struct: vscode.SymbolKind.Struct,
    enum: vscode.SymbolKind.Enum,
    enumMember: vscode.SymbolKind.EnumMember,
    type: vscode.SymbolKind.TypeParameter,
    variable: vscode.SymbolKind.Variable,
    field: vscode.SymbolKind.Field,
    interface: vscode.SymbolKind.Interface,
    module: vscode.SymbolKind.Module,
};
class StashaWorkspaceSymbolProvider {
    async provideWorkspaceSymbols(query) {
        const files = await vscode.workspace.findFiles('**/*.sts', '**/node_modules/**');
        const results = [];
        const lq = query.toLowerCase();
        for (const file of files) {
            const syms = (0, symbolProvider_1.getSymbols)(file.fsPath);
            for (const s of syms) {
                if (query && !s.name.toLowerCase().includes(lq))
                    continue;
                const kind = KIND_MAP[s.kind] ?? vscode.SymbolKind.Variable;
                const loc = new vscode.Location(file, new vscode.Position(Math.max(0, s.line), Math.max(0, s.col)));
                results.push(new vscode.SymbolInformation(s.name, kind, s.detail ?? path.basename(file.fsPath), loc));
            }
        }
        return results;
    }
}
exports.StashaWorkspaceSymbolProvider = StashaWorkspaceSymbolProvider;
//# sourceMappingURL=workspaceSymbolProvider.js.map