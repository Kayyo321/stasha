import * as vscode from 'vscode';

export class StashaCodeLensProvider implements vscode.CodeLensProvider {
    provideCodeLenses(doc: vscode.TextDocument): vscode.CodeLens[] {
        const lenses: vscode.CodeLens[] = [];
        const filePath = doc.uri.fsPath;

        for (let i = 0; i < doc.lineCount; i++) {
            const text = doc.lineAt(i).text;
            const range = new vscode.Range(i, 0, i, text.length);

            if (/^\s*(ext\s+)?fn\s+main\s*\(/.test(text)) {
                lenses.push(new vscode.CodeLens(range, {
                    title: '▶ Run',
                    command: 'stasha.run',
                    arguments: [filePath],
                }));
                lenses.push(new vscode.CodeLens(range, {
                    title: '⚡ Debug',
                    command: 'stasha.startDebug',
                    arguments: [filePath],
                }));
            }

            const testMatch = text.match(/^\s*test\s+'([^']+)'/);
            if (testMatch) {
                lenses.push(new vscode.CodeLens(range, {
                    title: `▶ Run Test "${testMatch[1]}"`,
                    command: 'stasha.runTestFile',
                    arguments: [filePath],
                }));
            }
        }
        return lenses;
    }
}
