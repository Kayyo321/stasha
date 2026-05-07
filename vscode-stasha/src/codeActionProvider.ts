import * as vscode from 'vscode';

// Patterns to extract a replacement from help text like:
//   "replace with 'foo'"  |  "try: foo"  |  "did you mean 'foo'?"
const REPLACE_PATTERNS = [
    /replace with ['"]([^'"]+)['"]/i,
    /try:\s+['"]?([^\s'"]+)['"]?/i,
    /did you mean ['"]([^'"]+)['"]/i,
    /use ['"]([^'"]+)['"]/i,
];

function extractReplacement(msg: string): string | undefined {
    for (const pat of REPLACE_PATTERNS) {
        const m = msg.match(pat);
        if (m) return m[1];
    }
    return undefined;
}

export class StashaCodeActionProvider implements vscode.CodeActionProvider {
    static readonly providedCodeActionKinds = [vscode.CodeActionKind.QuickFix];

    provideCodeActions(
        doc: vscode.TextDocument,
        range: vscode.Range,
        context: vscode.CodeActionContext
    ): vscode.CodeAction[] {
        const actions: vscode.CodeAction[] = [];

        for (const diag of context.diagnostics) {
            if (diag.source !== 'stasha') continue;
            const notes: Array<{ kind: string; message: string }> = (diag as any).__stashaNotes ?? [];

            for (const note of notes) {
                if (note.kind === 'help') {
                    const replacement = extractReplacement(note.message);
                    if (replacement) {
                        const fix = new vscode.CodeAction(
                            `Fix: ${note.message}`,
                            vscode.CodeActionKind.QuickFix
                        );
                        fix.diagnostics = [diag];
                        fix.edit = new vscode.WorkspaceEdit();
                        fix.edit.replace(doc.uri, diag.range, replacement);
                        fix.isPreferred = true;
                        actions.push(fix);
                    } else {
                        // Info-only action — no edit, just surfaced as insight
                        const info = new vscode.CodeAction(
                            `Help: ${note.message}`,
                            vscode.CodeActionKind.QuickFix
                        );
                        info.diagnostics = [diag];
                        actions.push(info);
                    }
                } else if (note.kind === 'note') {
                    const info = new vscode.CodeAction(
                        `Note: ${note.message}`,
                        vscode.CodeActionKind.Empty
                    );
                    info.diagnostics = [diag];
                    actions.push(info);
                }
            }
        }

        return actions;
    }
}
