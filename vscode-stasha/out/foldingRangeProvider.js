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
exports.StashaFoldingRangeProvider = void 0;
const vscode = __importStar(require("vscode"));
class StashaFoldingRangeProvider {
    provideFoldingRanges(doc) {
        const ranges = [];
        const stack = [];
        for (let i = 0; i < doc.lineCount; i++) {
            const line = doc.lineAt(i).text;
            let inStr = false;
            let strChar = '';
            for (let j = 0; j < line.length; j++) {
                const ch = line[j];
                if (inStr) {
                    if (ch === strChar && line[j - 1] !== '\\')
                        inStr = false;
                    continue;
                }
                if (ch === '"' || ch === "'") {
                    inStr = true;
                    strChar = ch;
                    continue;
                }
                if (ch === '/' && line[j + 1] === '/')
                    break;
                if (ch === '{') {
                    stack.push(i);
                }
                else if (ch === '}') {
                    if (stack.length > 0) {
                        const start = stack.pop();
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
exports.StashaFoldingRangeProvider = StashaFoldingRangeProvider;
//# sourceMappingURL=foldingRangeProvider.js.map