import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { basename, join, posix } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const destination = join(root, 'store', 'catalog.json');
const refIndex = process.argv.indexOf('--ref');
const contentRef = refIndex >= 0 ? process.argv[refIndex + 1] : 'main';
if (!contentRef || !/^[a-zA-Z0-9][a-zA-Z0-9._-]*$/.test(contentRef)) throw new Error('Invalid content ref.');
const raw = `https://raw.githubusercontent.com/catui-plataform/CATui/${contentRef}/store`;
const coreOrder = ['cemu', 'citra', 'dolphin', 'eden', 'flycast', 'genesis_plus_gx', 'melonds',
  'mesen', 'mgba', 'mupen64plus_next', 'pcsx2', 'ppsspp', 'rpcs3', 'snes9x', 'swanstation', 'shadps4'];
const raConsoles = { genesis_plus_gx: [1, 11, 15], melonds: [18], mesen: [7], mgba: [4, 5, 6],
  mupen64plus_next: [2], pcsx2: [21], ppsspp: [41], flycast: [40], snes9x: [3], swanstation: [12] };
const directories = path => readdirSync(path, { withFileTypes: true }).filter(e => e.isDirectory()).map(e => e.name);
const relativeFile = name => typeof name === 'string' && name.length > 0 && !name.includes('\\') &&
  !name.includes(':') && !name.startsWith('/') && name.split('/').every(p => p && p !== '.' && p !== '..');
const items = [];
const seen = new Set();
const store = join(root, 'store');
const platforms = directories(store).filter(p => ['windows', 'linux', 'android', 'macos'].includes(p)).sort();
for (const platform of platforms) {
  for (const architecture of directories(join(store, platform)).sort()) {
    if (!['x86_64', 'x86_32', 'arm64', 'arm32'].includes(architecture)) throw new Error(`Invalid architecture: ${architecture}`);
    for (const category of directories(join(store, platform, architecture)).sort()) {
      if (!['cores', 'seamless'].includes(category)) throw new Error(`Invalid runtime category: ${category}`);
      const group = join(store, platform, architecture, category);
      const folders = directories(group).sort((a, b) => {
        const ai = coreOrder.indexOf(a), bi = coreOrder.indexOf(b);
        return (ai < 0 ? 999 : ai) - (bi < 0 ? 999 : bi) || a.localeCompare(b);
      });
      for (const folder of folders) {
        const directory = join(group, folder);
        const manifest = JSON.parse(readFileSync(join(directory, 'manifest.json'), 'utf8'));
        const id = manifest.id || (category === 'seamless' ? `${folder}_standalone` : folder);
        if (!/^[a-z0-9_]+$/.test(id)) throw new Error(`Invalid package ID: ${id}`);
        const key = `${platform}/${architecture}/${category}/${id}`;
        if (seen.has(key)) throw new Error(`Duplicate package: ${key}`);
        seen.add(key);
        const filename = manifest.filename || `${folder}_libretro.dll`;
        const icon = manifest.icon || 'icon.png';
        const entry = manifest.entryFilename || filename;
        if (![filename, icon].every(n => basename(n) === n && relativeFile(n)) || !relativeFile(entry)) {
          throw new Error(`Invalid package path: ${key}`);
        }
        const bytes = readFileSync(join(directory, filename));
        if (bytes.length >= 100 * 1024 * 1024) throw new Error(`Package exceeds GitHub's file limit: ${key}`);
        const packageType = manifest.packageType || 'dll';
        if (packageType === 'so') {
          const machines = { x86_64: 62, x86_32: 3, arm64: 183, arm32: 40 };
          const elfClass = ['x86_64', 'arm64'].includes(architecture) ? 2 : 1;
          if (platform !== 'linux' || !filename.endsWith('.so') || entry !== filename ||
              bytes.length < 64 || bytes.subarray(0, 4).toString('hex') !== '7f454c46' ||
              bytes[4] !== elfClass || bytes[5] !== 1 || bytes.readUInt16LE(16) !== 3 ||
              bytes.readUInt16LE(18) !== machines[architecture]) {
            throw new Error(`Invalid Linux shared library or architecture: ${key}`);
          }
        } else {
          const magic = packageType === 'archive' ? '504b0304' : '4d5a';
          if (bytes.subarray(0, magic.length / 2).toString('hex') !== magic) throw new Error(`Invalid package binary: ${key}`);
        }
        if (category === 'seamless' && (!entry.toLowerCase().endsWith('.exe') || packageType !== 'archive')) {
          throw new Error(`Seamless packages must specify the exact archived EXE: ${key}`);
        }
        readFileSync(join(directory, icon));
        const sha256 = createHash('sha256').update(bytes).digest('hex');
        const baseUrl = `${raw}/${platform}/${architecture}/${category}/${folder}`;
        const consoles = raConsoles[folder] || [];
        items.push({
          ...manifest, id, coreId: id, filename, category,
          runtime: category === 'seamless' ? 'seamless' : 'libretro',
          version: manifest.version || sha256.slice(0, 12), packageVersion: manifest.version || '',
          buildVersion: sha256.slice(0, 12), size: `${(bytes.length / (1024 * 1024)).toFixed(1)} MB`,
          sizeBytes: bytes.length, sha256, downloadUrl: `${baseUrl}/${filename}`, packageType,
          entryFilename: posix.normalize(entry), repository: manifest.repository || '',
          platforms: [platform], architectures: [architecture], platform, architecture,
          iconFile: icon, icon: 'res://assets/images/catui/catui_placeholder.png',
          iconUrl: `${baseUrl}/${icon}`, verified: true,
          retroachievements: manifest.retroachievements || { supported: category === 'cores' && consoles.length > 0,
            mode: category === 'cores' && consoles.length > 0 ? 'frontend' : 'unavailable', consoleIds: consoles },
        });
      }
    }
  }
}
const output = JSON.stringify({ ok: true, schemaVersion: 2, contentRef, items }, null, 2) + '\n';
if (process.argv.includes('--check')) {
  if (readFileSync(destination, 'utf8') !== output) throw new Error('Catalog is stale. Run node store/build-catalog.mjs.');
  console.log(`Verified ${items.length} packages, targets, hashes and download URLs.`);
} else {
  writeFileSync(destination, output);
  console.log(`Built catalog for ${items.length} packages at ${contentRef}.`);
}
