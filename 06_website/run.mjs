import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

// Keep the editable configuration here and every generated file under temp/.
const configDir = path.dirname(fileURLToPath(import.meta.url));
const projectDir = path.dirname(configDir);
const runtimeDir = path.join(projectDir, 'temp', 'docusaurus');
const [command = 'start', ...extraArgs] = process.argv.slice(2);
const commands = new Set(['setup', 'start', 'build', 'serve']);

if (Number(process.versions.node.split('.')[0]) < 20) {
  throw new Error('文档站需要 Node.js 20 或更新版本。');
}
if (!commands.has(command)) {
  throw new Error(`未知命令：${command}；可用命令：setup、start、build、serve。`);
}

mkdirSync(runtimeDir, {recursive: true});
mkdirSync(path.join(runtimeDir, 'tmp'), {recursive: true});
for (const name of ['package.json', 'package-lock.json', 'docusaurus.config.js', 'sidebars.js', 'custom.css']) {
  copyFileSync(path.join(configDir, name), path.join(runtimeDir, name));
}

const env = {
  ...process.env,
  OPTO_PROJECT_ROOT: projectDir,
  npm_config_cache: path.join(runtimeDir, 'npm-cache'),
  DOCUSAURUS_TELEMETRY_DISABLED: 'true',
  NO_UPDATE_NOTIFIER: '1',
  TEMP: path.join(runtimeDir, 'tmp'),
  TMP: path.join(runtimeDir, 'tmp'),
  TMPDIR: path.join(runtimeDir, 'tmp'),
};
const lockHash = createHash('sha256')
  .update(readFileSync(path.join(runtimeDir, 'package-lock.json')))
  .update(process.versions.node.split('.')[0])
  .digest('hex');
const stampPath = path.join(runtimeDir, '.installed-lock.sha256');
const cli = path.join(runtimeDir, 'node_modules', '@docusaurus', 'core', 'bin', 'docusaurus.mjs');

function execute(args) {
  const result = spawnSync(process.execPath, args, {cwd: runtimeDir, env, stdio: 'inherit'});
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}

if (!existsSync(cli) || !existsSync(stampPath) || readFileSync(stampPath, 'utf8') !== lockHash) {
  if (!process.env.npm_execpath) throw new Error('请通过 npm run setup/start/build/serve 运行。');
  execute([process.env.npm_execpath, 'ci', '--no-audit', '--no-fund']);
  writeFileSync(stampPath, lockHash);
}

if (command === 'setup') {
  console.log('文档站依赖已安装：temp/docusaurus/node_modules');
} else {
  const args = [cli, command, runtimeDir];
  if (command === 'build') args.push('--out-dir', path.join(runtimeDir, 'build'));
  if (command === 'serve') args.push('--dir', path.join(runtimeDir, 'build'), '--no-open');
  if (command === 'start') args.push('--no-open');
  execute([...args, ...extraArgs]);
}
