import * as vscode from 'vscode';
import { spawn } from 'child_process';
import { getCompilerPath } from './compilerPath';
import { getSymbols } from './symbolProvider';

export function createTestController(ctx: vscode.ExtensionContext): vscode.TestController {
    const ctrl = vscode.tests.createTestController('stasha', 'Stasha Tests');
    ctx.subscriptions.push(ctrl);

    // Discover tests in all .sts files when workspace opens
    _discoverAll(ctrl);

    // Re-discover on file open/save
    ctx.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (doc.languageId === 'stasha') _discoverFile(ctrl, doc.uri);
        }),
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (doc.languageId === 'stasha') _discoverFile(ctrl, doc.uri);
        }),
        vscode.workspace.onDidDeleteFiles(e => {
            for (const uri of e.files) ctrl.items.delete(uri.toString());
        })
    );

    ctrl.createRunProfile('Run', vscode.TestRunProfileKind.Run, (req, token) => {
        _runTests(ctrl, req, token);
    });

    return ctrl;
}

async function _discoverAll(ctrl: vscode.TestController) {
    const files = await vscode.workspace.findFiles('**/*.sts', '**/node_modules/**');
    for (const uri of files) _discoverFile(ctrl, uri);
}

function _discoverFile(ctrl: vscode.TestController, uri: vscode.Uri) {
    const syms = getSymbols(uri.fsPath);
    const tests = syms.filter(s => s.kind === 'test' || s.kind === 'event');
    if (tests.length === 0) {
        ctrl.items.delete(uri.toString());
        return;
    }

    const fileItem = ctrl.createTestItem(uri.toString(), _basename(uri.fsPath), uri);
    fileItem.canResolveChildren = false;
    for (const t of tests) {
        const id = `${uri.toString()}::${t.name}`;
        const item = ctrl.createTestItem(id, t.name, uri);
        item.range = new vscode.Range(
            Math.max(0, t.line), Math.max(0, t.col),
            Math.max(0, t.line), Math.max(0, t.col) + t.name.length
        );
        fileItem.children.add(item);
    }
    ctrl.items.add(fileItem);
}

function _basename(p: string): string {
    return p.split('/').pop() ?? p;
}

function _runTests(
    ctrl: vscode.TestController,
    req: vscode.TestRunRequest,
    token: vscode.CancellationToken
) {
    const run = ctrl.createTestRun(req);
    const bin = getCompilerPath();

    // Collect tests to run, grouped by file
    const byFile = new Map<string, vscode.TestItem[]>();
    const include = req.include ?? _allItems(ctrl);
    for (const item of include) {
        if (item.uri && item.parent) {
            // leaf test item
            const key = item.uri.fsPath;
            if (!byFile.has(key)) byFile.set(key, []);
            byFile.get(key)!.push(item);
        } else if (item.uri && !item.parent) {
            // file-level item — run all children
            item.children.forEach(child => {
                const key = item.uri!.fsPath;
                if (!byFile.has(key)) byFile.set(key, []);
                byFile.get(key)!.push(child);
            });
        }
    }

    const promises: Promise<void>[] = [];
    for (const [filePath, items] of byFile) {
        for (const item of items) run.started(item);

        const p = new Promise<void>(resolve => {
            const proc = spawn(bin, ['test', filePath], { stdio: 'pipe' });
            let out = '';
            proc.stdout.on('data', (d: Buffer) => { out += d.toString(); });
            proc.stderr.on('data', (d: Buffer) => { out += d.toString(); });

            token.onCancellationRequested(() => proc.kill());

            proc.on('close', code => {
                for (const item of items) {
                    const testName = item.label;
                    // Parse output for per-test results
                    // Compiler emits lines like: "PASS test_name" / "FAIL test_name: reason"
                    const passRe = new RegExp(`\\bPASS\\b.*${escapeRegex(testName)}`);
                    const failRe = new RegExp(`\\bFAIL\\b.*${escapeRegex(testName)}:?\\s*(.*)`);
                    const failM  = out.match(failRe);
                    const passM  = out.match(passRe);

                    if (failM) {
                        run.failed(item, new vscode.TestMessage(failM[1] || 'Test failed'));
                    } else if (passM) {
                        run.passed(item);
                    } else if (code === 0) {
                        run.passed(item);
                    } else {
                        run.failed(item, new vscode.TestMessage(out.trim() || 'Test failed'));
                    }
                }
                resolve();
            });
        });
        promises.push(p);
    }

    Promise.all(promises).then(() => run.end());
}

function _allItems(ctrl: vscode.TestController): vscode.TestItem[] {
    const items: vscode.TestItem[] = [];
    ctrl.items.forEach(item => items.push(item));
    return items;
}

function escapeRegex(s: string): string {
    return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}
