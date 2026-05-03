import * as vscode from 'vscode';
import { StashaDiagnosticProvider } from './diagnosticProvider';

export class StashaStatusBar {
    private item: vscode.StatusBarItem;

    constructor(
        private diagProvider: StashaDiagnosticProvider,
        private collection: vscode.DiagnosticCollection
    ) {
        this.item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 10);
        this.item.command = 'workbench.action.showErrorsWarnings';
        this.item.tooltip = 'Stasha — click to see problems';
        this._update();

        diagProvider.onDidUpdate(() => this._update());
        vscode.window.onDidChangeActiveTextEditor(() => this._update());
    }

    private _update() {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== 'stasha') {
            this.item.hide();
            return;
        }

        let errors = 0;
        let warnings = 0;
        this.collection.forEach((uri, diags) => {
            for (const d of diags) {
                if (d.severity === vscode.DiagnosticSeverity.Error) errors++;
                else if (d.severity === vscode.DiagnosticSeverity.Warning) warnings++;
            }
        });

        const parts: string[] = ['$(tools) Stasha'];
        if (errors > 0) parts.push(`$(error) ${errors}`);
        if (warnings > 0) parts.push(`$(warning) ${warnings}`);

        this.item.text = parts.join(' ');
        this.item.backgroundColor = errors > 0
            ? new vscode.ThemeColor('statusBarItem.errorBackground')
            : undefined;
        this.item.show();
    }

    dispose() {
        this.item.dispose();
    }
}
