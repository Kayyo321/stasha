"use strict";

const cp = require("child_process");
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

const LANGUAGE_ID = "stasha";
const TOKEN_TYPES = [
  "keyword",
  "string",
  "number",
  "type",
  "operator",
  "variable",
  "function",
  "method",
  "property",
  "enumMember"
];
const TOKEN_LEGEND = new vscode.SemanticTokensLegend(TOKEN_TYPES, []);

function isStashaDocument(document) {
  return document && document.languageId === LANGUAGE_ID;
}

function workspaceRootFor(document) {
  const folder = document ? vscode.workspace.getWorkspaceFolder(document.uri) : undefined;
  return folder ? folder.uri.fsPath : undefined;
}

function resolveCompilerPath(document) {
  const config = vscode.workspace.getConfiguration("stasha", document);
  const configured = config.get("compilerPath", "").trim();
  if (configured) {
    return configured;
  }

  const root = workspaceRootFor(document);
  if (root) {
    const candidates = [
      path.join(root, "bin", "stasha"),
      path.join(root, "bin", "stasha.exe")
    ];
    for (const candidate of candidates) {
      if (fs.existsSync(candidate)) {
        return candidate;
      }
    }
  }

  return process.platform === "win32" ? "stasha.exe" : "stasha";
}

function runCompiler(args, { document, stdinText } = {}) {
  const compilerPath = resolveCompilerPath(document);
  return new Promise((resolve, reject) => {
    const child = cp.spawn(compilerPath, args, {
      cwd: workspaceRootFor(document),
      stdio: ["pipe", "pipe", "pipe"]
    });

    let stdout = "";
    let stderr = "";

    child.stdout.on("data", chunk => {
      stdout += chunk.toString();
    });
    child.stderr.on("data", chunk => {
      stderr += chunk.toString();
    });
    child.on("error", reject);
    child.on("close", code => {
      resolve({ code, stdout, stderr, compilerPath, args });
    });

    if (stdinText != null) {
      child.stdin.write(stdinText);
    }
    child.stdin.end();
  });
}

async function runJsonCommand(subcommand, document) {
  const args = [subcommand, "--stdin", "--path", document.uri.fsPath];
  const result = await runCompiler(args, {
    document,
    stdinText: document.getText()
  });

  const text = result.stdout.trim();
  if (!text) {
    if (result.stderr.trim()) {
      throw new Error(result.stderr.trim());
    }
    throw new Error(`No output from ${result.compilerPath} ${result.args.join(" ")}`);
  }

  try {
    return JSON.parse(text);
  } catch (error) {
    throw new Error(`Invalid JSON from ${path.basename(result.compilerPath)}: ${text}`);
  }
}

async function runDefinitionCommand(document, position) {
  const args = [
    "definition",
    "--stdin",
    "--path",
    document.uri.fsPath,
    "--line",
    String(position.line),
    "--col",
    String(position.character)
  ];
  const result = await runCompiler(args, {
    document,
    stdinText: document.getText()
  });
  const text = result.stdout.trim();
  if (!text) {
    throw new Error(`No output from ${path.basename(result.compilerPath)} ${result.args.join(" ")}`);
  }
  return JSON.parse(text);
}

function severityFor(name) {
  switch (name) {
    case "warning":
      return vscode.DiagnosticSeverity.Warning;
    case "note":
    case "help":
      return vscode.DiagnosticSeverity.Information;
    default:
      return vscode.DiagnosticSeverity.Error;
  }
}

function diagnosticRange(document, entry) {
  const start = new vscode.Position(entry.line || 0, entry.col || 0);
  const len = Math.max(1, entry.len || 1);
  const end = new vscode.Position(entry.line || 0, (entry.col || 0) + len);
  return new vscode.Range(start, end);
}

class StashaWorkspaceIndex {
  constructor() {
    this.byFile = new Map();
    this.byName = new Map();
  }

  clear() {
    this.byFile.clear();
    this.byName.clear();
  }

