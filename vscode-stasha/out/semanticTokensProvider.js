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
exports.StashaSemanticTokensProvider = exports.SEMANTIC_LEGEND = void 0;
const vscode = __importStar(require("vscode"));
const child_process_1 = require("child_process");
const compilerPath_1 = require("./compilerPath");
// Must match the legend declared in package.json contributions.semanticTokenTypes
exports.SEMANTIC_LEGEND = new vscode.SemanticTokensLegend(['keyword', 'type', 'function', 'variable', 'parameter', 'property',
    'string', 'number', 'comment', 'operator', 'decorator', 'enumMember'], ['defaultLibrary', 'declaration', 'definition']);
function kindToIndex(kind, text) {
    switch (kind) {
        case 'keyword': return { type: 0, modifiers: 0 };
        case 'type': return { type: 1, modifiers: 0 };
        case 'identifier': return null; // let TextMate handle plain identifiers
        case 'number': return { type: 7, modifiers: 0 };
        case 'string': return { type: 6, modifiers: 0 };
        case 'comment': return { type: 8, modifiers: 0 };
        case 'operator': return { type: 9, modifiers: 0 };
        case 'delimiter': return null;
        case 'eof': return null;
        case 'error': return null;
        default: return null;
    }
}
class StashaSemanticTokensProvider {
    provideDocumentSemanticTokens(doc) {
        const cfg = vscode.workspace.getConfiguration('stasha');
        if (!cfg.get('enableSemanticTokens', true))
            return undefined;
        const bin = (0, compilerPath_1.getCompilerPath)();
        const result = (0, child_process_1.spawnSync)(bin, ['tokens', '--stdin', '--path', doc.uri.fsPath], {
            input: doc.getText(),
            encoding: 'utf8',
            timeout: 5000,
        });
        if (result.error || result.status !== 0)
            return undefined;
        let raw;
        try {
            raw = JSON.parse(result.stdout.trim());
        }
        catch {
            return undefined;
        }
        const builder = new vscode.SemanticTokensBuilder(exports.SEMANTIC_LEGEND);
        for (const tok of raw.tokens ?? []) {
            const mapped = kindToIndex(tok.kind, tok.text);
            if (!mapped)
                continue;
            try {
                builder.push(tok.line, tok.col, tok.len, mapped.type, mapped.modifiers);
            }
            catch {
                // out-of-range token — skip
            }
        }
        return builder.build();
    }
}
exports.StashaSemanticTokensProvider = StashaSemanticTokensProvider;
//# sourceMappingURL=semanticTokensProvider.js.map