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
exports.StashaProjectProvider = void 0;
exports.openProjectWebview = openProjectWebview;
const vscode = __importStar(require("vscode"));
const fs = __importStar(require("fs"));
const path = __importStar(require("path"));
function parseProj(text) {
    const proj = { ext_libs: [] };
    for (const raw of text.split('\n')) {
        const line = raw.replace(/#.*$/, '').trim();
        if (!line)
            continue;
        const eq = line.indexOf('=');
        if (eq === -1)
            continue;
        const key = line.slice(0, eq).trim();
        const val = line.slice(eq + 1).trim();
        if (key === 'main')
            proj.main = val.replace(/^"|"$/g, '');
        if (key === 'binary')
            proj.binary = val.replace(/^"|"$/g, '');
        if (key === 'library')
            proj.library = val.replace(/^"|"$/g, '');
        if (key === 'ext_libs') {
            const inner = val.replace(/^\[/, '').replace(/\]$/, '').trim();
            if (!inner) {
                proj.ext_libs = [];
                continue;
            }
            for (const entry of inner.split(',')) {
                const e = entry.trim();
                if (!e)
                    continue;
                const pair = e.match(/^\("([^"]+)"\s*:\s*"([^"]+)"\)$/);
                if (pair) {
                    proj.ext_libs.push([pair[1], pair[2]]);
                }
                else {
                    proj.ext_libs.push(e.replace(/^"|"$/g, ''));
                }
            }
        }
    }
    return proj;
}
function writeProj(proj) {
    const lines = [];
    if (proj.main)
        lines.push(`main     = "${proj.main}"`);
    if (proj.binary)
        lines.push(`binary   = "${proj.binary}"`);
    if (proj.library)
        lines.push(`library  = "${proj.library}"`);
    const libEntries = proj.ext_libs.map(e => Array.isArray(e) ? `("${e[0]}" : "${e[1]}")` : `"${e}"`);
    lines.push(`ext_libs = [${libEntries.join(', ')}]`);
    return lines.join('\n') + '\n';
}
function getProjPath() {
    const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
    return ws ? path.join(ws, 'sts.sproj') : undefined;
}
function loadProject() {
    const p = getProjPath();
    if (!p || !fs.existsSync(p))
        return undefined;
    try {
        return parseProj(fs.readFileSync(p, 'utf8'));
    }
    catch {
        return undefined;
    }
}
function saveProject(proj) {
    const p = getProjPath();
    if (!p)
        return;
    fs.writeFileSync(p, writeProj(proj), 'utf8');
}
class ProjNode extends vscode.TreeItem {
    constructor(nodeKind, label, value = '', libIndex = -1, collapsible = vscode.TreeItemCollapsibleState.None) {
        super(label, collapsible);
        this.nodeKind = nodeKind;
        this.value = value;
        this.libIndex = libIndex;
        this._applyAppearance();
    }
    _applyAppearance() {
        switch (this.nodeKind) {
            case 'root':
                this.iconPath = new vscode.ThemeIcon('project');
                this.collapsibleState = vscode.TreeItemCollapsibleState.Expanded;
                break;
            case 'main':
                this.iconPath = new vscode.ThemeIcon('file-code');
                this.description = this.value;
                this.tooltip = `Entry point: ${this.value}`;
                this.command = this.value
                    ? { command: 'stasha.openProjectFile', title: 'Open', arguments: [this.value] }
                    : undefined;
                this.contextValue = 'stashaMain';
                break;
            case 'output':
                this.iconPath = new vscode.ThemeIcon('package');
                this.description = this.value;
                this.tooltip = `Output: ${this.value}`;
                this.contextValue = 'stashaOutput';
                break;
            case 'ext_libs_header':
                this.iconPath = new vscode.ThemeIcon('library');
                this.collapsibleState = vscode.TreeItemCollapsibleState.Expanded;
                this.contextValue = 'stashaExtLibsHeader';
                break;
            case 'ext_lib':
                this.iconPath = new vscode.ThemeIcon('archive');
                this.description = this.value;
                this.contextValue = 'stashaExtLib';
                break;
            case 'actions_header':
                this.collapsibleState = vscode.TreeItemCollapsibleState.Expanded;
                this.iconPath = new vscode.ThemeIcon('play-circle');
                break;
            case 'action':
                this.command = { command: this.value, title: this.label, arguments: [] };
                this.iconPath = new vscode.ThemeIcon(this.value.includes('build') ? 'tools' :
                    this.value.includes('test') ? 'beaker' :
                        this.value.includes('run') ? 'play' : 'gear');
                break;
        }
    }
}
// ─── TreeDataProvider ─────────────────────────────────────────────────────────
class StashaProjectProvider {
    constructor() {
        this._onDidChangeTreeData = new vscode.EventEmitter();
        this.onDidChangeTreeData = this._onDidChangeTreeData.event;
    }
    refresh() { this._onDidChangeTreeData.fire(undefined); }
    getTreeItem(el) { return el; }
    getChildren(el) {
        if (!el) {
            const proj = loadProject();
            if (!proj) {
                const noProj = new ProjNode('action', 'No sts.sproj found', 'stasha.initProject');
                noProj.iconPath = new vscode.ThemeIcon('warning');
                noProj.command = { command: 'stasha.initProject', title: 'Initialize project', arguments: [] };
                return [noProj];
            }
            const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? '';
            const name = path.basename(ws) || 'Stasha Project';
            return [new ProjNode('root', name, '', -1, vscode.TreeItemCollapsibleState.Expanded)];
        }
        if (el.nodeKind === 'root') {
            const proj = loadProject();
            const nodes = [];
            nodes.push(new ProjNode('main', 'main', proj.main ?? '(not set)'));
            const out = proj.binary ?? proj.library ?? '(not set)';
            nodes.push(new ProjNode('output', proj.binary ? 'binary' : 'library', out));
            nodes.push(new ProjNode('ext_libs_header', `ext_libs (${proj.ext_libs.length})`, '', -1, vscode.TreeItemCollapsibleState.Expanded));
            nodes.push(new ProjNode('actions_header', 'Actions', '', -1, vscode.TreeItemCollapsibleState.Expanded));
            return nodes;
        }
        if (el.nodeKind === 'ext_libs_header') {
            const proj = loadProject();
            return proj.ext_libs.map((lib, i) => {
                const display = Array.isArray(lib) ? lib[0] : lib;
                const desc = Array.isArray(lib) ? lib[1] : '';
                return new ProjNode('ext_lib', display, desc, i);
            });
        }
        if (el.nodeKind === 'actions_header') {
            return [
                new ProjNode('action', '$(tools) Build', 'stasha.build'),
                new ProjNode('action', '$(play) Run', 'stasha.run'),
                new ProjNode('action', '$(beaker) Test', 'stasha.test'),
                new ProjNode('action', '$(gear) Edit sts.sproj', 'stasha.openProjectPanel'),
            ];
        }
        return [];
    }
    dispose() { this._onDidChangeTreeData.dispose(); }
}
exports.StashaProjectProvider = StashaProjectProvider;
// ─── Webview panel ────────────────────────────────────────────────────────────
function openProjectWebview(ctx, provider) {
    const panel = vscode.window.createWebviewPanel('stashaProject', 'Stasha Project', vscode.ViewColumn.One, { enableScripts: true });
    const proj = loadProject() ?? { main: '', binary: '', ext_libs: [] };
    panel.webview.html = buildWebviewHtml(proj);
    panel.webview.onDidReceiveMessage(msg => {
        switch (msg.type) {
            case 'save': {
                const updated = {
                    main: msg.main || undefined,
                    binary: msg.binary || undefined,
                    library: msg.library || undefined,
                    ext_libs: msg.ext_libs
                        .filter((s) => s.trim())
                        .map((s) => s.trim()),
                };
                saveProject(updated);
                provider.refresh();
                vscode.window.showInformationMessage('sts.sproj saved.');
                break;
            }
            case 'pickMain': {
                vscode.window.showOpenDialog({
                    canSelectFiles: true,
                    canSelectMany: false,
                    filters: { 'Stasha': ['sts'] },
                    defaultUri: vscode.workspace.workspaceFolders?.[0]?.uri,
                }).then(uris => {
                    if (!uris || uris.length === 0)
                        return;
                    const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? '';
                    const rel = path.relative(ws, uris[0].fsPath).replace(/\\/g, '/');
                    panel.webview.postMessage({ type: 'setMain', value: rel });
                });
                break;
            }
        }
    }, undefined, ctx.subscriptions);
}
function buildWebviewHtml(proj) {
    const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
    const mainVal = esc(proj.main ?? '');
    const binaryVal = esc(proj.binary ?? '');
    const libraryVal = esc(proj.library ?? '');
    const libRows = proj.ext_libs.map((lib, i) => {
        const val = Array.isArray(lib) ? lib[0] : lib;
        return `<div class="lib-row" id="lib-${i}">
            <input type="text" class="lib-input" value="${esc(val)}" placeholder="path/to/lib.a" />
            <button onclick="removeLib(${i})">✕</button>
        </div>`;
    }).join('\n');
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stasha Project</title>
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground);
         background: var(--vscode-editor-background); padding: 20px; }
  h2 { color: var(--vscode-titleBar-activeForeground); margin-top: 0; }
  label { display: block; margin-top: 14px; font-weight: 600; font-size: 0.85em;
          text-transform: uppercase; letter-spacing: 0.05em; opacity: 0.7; }
  .row { display: flex; gap: 6px; align-items: center; margin-top: 6px; }
  input[type=text] { flex: 1; background: var(--vscode-input-background);
                     color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border);
                     padding: 6px 8px; font-size: 0.95em; border-radius: 3px; }
  input[type=text]:focus { outline: none; border-color: var(--vscode-focusBorder); }
  button { background: var(--vscode-button-background); color: var(--vscode-button-foreground);
           border: none; padding: 6px 12px; cursor: pointer; border-radius: 3px; font-size: 0.9em; }
  button:hover { background: var(--vscode-button-hoverBackground); }
  button.secondary { background: var(--vscode-button-secondaryBackground);
                     color: var(--vscode-button-secondaryForeground); }
  button.secondary:hover { background: var(--vscode-button-secondaryHoverBackground); }
  #lib-list { margin-top: 8px; }
  .lib-row { display: flex; gap: 6px; align-items: center; margin-bottom: 6px; }
  .lib-row button { padding: 4px 8px; }
  .actions { margin-top: 24px; display: flex; gap: 10px; }
  .section { margin-top: 20px; padding: 16px; background: var(--vscode-sideBar-background);
             border-radius: 6px; }
  .section-title { font-weight: bold; margin-bottom: 12px; }
