import * as vscode from 'vscode';
import { StashaDiagnosticProvider } from './diagnosticProvider';
import { StashaDefinitionProvider } from './definitionProvider';
import { StashaDocumentSymbolProvider } from './symbolProvider';
import { StashaHoverProvider } from './hoverProvider';
import { StashaTaskProvider } from './taskProvider';
import { StashaDebugConfigProvider } from './debugConfigProvider';
import { registerProjectCommands } from './projectCommands';

const STASHA_LANG = { language: 'stasha' };

export function activate(ctx: vscode.ExtensionContext) {
    const diagnostics = vscode.languages.createDiagnosticCollection('stasha');
    ctx.subscriptions.push(diagnostics);

    const diagProvider = new StashaDiagnosticProvider(diagnostics);
    ctx.subscriptions.push(diagProvider);

    ctx.subscriptions.push(
        vscode.languages.registerDefinitionProvider(STASHA_LANG, new StashaDefinitionProvider()),
        vscode.languages.registerDocumentSymbolProvider(STASHA_LANG, new StashaDocumentSymbolProvider()),
        vscode.languages.registerHoverProvider(STASHA_LANG, new StashaHoverProvider()),
    );

    ctx.subscriptions.push(
        vscode.tasks.registerTaskProvider('stasha', new StashaTaskProvider())
    );

    ctx.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('stasha', new StashaDebugConfigProvider())
    );

    registerProjectCommands(ctx);

    // Run diagnostics on currently open .sts files
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === 'stasha') {
            diagProvider.lint(doc);
        }
    }
}

export function deactivate() {}