  setSymbols(filePath, symbols) {
    this.byFile.set(filePath, symbols);
    this.rebuild();
  }

  remove(filePath) {
    this.byFile.delete(filePath);
    this.rebuild();
  }

  rebuild() {
    this.byName.clear();
    for (const symbols of this.byFile.values()) {
      for (const symbol of symbols) {
        const bucket = this.byName.get(symbol.name) || [];
        bucket.push(symbol);
        this.byName.set(symbol.name, bucket);
      }
    }
  }

  getByName(name) {
    return this.byName.get(name) || [];
  }

  getFileSymbols(filePath) {
    return this.byFile.get(filePath) || [];
  }
}

class StashaLanguageService {
  constructor(context) {
    this.context = context;
    this.diagnostics = vscode.languages.createDiagnosticCollection("stasha");
    this.index = new StashaWorkspaceIndex();
    this.pendingDiagnostics = new Map();
    this.pendingIndex = new Map();
    this.context.subscriptions.push(this.diagnostics);
  }

  dispose() {
    this.diagnostics.dispose();
  }

  scheduleDiagnostics(document, delay = 250) {
    if (!isStashaDocument(document)) {
      return;
    }

    const enabled = vscode.workspace.getConfiguration("stasha", document).get("diagnosticsOnType", true);
    if (!enabled && delay > 0) {
      return;
    }

    const key = document.uri.toString();
    clearTimeout(this.pendingDiagnostics.get(key));
    this.pendingDiagnostics.set(key, setTimeout(() => {
      this.pendingDiagnostics.delete(key);
      this.refreshDiagnostics(document);
    }, delay));
  }

  async refreshDiagnostics(document) {
    if (!isStashaDocument(document)) {
      return;
    }

    try {
      const payload = await runJsonCommand("check", document);
      const items = [];
      for (const entry of payload.diagnostics || []) {
        if (entry.file && path.resolve(entry.file) !== path.resolve(document.uri.fsPath)) {
          continue;
        }
        const diag = new vscode.Diagnostic(
          diagnosticRange(document, entry),
          entry.message,
          severityFor(entry.severity)
        );
        diag.source = "stasha";
        const related = [];
        for (const label of entry.labels || []) {
          if (!label.message || label.primary) {
            continue;
          }
          related.push(new vscode.DiagnosticRelatedInformation(
            new vscode.Location(document.uri, diagnosticRange(document, label)),
            label.message
          ));
        }
        if (related.length > 0) {
          diag.relatedInformation = related;
        }
        items.push(diag);
      }
      this.diagnostics.set(document.uri, items);
    } catch (error) {
      this.diagnostics.set(document.uri, [
        new vscode.Diagnostic(
          new vscode.Range(0, 0, 0, 1),
          String(error.message || error),
          vscode.DiagnosticSeverity.Error
        )
      ]);
    }
  }

  scheduleIndex(document, delay = 300) {
    if (!isStashaDocument(document)) {
      return;
    }
    const key = document.uri.toString();
    clearTimeout(this.pendingIndex.get(key));
    this.pendingIndex.set(key, setTimeout(() => {
      this.pendingIndex.delete(key);
      this.refreshSymbols(document);
    }, delay));
  }

  async refreshSymbols(document) {
    try {
      const payload = await runJsonCommand("symbols", document);
      this.index.setSymbols(document.uri.fsPath, payload.symbols || []);
      return payload.symbols || [];
    } catch (error) {
      return [];
    }
  }

  async indexWorkspace() {
    this.index.clear();
    const maxFiles = vscode.workspace.getConfiguration("stasha").get("maxWorkspaceFiles", 2000);
    const files = await vscode.workspace.findFiles("**/*.sts", "**/extlib/**", maxFiles);
    for (const file of files) {
      try {
        const bytes = await vscode.workspace.fs.readFile(file);
        const text = Buffer.from(bytes).toString("utf8");
        const docLike = {
          uri: file,
          languageId: LANGUAGE_ID,
          getText() {
            return text;
          }
        };
        const payload = await runJsonCommand("symbols", docLike);
        this.index.setSymbols(file.fsPath, payload.symbols || []);
      } catch (_error) {
      }
    }
  }

