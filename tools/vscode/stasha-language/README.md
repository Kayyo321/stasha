# Stasha Language Tools

VS Code support for `.sts` files backed by the local `stasha` compiler.

Features:

- Live diagnostics from `stasha check`
- Semantic tokens from `stasha tokens`
- Document and workspace symbols from `stasha symbols`
- Go to definition from `stasha definition`
- Hover backed by the workspace symbol index
- TextMate grammar and language configuration for immediate syntax support

By default the extension auto-detects `bin/stasha` inside the workspace. You can override that with the `stasha.compilerPath` setting.

## Development Host

From the repo root:

```bash
'/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code' \
  --extensionDevelopmentPath tools/vscode/stasha-language \
  .
```

## What To Expect

- Open any `.sts` file and the language mode should switch to `Stasha`.
- Diagnostics appear automatically while typing and on save.
- `Go to Definition` uses the compiler-backed definition resolver.
- `Document Symbols` and `Workspace Symbols` are populated from the compiler symbol export.
- `Stasha: Restart Language Services` reloads the compiler-backed services.
