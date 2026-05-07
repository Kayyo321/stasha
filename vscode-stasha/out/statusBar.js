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
exports.StashaStatusBar = void 0;
const vscode = __importStar(require("vscode"));
class StashaStatusBar {
    constructor(diagProvider, collection) {
        this.diagProvider = diagProvider;
        this.collection = collection;
        this.item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 10);
        this.item.command = 'workbench.action.showErrorsWarnings';
        this.item.tooltip = 'Stasha — click to see problems';
        this._update();
        diagProvider.onDidUpdate(() => this._update());
        vscode.window.onDidChangeActiveTextEditor(() => this._update());
    }
    _update() {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== 'stasha') {
            this.item.hide();
            return;
        }
        let errors = 0;
        let warnings = 0;
        this.collection.forEach((uri, diags) => {
            for (const d of diags) {
                if (d.severity === vscode.DiagnosticSeverity.Error)
                    errors++;
                else if (d.severity === vscode.DiagnosticSeverity.Warning)
                    warnings++;
            }
        });
        const parts = ['$(tools) Stasha'];
        if (errors > 0)
            parts.push(`$(error) ${errors}`);
        if (warnings > 0)
            parts.push(`$(warning) ${warnings}`);
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
exports.StashaStatusBar = StashaStatusBar;
//# sourceMappingURL=statusBar.js.map