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
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = __importStar(require("vscode"));
const diagnosticProvider_1 = require("./diagnosticProvider");
const definitionProvider_1 = require("./definitionProvider");
const symbolProvider_1 = require("./symbolProvider");
const hoverProvider_1 = require("./hoverProvider");
const taskProvider_1 = require("./taskProvider");
const debugConfigProvider_1 = require("./debugConfigProvider");
const projectCommands_1 = require("./projectCommands");
const STASHA_LANG = { language: 'stasha' };
function activate(ctx) {
    const diagnostics = vscode.languages.createDiagnosticCollection('stasha');
    ctx.subscriptions.push(diagnostics);
    const diagProvider = new diagnosticProvider_1.StashaDiagnosticProvider(diagnostics);
    ctx.subscriptions.push(diagProvider);
    ctx.subscriptions.push(vscode.languages.registerDefinitionProvider(STASHA_LANG, new definitionProvider_1.StashaDefinitionProvider()), vscode.languages.registerDocumentSymbolProvider(STASHA_LANG, new symbolProvider_1.StashaDocumentSymbolProvider()), vscode.languages.registerHoverProvider(STASHA_LANG, new hoverProvider_1.StashaHoverProvider()));
    ctx.subscriptions.push(vscode.tasks.registerTaskProvider('stasha', new taskProvider_1.StashaTaskProvider()));
    ctx.subscriptions.push(vscode.debug.registerDebugConfigurationProvider('stasha', new debugConfigProvider_1.StashaDebugConfigProvider()));
    (0, projectCommands_1.registerProjectCommands)(ctx);
    // Run diagnostics on currently open .sts files
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === 'stasha') {
            diagProvider.lint(doc);
        }
    }
}
function deactivate() { }
//# sourceMappingURL=extension.js.map