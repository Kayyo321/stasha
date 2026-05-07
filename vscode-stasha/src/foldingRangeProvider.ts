import * as vscode from 'vscode';

export class StashaFoldingRangeProvider implements vscode.FoldingRangeProvider {
    provideFoldingRanges(doc: vscode.TextDocument): vscode.FoldingRange[] {
        const ranges: vscode.FoldingRange[] = [];
        const stack: number[] = [];

        for (let i = 0; i < doc.lineCount; i++) {
            const line = doc.lineAt(i).text;

            let inStr = false;
            let strChar = '';
            for (let j = 0; j < line.length; j++) {
                const ch = line[j];
                if (inStr) {
                    if (ch === strChar && line[j - 1] !== '\\') inStr = false;
                    continue;
                }
                if (ch === '"' || ch === "'") { inStr = true; strChar = ch; continue; }
                if (ch === '/' && line[j + 1] === '/') break;
                if (ch === '{') {
                    stack.push(i);
                } else if (ch === '}') {
                    if (stack.length > 0) {
                        const start = stack.pop()!;
                        if (i > start) {
                            ranges.push(new vscode.FoldingRange(start, i));
                        }
                    }
                }
            }
        }

        // Block comment folding
        let blockStart = -1;
        for (let i = 0; i < doc.lineCount; i++) {
            const text = doc.lineAt(i).text.trim();
            if (blockStart === -1 && text.startsWith('/*')) {
                blockStart = i;
            }
            if (blockStart !== -1 && text.endsWith('*/')) {
                if (i > blockStart) {
                    ranges.push(new vscode.FoldingRange(blockStart, i, vscode.FoldingRangeKind.Comment));
                }
                blockStart = -1;
            }
        }

        return ranges;
    }
}
