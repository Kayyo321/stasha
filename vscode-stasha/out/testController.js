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
exports.createTestController = createTestController;
const vscode = __importStar(require("vscode"));
const child_process_1 = require("child_process");
const compilerPath_1 = require("./compilerPath");
const symbolProvider_1 = require("./symbolProvider");
function createTestController(ctx) {
    const ctrl = vscode.tests.createTestController('stasha', 'Stasha Tests');
    ctx.subscriptions.push(ctrl);
    // Discover tests in all .sts files when workspace opens
    _discoverAll(ctrl);
    // Re-discover on file open/save
    ctx.subscriptions.push(vscode.workspace.onDidOpenTextDocument(doc => {
        if (doc.languageId === 'stasha')
            _discoverFile(ctrl, doc.uri);
    }), vscode.workspace.onDidSaveTextDocument(doc => {
        if (doc.languageId === 'stasha')
            _discoverFile(ctrl, doc.uri);
    }), vscode.workspace.onDidDeleteFiles(e => {
        for (const uri of e.files)
            ctrl.items.delete(uri.toString());
    }));
    ctrl.createRunProfile('Run', vscode.TestRunProfileKind.Run, (req, token) => {
        _runTests(ctrl, req, token);
    });
    return ctrl;
}
async function _discoverAll(ctrl) {
    const files = await vscode.workspace.findFiles('**/*.sts', '**/node_modules/**');
    for (const uri of files)
        _discoverFile(ctrl, uri);
}
function _discoverFile(ctrl, uri) {
    const syms = (0, symbolProvider_1.getSymbols)(uri.fsPath);
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
        item.range = new vscode.Range(Math.max(0, t.line), Math.max(0, t.col), Math.max(0, t.line), Math.max(0, t.col) + t.name.length);
        fileItem.children.add(item);
    }
    ctrl.items.add(fileItem);
}
function _basename(p) {
    return p.split('/').pop() ?? p;
}
function _runTests(ctrl, req, token) {
    const run = ctrl.createTestRun(req);
    const bin = (0, compilerPath_1.getCompilerPath)();
    // Collect tests to run, grouped by file
    const byFile = new Map();
    const include = req.include ?? _allItems(ctrl);
    for (const item of include) {
        if (item.uri && item.parent) {
            // leaf test item
            const key = item.uri.fsPath;
            if (!byFile.has(key))
                byFile.set(key, []);
            byFile.get(key).push(item);
        }
        else if (item.uri && !item.parent) {
            // file-level item — run all children
            item.children.forEach(child => {
                const key = item.uri.fsPath;
                if (!byFile.has(key))
                    byFile.set(key, []);
                byFile.get(key).push(child);
            });
        }
    }
    const promises = [];
    for (const [filePath, items] of byFile) {
        for (const item of items)
            run.started(item);
        const p = new Promise(resolve => {
            const proc = (0, child_process_1.spawn)(bin, ['test', filePath], { stdio: 'pipe' });
            let out = '';
            proc.stdout.on('data', (d) => { out += d.toString(); });
            proc.stderr.on('data', (d) => { out += d.toString(); });
            token.onCancellationRequested(() => proc.kill());
            proc.on('close', code => {
                for (const item of items) {
                    const testName = item.label;
                    // Parse output for per-test results
                    // Compiler emits lines like: "PASS test_name" / "FAIL test_name: reason"
                    const passRe = new RegExp(`\\bPASS\\b.*${escapeRegex(testName)}`);
                    const failRe = new RegExp(`\\bFAIL\\b.*${escapeRegex(testName)}:?\\s*(.*)`);
                    const failM = out.match(failRe);
                    const passM = out.match(passRe);
                    if (failM) {
                        run.failed(item, new vscode.TestMessage(failM[1] || 'Test failed'));
                    }
                    else if (passM) {
                        run.passed(item);
                    }
                    else if (code === 0) {
                        run.passed(item);
                    }
                    else {
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
function _allItems(ctrl) {
    const items = [];
    ctrl.items.forEach(item => items.push(item));
    return items;
}
function escapeRegex(s) {
    return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}
//# sourceMappingURL=testController.js.map