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
exports.StashaDiagnosticProvider = void 0;
const vscode = __importStar(require("vscode"));
const child_process_1 = require("child_process");
const compilerPath_1 = require("./compilerPath");
function toVscodeSeverity(s) {
    if (s === 'error')
        return vscode.DiagnosticSeverity.Error;
    if (s === 'warning')
        return vscode.DiagnosticSeverity.Warning;
    return vscode.DiagnosticSeverity.Information;
}
function makeRange(line, col, len, doc) {
    const l = Math.max(0, line);
    let c = Math.max(0, col);
    if (c === 0 && doc && l < doc.lineCount) {
        c = doc.lineAt(l).firstNonWhitespaceCharacterIndex;
    }
    const lineLen = doc && l < doc.lineCount ? doc.lineAt(l).text.length : c + Math.max(1, len);
    const end = Math.min(c + Math.max(1, len), lineLen);
    return new vscode.Range(l, c, l, end);
}
class StashaDiagnosticProvider {
    constructor(collection) {
        this.collection = collection;
        this.timers = new Map();
        this._onDidUpdate = new vscode.EventEmitter();
        this.onDidUpdate = this._onDidUpdate.event;
        this._register();
    }
    _register() {
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (doc.languageId === 'stasha')
                this.lint(doc);
        });
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (doc.languageId === 'stasha')
                this.lint(doc);
        });
        vscode.workspace.onDidCloseTextDocument(doc => {
            this.collection.delete(doc.uri);
            const key = doc.uri.toString();
            const t = this.timers.get(key);
            if (t) {
                clearTimeout(t);
                this.timers.delete(key);
            }
        });
        vscode.workspace.onDidChangeTextDocument(e => {
            const doc = e.document;
            if (doc.languageId !== 'stasha')
                return;
            const cfg = vscode.workspace.getConfiguration('stasha');
            if (!cfg.get('checkOnType', true))
                return;
            const delay = cfg.get('checkDelay', 400);
            const key = doc.uri.toString();
            const existing = this.timers.get(key);
            if (existing)
                clearTimeout(existing);
            this.timers.set(key, setTimeout(() => {
                this.timers.delete(key);
                this.lint(doc);
            }, delay));
        });
    }
    lint(doc) {
        const bin = (0, compilerPath_1.getCompilerPath)();
        const filePath = doc.uri.fsPath;
        const args = ['check', '--stdin', '--path', filePath];
        const proc = (0, child_process_1.spawn)(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
        let stdout = '';
        let stderr = '';
        let hadSpawnError = false;
        proc.stdout.on('data', (d) => { stdout += d.toString(); });
        proc.stderr.on('data', (d) => { stderr += d.toString(); });
        proc.on('error', (err) => {
            hadSpawnError = true;
            const msg = err.code === 'ENOENT'
                ? `Stasha compiler not found at '${bin}'. Set stasha.compilerPath in settings.`
                : `Failed to launch Stasha compiler: ${err.message}`;
            this.collection.set(doc.uri, [new vscode.Diagnostic(new vscode.Range(0, 0, 0, 1), msg, vscode.DiagnosticSeverity.Error)]);
            this._onDidUpdate.fire(doc.uri);
        });
        proc.on('close', () => {
            if (hadSpawnError)
                return;
            const diags = [];
            try {
                const response = JSON.parse(stdout);
                const items = response.diagnostics ?? [];
                for (const d of items) {
                    const range = makeRange(d.line, d.col, d.len, doc);
                    let msg = d.message;
                    if (d.notes && d.notes.length > 0) {
                        msg += '\n' + d.notes.map(n => `${n.kind}: ${n.message}`).join('\n');
                    }
                    const diag = new vscode.Diagnostic(range, msg, toVscodeSeverity(d.severity));
                    diag.source = 'stasha';
                    if (d.labels && d.labels.length > 0) {
                        diag.relatedInformation = d.labels.map(lbl => new vscode.DiagnosticRelatedInformation(new vscode.Location(doc.uri, makeRange(lbl.line, lbl.col, lbl.len, doc)), lbl.message));
                    }
                    if (d.notes) {
                        diag.__stashaNotes = d.notes;
                    }
                    diags.push(diag);
                }
            }
            catch {
                if (stderr.trim()) {
                    diags.push(new vscode.Diagnostic(new vscode.Range(0, 0, 0, 1), stderr.trim(), vscode.DiagnosticSeverity.Error));
                }
            }
            this.collection.set(doc.uri, diags);
            this._onDidUpdate.fire(doc.uri);
        });
        proc.stdin.write(doc.getText());
        proc.stdin.end();
    }
    dispose() {
        for (const t of this.timers.values())
            clearTimeout(t);
        this.timers.clear();
        this._onDidUpdate.dispose();
    }
}
exports.StashaDiagnosticProvider = StashaDiagnosticProvider;
//# sourceMappingURL=diagnosticProvider.js.map