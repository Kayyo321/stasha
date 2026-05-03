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
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = __importStar(require("vscode"));
const diagnosticProvider_1 = require("./diagnosticProvider");
const definitionProvider_1 = require("./definitionProvider");
const symbolProvider_1 = require("./symbolProvider");
const hoverProvider_1 = require("./hoverProvider");
const taskProvider_1 = require("./taskProvider");
const debugConfigProvider_1 = require("./debugConfigProvider");
const projectCommands_1 = require("./projectCommands");
const foldingRangeProvider_1 = require("./foldingRangeProvider");
const statusBar_1 = require("./statusBar");
const semanticTokensProvider_1 = require("./semanticTokensProvider");
const workspaceSymbolProvider_1 = require("./workspaceSymbolProvider");
const codeActionProvider_1 = require("./codeActionProvider");
const projectPanel_1 = require("./projectPanel");
const testController_1 = require("./testController");
const signatureHelpProvider_1 = require("./signatureHelpProvider");
const completionProvider_1 = require("./completionProvider");
const inlayHintProvider_1 = require("./inlayHintProvider");
const formatProvider_1 = require("./formatProvider");
const referencesProvider_1 = require("./referencesProvider");
const renameProvider_1 = require("./renameProvider");
const STASHA_LANG = { language: 'stasha' };
function activate(ctx) {
    // ── Diagnostics ──────────────────────────────────────────────────────────
    const diagnostics = vscode.languages.createDiagnosticCollection('stasha');
    ctx.subscriptions.push(diagnostics);
    const diagProvider = new diagnosticProvider_1.StashaDiagnosticProvider(diagnostics);
    ctx.subscriptions.push(diagProvider);
    // ── Status Bar ───────────────────────────────────────────────────────────
    const statusBar = new statusBar_1.StashaStatusBar(diagProvider, diagnostics);
    ctx.subscriptions.push(statusBar);
    // ── Core Language Providers ───────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerDefinitionProvider(STASHA_LANG, new definitionProvider_1.StashaDefinitionProvider()), vscode.languages.registerDocumentSymbolProvider(STASHA_LANG, new symbolProvider_1.StashaDocumentSymbolProvider()), vscode.languages.registerHoverProvider(STASHA_LANG, new hoverProvider_1.StashaHoverProvider()), vscode.languages.registerFoldingRangeProvider(STASHA_LANG, new foldingRangeProvider_1.StashaFoldingRangeProvider()));
    // ── Semantic Tokens ───────────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerDocumentSemanticTokensProvider(STASHA_LANG, new semanticTokensProvider_1.StashaSemanticTokensProvider(), semanticTokensProvider_1.SEMANTIC_LEGEND));
    // ── Workspace Symbols ─────────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerWorkspaceSymbolProvider(new workspaceSymbolProvider_1.StashaWorkspaceSymbolProvider()));
    // ── Code Actions ──────────────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerCodeActionsProvider(STASHA_LANG, new codeActionProvider_1.StashaCodeActionProvider(), { providedCodeActionKinds: codeActionProvider_1.StashaCodeActionProvider.providedCodeActionKinds }));
    // ── Signature Help ────────────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerSignatureHelpProvider(STASHA_LANG, new signatureHelpProvider_1.StashaSignatureHelpProvider(), { triggerCharacters: ['('], retriggerCharacters: [','] }));
    // ── Completion ────────────────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerCompletionItemProvider(STASHA_LANG, new completionProvider_1.StashaCompletionProvider(), '.', ':'));
    // ── Inlay Hints ───────────────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerInlayHintsProvider(STASHA_LANG, new inlayHintProvider_1.StashaInlayHintProvider()));
    // ── Formatting ────────────────────────────────────────────────────────────
    const formatProvider = new formatProvider_1.StashaFormatProvider();
    ctx.subscriptions.push(vscode.languages.registerDocumentFormattingEditProvider(STASHA_LANG, formatProvider));
    // Format on save
    ctx.subscriptions.push(vscode.workspace.onWillSaveTextDocument(e => {
        if (e.document.languageId !== 'stasha')
            return;
        const cfg = vscode.workspace.getConfiguration('stasha');
        if (!cfg.get('formatOnSave', false))
            return;
        const edits = formatProvider.provideDocumentFormattingEdits(e.document);
        if (edits && edits.length > 0)
            e.waitUntil(Promise.resolve(edits));
    }));
    // ── References & Rename ───────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.languages.registerReferenceProvider(STASHA_LANG, new referencesProvider_1.StashaReferencesProvider()), vscode.languages.registerRenameProvider(STASHA_LANG, new renameProvider_1.StashaRenameProvider()));
    // ── Tasks & Debug ─────────────────────────────────────────────────────────
    ctx.subscriptions.push(vscode.tasks.registerTaskProvider('stasha', new taskProvider_1.StashaTaskProvider()));
    ctx.subscriptions.push(vscode.debug.registerDebugConfigurationProvider('stasha', new debugConfigProvider_1.StashaDebugConfigProvider()));
    // ── Project Commands ─────────────────────────────────────────────────────
    (0, projectCommands_1.registerProjectCommands)(ctx);
    // ── Project Panel (Activity Bar Sidebar) ──────────────────────────────────
    const projProvider = new projectPanel_1.StashaProjectProvider();
    ctx.subscriptions.push(projProvider);
    ctx.subscriptions.push(vscode.window.registerTreeDataProvider('stashaProject', projProvider));
    ctx.subscriptions.push(vscode.commands.registerCommand('stasha.openProjectPanel', () => {
        (0, projectPanel_1.openProjectWebview)(ctx, projProvider);
    }), vscode.commands.registerCommand('stasha.refreshProject', () => {
        projProvider.refresh();
    }), vscode.commands.registerCommand('stasha.openProjectFile', (rel) => {
        const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        if (!ws)
            return;
        const abs = require('path').join(ws, rel);
        vscode.window.showTextDocument(vscode.Uri.file(abs));
    }), vscode.commands.registerCommand('stasha.setMain', async () => {
        const uris = await vscode.window.showOpenDialog({
            canSelectFiles: true, canSelectMany: false,
            filters: { 'Stasha': ['sts'] },
            defaultUri: vscode.workspace.workspaceFolders?.[0]?.uri,
        });
        if (!uris || uris.length === 0)
            return;
        (0, projectPanel_1.openProjectWebview)(ctx, projProvider);
    }), vscode.commands.registerCommand('stasha.removeExtLib', async (node) => {
        if (node?.libIndex === undefined)
            return;
        (0, projectPanel_1.openProjectWebview)(ctx, projProvider);
    }));
    // Refresh project panel on sts.sproj change
    const sprojWatcher = vscode.workspace.createFileSystemWatcher('**/sts.sproj');
    ctx.subscriptions.push(sprojWatcher);
    sprojWatcher.onDidChange(() => projProvider.refresh());
    sprojWatcher.onDidCreate(() => projProvider.refresh());
    sprojWatcher.onDidDelete(() => projProvider.refresh());
    // ── Test Explorer ─────────────────────────────────────────────────────────
    (0, testController_1.createTestController)(ctx);
    // ── Initial diagnostics on open .sts files ────────────────────────────────
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === 'stasha')
            diagProvider.lint(doc);
    }
}
function deactivate() { }
//# sourceMappingURL=extension.js.map