import * as vscode from 'vscode';
import { spawn } from 'child_process';
import { getCompilerPath } from './compilerPath';

interface DiagLabel {
    line: number;
    col: number;
    len: number;
    message: string;
    primary?: boolean;
}

interface DiagNote {
    kind: 'note' | 'help';
    message: string;
}

interface CompilerDiag {
    severity: 'error' | 'warning' | 'note' | 'help';
    message: string;
    file: string;
    line: number;
    col: number;
    len: number;
    labels?: DiagLabel[];
    notes?: DiagNote[];
}

interface DiagnosticsResponse {
    diagnostics: CompilerDiag[];
}

function toVscodeSeverity(s: string): vscode.DiagnosticSeverity {
    if (s === 'error') return vscode.DiagnosticSeverity.Error;
    if (s === 'warning') return vscode.DiagnosticSeverity.Warning;
    return vscode.DiagnosticSeverity.Information;
}

function makeRange(line: number, col: number, len: number): vscode.Range {
    const l = Math.max(0, line);
    const c = Math.max(0, col);
    return new vscode.Range(l, c, l, c + Math.max(1, len));
}

export class StashaDiagnosticProvider {
    private timers = new Map<string, NodeJS.Timeout>();
    private _onDidUpdate = new vscode.EventEmitter<vscode.Uri>();
    readonly onDidUpdate = this._onDidUpdate.event;

    constructor(private collection: vscode.DiagnosticCollection) {
        this._register();
    }

    private _register() {
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (doc.languageId === 'stasha') this.lint(doc);
        });
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (doc.languageId === 'stasha') this.lint(doc);
        });
        vscode.workspace.onDidCloseTextDocument(doc => {
            this.collection.delete(doc.uri);
            const key = doc.uri.toString();
            const t = this.timers.get(key);
            if (t) { clearTimeout(t); this.timers.delete(key); }
        });
        vscode.workspace.onDidChangeTextDocument(e => {
            const doc = e.document;
            if (doc.languageId !== 'stasha') return;
            const cfg = vscode.workspace.getConfiguration('stasha');
            if (!cfg.get<boolean>('checkOnType', true)) return;
            const delay = cfg.get<number>('checkDelay', 400);
            const key = doc.uri.toString();
            const existing = this.timers.get(key);
            if (existing) clearTimeout(existing);
            this.timers.set(key, setTimeout(() => {
                this.timers.delete(key);
                this.lint(doc);
            }, delay));
        });
    }

    lint(doc: vscode.TextDocument) {
        const bin = getCompilerPath();
        const filePath = doc.uri.fsPath;
        const args = ['check', '--stdin', '--path', filePath];
        const proc = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });

        let stdout = '';
        let stderr = '';
        proc.stdout.on('data', (d: Buffer) => { stdout += d.toString(); });
        proc.stderr.on('data', (d: Buffer) => { stderr += d.toString(); });

        proc.on('error', (err) => {
            if ((err as NodeJS.ErrnoException).code === 'ENOENT') {
                this.collection.set(doc.uri, [new vscode.Diagnostic(
                    new vscode.Range(0, 0, 0, 0),
                    `Stasha compiler not found ('${bin}'). Set stasha.compilerPath in settings.`,
                    vscode.DiagnosticSeverity.Error
                )]);
            }
        });

        proc.on('close', () => {
            const diags: vscode.Diagnostic[] = [];
            try {
                const response: DiagnosticsResponse = JSON.parse(stdout);
                const items: CompilerDiag[] = response.diagnostics ?? [];
                for (const d of items) {
                    const range = makeRange(d.line, d.col, d.len);
                    let msg = d.message;
                    if (d.notes && d.notes.length > 0) {
                        msg += '\n' + d.notes.map(n => `${n.kind}: ${n.message}`).join('\n');
                    }
                    const diag = new vscode.Diagnostic(range, msg, toVscodeSeverity(d.severity));
                    diag.source = 'stasha';
                    if (d.labels && d.labels.length > 0) {
                        diag.relatedInformation = d.labels.map(lbl => new vscode.DiagnosticRelatedInformation(
                            new vscode.Location(doc.uri, makeRange(lbl.line, lbl.col, lbl.len)),
                            lbl.message
                        ));
                    }
                    // Attach raw notes for code actions
                    if (d.notes) {
                        (diag as any).__stashaNotes = d.notes;
                    }
                    diags.push(diag);
                }
            } catch {
                // Not valid JSON — show raw stderr as a single error if non-empty
                if (stderr.trim()) {
                    diags.push(new vscode.Diagnostic(
                        new vscode.Range(0, 0, 0, 0),
                        stderr.trim(),
                        vscode.DiagnosticSeverity.Error
                    ));
                }
            }
            this.collection.set(doc.uri, diags);
            this._onDidUpdate.fire(doc.uri);
        });

        proc.stdin.write(doc.getText());
        proc.stdin.end();
    }

    dispose() {
        for (const t of this.timers.values()) clearTimeout(t);
        this.timers.clear();
        this._onDidUpdate.dispose();
    }
}
