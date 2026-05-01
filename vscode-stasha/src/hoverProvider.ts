import * as vscode from 'vscode';
import { getSymbols, StashaSymbol } from './symbolProvider';

// Cache symbols per file, invalidated on document change
const cache = new Map<string, { version: number; symbols: StashaSymbol[] }>();

vscode.workspace.onDidChangeTextDocument(e => {
    cache.delete(e.document.uri.toString());
});

function getCached(doc: vscode.TextDocument): StashaSymbol[] {
    const key = doc.uri.toString();
    const entry = cache.get(key);
    if (entry && entry.version === doc.version) return entry.symbols;
    const symbols = getSymbols(doc.uri.fsPath);
    cache.set(key, { version: doc.version, symbols });
    return symbols;
}

export class StashaHoverProvider implements vscode.HoverProvider {
    provideHover(doc: vscode.TextDocument, pos: vscode.Position): vscode.Hover | undefined {
        const wordRange = doc.getWordRangeAtPosition(pos, /[a-zA-Z_][a-zA-Z0-9_]*/);
        if (!wordRange) return undefined;
        const word = doc.getText(wordRange);

        const symbols = getCached(doc);
        const match = symbols.find(s => s.name === word);
        if (!match) return undefined;

        const md = new vscode.MarkdownString();
        md.appendCodeblock(`${match.kind} ${match.name}${match.detail ? ': ' + match.detail : ''}`, 'stasha');
        return new vscode.Hover(md, wordRange);
    }
}