  async provideDocumentSymbols(document) {
    const symbols = await this.refreshSymbols(document);
    const groups = new Map();
    const topLevel = [];

    for (const entry of symbols) {
      const selection = new vscode.Range(entry.line, entry.col, entry.line, entry.col + Math.max(1, entry.name.length));
      const symbol = new vscode.DocumentSymbol(
        entry.name,
        entry.detail || "",
        this.symbolKind(entry.kind),
        selection,
        selection
      );
      if (entry.container) {
        const bucket = groups.get(entry.container) || [];
        bucket.push(symbol);
        groups.set(entry.container, bucket);
      } else {
        topLevel.push(symbol);
      }
    }

    for (const symbol of topLevel) {
      const children = groups.get(symbol.name);
      if (children) {
        symbol.children.push(...children);
      }
    }

    return topLevel;
  }

  symbolKind(kind) {
    switch (kind) {
      case "function":
        return vscode.SymbolKind.Function;
      case "method":
        return vscode.SymbolKind.Method;
      case "struct":
        return vscode.SymbolKind.Struct;
      case "interface":
        return vscode.SymbolKind.Interface;
      case "enum":
        return vscode.SymbolKind.Enum;
      case "enumMember":
        return vscode.SymbolKind.EnumMember;
      case "type":
        return vscode.SymbolKind.TypeParameter;
      case "variable":
        return vscode.SymbolKind.Variable;
      default:
        return vscode.SymbolKind.Object;
    }
  }

  async provideDefinition(document, position) {
    try {
      const payload = await runDefinitionCommand(document, position);
      const symbol = payload.definition;
      if (!symbol) {
        return null;
      }
      const uri = vscode.Uri.file(symbol.path);
      const target = new vscode.Position(symbol.line, symbol.col);
      return new vscode.Location(uri, new vscode.Range(target, target.translate(0, Math.max(1, symbol.name.length || 1))));
    } catch (_error) {
      const range = document.getWordRangeAtPosition(position);
      if (!range) {
        return null;
      }
      const name = document.getText(range);
      const matches = this.index.getByName(name);
      if (matches.length === 0) {
        return null;
      }
      return matches.map(symbol => new vscode.Location(
        vscode.Uri.file(symbol.path),
        new vscode.Position(symbol.line, symbol.col)
      ));
    }
  }

  async provideWorkspaceSymbols(query) {
    if (!query) {
      return [];
    }

    const out = [];
    for (const symbols of this.index.byFile.values()) {
      for (const symbol of symbols) {
        if (!symbol.name.toLowerCase().includes(query.toLowerCase())) {
          continue;
        }
        out.push(new vscode.SymbolInformation(
          symbol.name,
          this.symbolKind(symbol.kind),
          symbol.container || "",
          new vscode.Location(
            vscode.Uri.file(symbol.path),
            new vscode.Position(symbol.line, symbol.col)
          )
        ));
      }
    }
    return out;
  }

  async provideHover(document, position) {
    const range = document.getWordRangeAtPosition(position);
    if (!range) {
      return null;
    }

    const name = document.getText(range);
    const localSymbols = await this.refreshSymbols(document);
    const symbol = localSymbols.find(entry => entry.name === name)
      || this.index.getByName(name)[0];
    if (!symbol) {
      return null;
    }

    const lines = [`**${symbol.name}**`, `Kind: \`${symbol.kind}\``];
    if (symbol.container) {
      lines.push(`Container: \`${symbol.container}\``);
    }
    return new vscode.Hover(new vscode.MarkdownString(lines.join("\n\n")));
  }
}

