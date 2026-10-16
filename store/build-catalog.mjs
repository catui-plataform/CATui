import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const destination = join(root, 'store', 'catalog.json');
const refIndex = process.argv.indexOf('--ref');
const contentRef = refIndex >= 0 ? process.argv[refIndex + 1] : 'main';
if (!contentRef || !/^[a-zA-Z0-9][a-zA-Z0-9._-]*$/.test(contentRef)) {
  throw new Error('Provide a valid branch or commit with --ref.');
}
const raw = `https://raw.githubusercontent.com/catui-plataform/CATui/${contentRef}/store/cores`;
// Preserve the published order. Add new packages to this list explicitly.
const ids = [
  'cemu', 'citra', 'dolphin', 'eden', 'flycast', 'genesis_plus_gx',
  'melonds', 'mesen', 'mgba', 'mupen64plus_next', 'pcsx2', 'ppsspp',
  'rpcs3', 'snes9x', 'swanstation', 'shadps4',
];
const items = ids.map(id => {
  const directory = join(root, 'store', 'cores', id);
  const manifest = JSON.parse(readFileSync(join(directory, 'manifest.json'), 'utf8'));
  const filename = manifest.filename || `${id}_libretro.dll`;
  const icon = manifest.icon || 'icon.png';
  for (const name of [filename, icon]) {
    if (basename(name) !== name || name === '.' || name === '..') throw new Error(`Invalid filename: ${id}/${name}`);
  }
  const bytes = readFileSync(join(directory, filename));
  if (bytes.length >= 100 * 1024 * 1024) throw new Error(`Package exceeds GitHub's Git file limit: ${id}`);
  const packageType = manifest.packageType || 'dll';
  const magic = packageType === 'archive' ? '504b0304' : '4d5a';
  if (bytes.subarray(0, magic.length / 2).toString('hex') !== magic) throw new Error(`Invalid package: ${id}`);
  readFileSync(join(directory, icon));
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  return {
    ...manifest,
    id,
    coreId: id,
    filename,
    category: 'cores',
    version: manifest.version || sha256.slice(0, 12),
    packageVersion: manifest.version || '',
    buildVersion: sha256.slice(0, 12),
    size: `${(bytes.length / (1024 * 1024)).toFixed(1)} MB`,
    sizeBytes: bytes.length,
    sha256,
    downloadUrl: `${raw}/${id}/${filename}`,
    packageType,
    entryFilename: manifest.entryFilename || filename,
    repository: manifest.repository || '',
    platforms: ['windows'],
    iconFile: icon,
    icon: 'res://assets/images/catui/catui_placeholder.png',
    iconUrl: `${raw}/${id}/${icon}`,
    verified: true,
  };
});
const output = JSON.stringify({ ok: true, schemaVersion: 1, contentRef, items }, null, 2) + '\n';
if (process.argv.includes('--check')) {
  if (readFileSync(destination, 'utf8') !== output) throw new Error('Catalog is stale. Run node store/build-catalog.mjs.');
  console.log(`Verified ${items.length} packages, hashes and download URLs.`);
} else {
  writeFileSync(destination, output);
  console.log(`Built catalog for ${items.length} packages at ${contentRef}.`);
}
