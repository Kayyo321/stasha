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
exports.StashaCodeActionProvider = void 0;
const vscode = __importStar(require("vscode"));
// Patterns to extract a replacement from help text like:
//   "replace with 'foo'"  |  "try: foo"  |  "did you mean 'foo'?"
const REPLACE_PATTERNS = [
    /replace with ['"]([^'"]+)['"]/i,
    /try:\s+['"]?([^\s'"]+)['"]?/i,
    /did you mean ['"]([^'"]+)['"]/i,
    /use ['"]([^'"]+)['"]/i,
];
function extractReplacement(msg) {
    for (const pat of REPLACE_PATTERNS) {
        const m = msg.match(pat);
        if (m)
            return m[1];
    }
    return undefined;
}
class StashaCodeActionProvider {
    provideCodeActions(doc, range, context) {
        const actions = [];
        for (const diag of context.diagnostics) {
            if (diag.source !== 'stasha')
                continue;
            const notes = diag.__stashaNotes ?? [];
            for (const note of notes) {
                if (note.kind === 'help') {
                    const replacement = extractReplacement(note.message);
                    if (replacement) {
                        const fix = new vscode.CodeAction(`Fix: ${note.message}`, vscode.CodeActionKind.QuickFix);
                        fix.diagnostics = [diag];
                        fix.edit = new vscode.WorkspaceEdit();
                        fix.edit.replace(doc.uri, diag.range, replacement);
                        fix.isPreferred = true;
                        actions.push(fix);
                    }
                    else {
                        // Info-only action — no edit, just surfaced as insight
                        const info = new vscode.CodeAction(`Help: ${note.message}`, vscode.CodeActionKind.QuickFix);
                        info.diagnostics = [diag];
                        actions.push(info);
                    }
                }
                else if (note.kind === 'note') {
                    const info = new vscode.CodeAction(`Note: ${note.message}`, vscode.CodeActionKind.Empty);
                    info.diagnostics = [diag];
                    actions.push(info);
                }
            }
        }
        return actions;
    }
}
exports.StashaCodeActionProvider = StashaCodeActionProvider;
StashaCodeActionProvider.providedCodeActionKinds = [vscode.CodeActionKind.QuickFix];
//# sourceMappingURL=codeActionProvider.js.map