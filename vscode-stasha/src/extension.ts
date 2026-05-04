import * as vscode from 'vscode';
import { StashaDiagnosticProvider } from './diagnosticProvider';
import { StashaDefinitionProvider } from './definitionProvider';
import { StashaDocumentSymbolProvider } from './symbolProvider';
import { StashaHoverProvider } from './hoverProvider';
import { StashaTaskProvider } from './taskProvider';
import { StashaDebugConfigProvider } from './debugConfigProvider';
import { registerProjectCommands } from './projectCommands';
import { StashaFoldingRangeProvider } from './foldingRangeProvider';
import { StashaStatusBar } from './statusBar';
import { StashaSemanticTokensProvider, SEMANTIC_LEGEND } from './semanticTokensProvider';
import { StashaWorkspaceSymbolProvider } from './workspaceSymbolProvider';
import { StashaCodeActionProvider } from './codeActionProvider';
import { StashaProjectProvider, openProjectWebview } from './projectPanel';
import { createTestController } from './testController';
import { StashaSignatureHelpProvider } from './signatureHelpProvider';
import { StashaCompletionProvider } from './completionProvider';
import { StashaInlayHintProvider } from './inlayHintProvider';
import { StashaFormatProvider } from './formatProvider';
import { StashaReferencesProvider } from './referencesProvider';
import { StashaRenameProvider } from './renameProvider';
import { StashaCodeLensProvider } from './codeLensProvider';

const STASHA_LANG = { language: 'stasha' };

export function activate(ctx: vscode.ExtensionContext) {
    // ── Diagnostics ──────────────────────────────────────────────────────────
    const diagnostics = vscode.languages.createDiagnosticCollection('stasha');
    ctx.subscriptions.push(diagnostics);
    const diagProvider = new StashaDiagnosticProvider(diagnostics);
    ctx.subscriptions.push(diagProvider);

    // ── Status Bar ───────────────────────────────────────────────────────────
    const statusBar = new StashaStatusBar(diagProvider, diagnostics);
    ctx.subscriptions.push(statusBar);

    // ── Core Language Providers ───────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerDefinitionProvider(STASHA_LANG, new StashaDefinitionProvider()),
        vscode.languages.registerDocumentSymbolProvider(STASHA_LANG, new StashaDocumentSymbolProvider()),
        vscode.languages.registerHoverProvider(STASHA_LANG, new StashaHoverProvider()),
        vscode.languages.registerFoldingRangeProvider(STASHA_LANG, new StashaFoldingRangeProvider()),
    );

    // ── Semantic Tokens ───────────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerDocumentSemanticTokensProvider(
            STASHA_LANG,
            new StashaSemanticTokensProvider(),
            SEMANTIC_LEGEND
        )
    );

    // ── Workspace Symbols ─────────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerWorkspaceSymbolProvider(new StashaWorkspaceSymbolProvider())
    );

    // ── Code Actions ──────────────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerCodeActionsProvider(
            STASHA_LANG,
            new StashaCodeActionProvider(),
            { providedCodeActionKinds: StashaCodeActionProvider.providedCodeActionKinds }
        )
    );

    // ── Signature Help ────────────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerSignatureHelpProvider(
            STASHA_LANG,
            new StashaSignatureHelpProvider(),
            { triggerCharacters: ['('], retriggerCharacters: [','] }
        )
    );

    // ── Completion ────────────────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            STASHA_LANG,
            new StashaCompletionProvider(),
            '.', ':'
        )
    );

    // ── Inlay Hints ───────────────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerInlayHintsProvider(STASHA_LANG, new StashaInlayHintProvider())
    );

    // ── Formatting ────────────────────────────────────────────────────────────
    const formatProvider = new StashaFormatProvider();
    ctx.subscriptions.push(
        vscode.languages.registerDocumentFormattingEditProvider(STASHA_LANG, formatProvider)
    );

    // Format on save
    ctx.subscriptions.push(
        vscode.workspace.onWillSaveTextDocument(e => {
            if (e.document.languageId !== 'stasha') return;
            const cfg = vscode.workspace.getConfiguration('stasha');
            if (!cfg.get<boolean>('formatOnSave', false)) return;
            const edits = formatProvider.provideDocumentFormattingEdits(e.document);
            if (edits && edits.length > 0) e.waitUntil(Promise.resolve(edits));
        })
    );

    // ── References & Rename ───────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.languages.registerReferenceProvider(STASHA_LANG, new StashaReferencesProvider()),
        vscode.languages.registerRenameProvider(STASHA_LANG, new StashaRenameProvider()),
        vscode.languages.registerCodeLensProvider(STASHA_LANG, new StashaCodeLensProvider()),
    );

    // ── Tasks & Debug ─────────────────────────────────────────────────────────
    ctx.subscriptions.push(
        vscode.tasks.registerTaskProvider('stasha', new StashaTaskProvider())
    );
    ctx.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('stasha', new StashaDebugConfigProvider())
    );

    // ── Project Commands ─────────────────────────────────────────────────────
    registerProjectCommands(ctx);

    // ── Project Panel (Activity Bar Sidebar) ──────────────────────────────────
    const projProvider = new StashaProjectProvider();
    ctx.subscriptions.push(projProvider);
    ctx.subscriptions.push(
        vscode.window.registerTreeDataProvider('stashaProject', projProvider)
    );

    ctx.subscriptions.push(
        vscode.commands.registerCommand('stasha.openProjectPanel', () => {
            openProjectWebview(ctx, projProvider);
        }),
        vscode.commands.registerCommand('stasha.refreshProject', () => {
            projProvider.refresh();
        }),
        vscode.commands.registerCommand('stasha.openProjectFile', (rel: string) => {
            const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
            if (!ws) return;
            const abs = require('path').join(ws, rel);
            vscode.window.showTextDocument(vscode.Uri.file(abs));
        }),
        vscode.commands.registerCommand('stasha.setMain', async () => {
            const uris = await vscode.window.showOpenDialog({
                canSelectFiles: true, canSelectMany: false,
                filters: { 'Stasha': ['sts'] },
                defaultUri: vscode.workspace.workspaceFolders?.[0]?.uri,
            });
            if (!uris || uris.length === 0) return;
            openProjectWebview(ctx, projProvider);
        }),
        vscode.commands.registerCommand('stasha.removeExtLib', async (node: any) => {
            if (node?.libIndex === undefined) return;
            openProjectWebview(ctx, projProvider);
        }),
    );

    // Refresh project panel on sts.sproj change
    const sprojWatcher = vscode.workspace.createFileSystemWatcher('**/sts.sproj');
    ctx.subscriptions.push(sprojWatcher);
    sprojWatcher.onDidChange(() => projProvider.refresh());
    sprojWatcher.onDidCreate(() => projProvider.refresh());
    sprojWatcher.onDidDelete(() => projProvider.refresh());

    // ── Test Explorer ─────────────────────────────────────────────────────────
    createTestController(ctx);

    // ── Initial diagnostics on open .sts files ────────────────────────────────
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === 'stasha') diagProvider.lint(doc);
    }
}

export function deactivate() {}