function semanticTypeFor(token, symbolIndex, documentPath) {
  switch (token.kind) {
    case "keyword":
      return "keyword";
    case "string":
      return "string";
    case "number":
      return "number";
    case "type":
      return "type";
    case "operator":
      return "operator";
    default:
      break;
  }

  const fileSymbols = symbolIndex.getFileSymbols(documentPath);
  const symbol = fileSymbols.find(entry => entry.line === token.line && entry.col === token.col);
  if (!symbol) {
    return "variable";
  }

  switch (symbol.kind) {
    case "function":
      return "function";
    case "method":
      return "method";
    case "enumMember":
      return "enumMember";
    case "struct":
    case "interface":
    case "enum":
    case "type":
      return "type";
    case "variable":
      return symbol.container ? "property" : "variable";
    default:
      return "variable";
  }
}

async function activate(context) {
  const service = new StashaLanguageService(context);

  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(doc => {
    if (isStashaDocument(doc)) {
      service.scheduleDiagnostics(doc, 0);
      service.scheduleIndex(doc, 0);
    }
  }));

  context.subscriptions.push(vscode.workspace.onDidChangeTextDocument(event => {
    if (isStashaDocument(event.document)) {
      service.scheduleDiagnostics(event.document);
      service.scheduleIndex(event.document);
    }
  }));

  context.subscriptions.push(vscode.workspace.onDidSaveTextDocument(doc => {
    if (isStashaDocument(doc)) {
      service.scheduleDiagnostics(doc, 0);
      service.scheduleIndex(doc, 0);
    }
  }));

  context.subscriptions.push(vscode.workspace.onDidCloseTextDocument(doc => {
    if (isStashaDocument(doc)) {
      service.diagnostics.delete(doc.uri);
      service.index.remove(doc.uri.fsPath);
    }
  }));

  context.subscriptions.push(vscode.languages.registerDocumentSemanticTokensProvider(
    { language: LANGUAGE_ID },
    {
      async provideDocumentSemanticTokens(document) {
        const payload = await runJsonCommand("tokens", document);
        const builder = new vscode.SemanticTokensBuilder(TOKEN_LEGEND);
        await service.refreshSymbols(document);
        for (const token of payload.tokens || []) {
          const tokenType = semanticTypeFor(token, service.index, document.uri.fsPath);
          const typeIndex = TOKEN_TYPES.indexOf(tokenType);
          if (typeIndex === -1) {
            continue;
          }
          builder.push(token.line, token.col, token.len, typeIndex, 0);
        }
        return builder.build();
      }
    },
    TOKEN_LEGEND
  ));

  context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider(
    { language: LANGUAGE_ID },
    {
      provideDocumentSymbols(document) {
        return service.provideDocumentSymbols(document);
      }
    }
  ));

  context.subscriptions.push(vscode.languages.registerDefinitionProvider(
    { language: LANGUAGE_ID },
    {
      provideDefinition(document, position) {
        return service.provideDefinition(document, position);
      }
    }
  ));

  context.subscriptions.push(vscode.languages.registerWorkspaceSymbolProvider({
    provideWorkspaceSymbols(query) {
      return service.provideWorkspaceSymbols(query);
    }
  }));

  context.subscriptions.push(vscode.languages.registerHoverProvider(
    { language: LANGUAGE_ID },
    {
      provideHover(document, position) {
        return service.provideHover(document, position);
      }
    }
  ));

  context.subscriptions.push(vscode.commands.registerCommand("stasha.restartLanguageServices", async () => {
    service.diagnostics.clear();
    await service.indexWorkspace();
    for (const document of vscode.workspace.textDocuments) {
      if (isStashaDocument(document)) {
        service.scheduleDiagnostics(document, 0);
        service.scheduleIndex(document, 0);
      }
    }
    vscode.window.showInformationMessage("Stasha language services restarted.");
  }));

  context.subscriptions.push(vscode.commands.registerCommand("stasha.reindexWorkspace", async () => {
    await service.indexWorkspace();
    vscode.window.showInformationMessage("Stasha workspace index refreshed.");
  }));

  await service.indexWorkspace();
  for (const document of vscode.workspace.textDocuments) {
    if (isStashaDocument(document)) {
      service.scheduleDiagnostics(document, 0);
      service.scheduleIndex(document, 0);
    }
  }
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