</style>
</head>
<body>
<h2>$(tools) Stasha Project Settings</h2>

<div class="section">
  <div class="section-title">Entry Point</div>
  <label>Main file (.sts)</label>
  <div class="row">
    <input type="text" id="main" value="${mainVal}" placeholder="src/main.sts" />
    <button class="secondary" onclick="pickMain()">Browse…</button>
  </div>
</div>

<div class="section">
  <div class="section-title">Output</div>
  <label>Binary output path</label>
  <input type="text" id="binary" value="${binaryVal}" placeholder="bin/myapp" style="margin-top:6px;width:100%;box-sizing:border-box;" />
  <label>Library output path (leave blank for executable)</label>
  <input type="text" id="library" value="${libraryVal}" placeholder="bin/libmyapp.a" style="margin-top:6px;width:100%;box-sizing:border-box;" />
</div>

<div class="section">
  <div class="section-title">External Libraries</div>
  <div id="lib-list">${libRows}</div>
  <button class="secondary" onclick="addLib()" style="margin-top:8px;">+ Add Library</button>
</div>

<div class="actions">
  <button onclick="save()">Save sts.sproj</button>
  <button class="secondary" onclick="cancel()">Cancel</button>
</div>

<script>
  const vscode = acquireVsCodeApi();
  let libCount = ${proj.ext_libs.length};

  function addLib() {
    const list = document.getElementById('lib-list');
    const div = document.createElement('div');
    div.className = 'lib-row';
    div.id = 'lib-' + libCount;
    div.innerHTML = '<input type="text" class="lib-input" placeholder="path/to/lib.a" />'
                  + '<button onclick="removeLib(' + libCount + ')">✕</button>';
    list.appendChild(div);
    libCount++;
  }

  function removeLib(i) {
    const el = document.getElementById('lib-' + i);
    if (el) el.remove();
  }

  function save() {
    const libs = Array.from(document.querySelectorAll('.lib-input'))
                      .map(el => el.value.trim())
                      .filter(v => v);
    vscode.postMessage({
      type: 'save',
      main: document.getElementById('main').value.trim(),
      binary: document.getElementById('binary').value.trim(),
      library: document.getElementById('library').value.trim(),
      ext_libs: libs,
    });
  }

  function cancel() { vscode.postMessage({ type: 'cancel' }); }
  function pickMain() { vscode.postMessage({ type: 'pickMain' }); }

  window.addEventListener('message', e => {
    const msg = e.data;
    if (msg.type === 'setMain') {
      document.getElementById('main').value = msg.value;
    }
  });
</script>
</body>
</html>`;
}
//# sourceMappingURL=projectPanel.js.map