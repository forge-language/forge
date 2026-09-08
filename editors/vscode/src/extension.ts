import * as fs from 'node:fs';
import * as path from 'node:path';
import { workspace, ExtensionContext, WorkspaceConfiguration, window } from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

function forgeConfig(): WorkspaceConfiguration {
  return workspace.getConfiguration('forge');
}

function findOnPath(binName: string): string | undefined {
  const pathEnv = process.env.PATH || '';
  const exts = process.platform === 'win32' ? ['.exe', '.cmd', ''] : [''];
  for (const dir of pathEnv.split(path.delimiter)) {
    for (const ext of exts) {
      const candidate = path.join(dir, binName + ext);
      if (fs.existsSync(candidate)) return candidate;
    }
  }
  return undefined;
}

function resolveServerBinary(): string | undefined {
  const cfg = forgeConfig();
  const configured = cfg.get<string>('lspPath');
  if (configured && fs.existsSync(configured)) return configured;

  for (const folder of workspace.workspaceFolders ?? []) {
    const candidate = path.join(folder.uri.fsPath, 'build', 'bin', 'forge-lsp');
    if (fs.existsSync(candidate)) return candidate;
  }

  return findOnPath('forge-lsp');
}

function buildInitializationOptions() {
  const cfg = forgeConfig();
  const workspaceRoot = workspace.workspaceFolders?.[0]?.uri.fsPath;
  return {
    workspaceRoot,
    forge: {
      path: cfg.get<string>('path') || undefined,
      forgeRoot: cfg.get<string>('forgeRoot') || undefined,
      libDir: cfg.get<string>('libDir') || undefined,
      includePaths: cfg.get<string[]>('includePaths') || [],
    },
  };
}

export function activate(context: ExtensionContext): void {
  const serverBinary = resolveServerBinary();
  if (!serverBinary) {
    void window.showErrorMessage(
      "Forge: couldn't find the forge-lsp binary. Build it (cmake --build build --target forge-lsp) " +
        "or set the forge.lspPath setting.",
    );
    return;
  }

  const serverOptions: ServerOptions = {
    run: { command: serverBinary, args: [] },
    debug: { command: serverBinary, args: [] },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'forge' }],
    synchronize: {
      configurationSection: 'forge',
    },
    initializationOptions: buildInitializationOptions(),
  };

  client = new LanguageClient('forgeLanguageServer', 'Forge Language Server', serverOptions, clientOptions);
  context.subscriptions.push(
    workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('forge') && client?.isRunning()) {
        void client.sendNotification('workspace/didChangeConfiguration', {
          settings: { forge: buildInitializationOptions().forge },
        });
      }
    }),
  );
  void client.start();
}

export function deactivate(): Promise<void> | undefined {
  if (!client) return undefined;
  return client.stop();
}
