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
exports.StashaHoverProvider = void 0;
const vscode = __importStar(require("vscode"));
const symbolProvider_1 = require("./symbolProvider");
// Cache symbols per file, invalidated on document change
const cache = new Map();
vscode.workspace.onDidChangeTextDocument(e => {
    cache.delete(e.document.uri.toString());
});
function getCached(doc) {
    const key = doc.uri.toString();
    const entry = cache.get(key);
    if (entry && entry.version === doc.version)
        return entry.symbols;
    const symbols = (0, symbolProvider_1.getSymbols)(doc.uri.fsPath);
    cache.set(key, { version: doc.version, symbols });
    return symbols;
}
class StashaHoverProvider {
    provideHover(doc, pos) {
        const wordRange = doc.getWordRangeAtPosition(pos, /[a-zA-Z_][a-zA-Z0-9_]*/);
        if (!wordRange)
            return undefined;
        const word = doc.getText(wordRange);
        const symbols = getCached(doc);
        const match = symbols.find(s => s.name === word);
        if (!match)
            return undefined;
        const md = new vscode.MarkdownString();
        md.appendCodeblock(`${match.kind} ${match.name}${match.detail ? ': ' + match.detail : ''}`, 'stasha');
        return new vscode.Hover(md, wordRange);
    }
}
exports.StashaHoverProvider = StashaHoverProvider;
//# sourceMappingURL=hoverProvider.js.map