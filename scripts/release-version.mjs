#!/usr/bin/env node
/**
 * The next release's tag, from the tags already made (the same script in RhynoTech/CommaView and
 * RhynoTech/commaviewd; keep them the same).
 *
 * A version (0.0.159) is released as alphas, <prefix>0.0.159-alpha.1, -alpha.2, ..., for testers,
 * then once as the beta, <prefix>0.0.159-beta, built from the alpha chosen (the newest by
 * default). After its beta the next alpha is the next patch (0.0.160-alpha.1). The floor (the
 * repo's own version file) only moves that up, for a minor or major version.
 *
 *   node scripts/release-version.mjs --prefix app-v --floor 0.0.159 --channel alpha|beta \
 *     [--from <alpha tag>] [--tags <tag,...>] [--github-output]
 *
 * Without --tags it reads the origin's tags (git ls-remote). Prints tag, version, version_code
 * and, for a beta, from_tag, as key=value lines (also appended to $GITHUB_OUTPUT when asked).
 *
 * Android version codes grow with every build: ((major * 100 + minor) * 1000 + patch) * 100, plus
 * the alpha's number (1 to 98), or 99 for the beta.
 */
import { execFileSync } from 'node:child_process';
import { appendFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

const BETA = 99;

export function parseVersion(text) {
  const match = /^(\d+)\.(\d+)\.(\d+)$/.exec(text ?? '');
  if (!match) throw new Error(`Not a version (<major>.<minor>.<patch>): ${text}`);
  return match.slice(1, 4).map(Number);
}

const formatVersion = ([major, minor, patch]) => `${major}.${minor}.${patch}`;

export function versionCode([major, minor, patch], build) {
  if (major > 20 || minor > 99 || patch > 999 || build < 0 || build > BETA) {
    throw new Error(`No version code for ${formatVersion([major, minor, patch])} build ${build}`);
  }
  return ((major * 100 + minor) * 1000 + patch) * 100 + build;
}

/** The tags under the prefix, as { tag, version, stage: 'alpha' | 'beta', number }. */
function releases(prefix, tags) {
  const pattern = new RegExp(
    `^${prefix.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}(\\d+\\.\\d+\\.\\d+)-(alpha(?:\\.(\\d+))?|beta)$`
  );
  return tags.flatMap((tag) => {
    const match = pattern.exec(tag);
    if (!match) return [];
    const beta = match[2] === 'beta';
    // The old single tag per version (v0.0.56-alpha) counts as alpha 0.
    return [{ tag, version: match[1], stage: beta ? 'beta' : 'alpha', number: beta ? BETA : Number(match[3] ?? 0) }];
  });
}

export function nextRelease({ prefix, floor, channel, from, tags }) {
  if (channel !== 'alpha' && channel !== 'beta') throw new Error(`--channel is alpha or beta: ${channel}`);
  const all = releases(prefix, tags);
  let version = parseVersion(floor);
  // A version whose beta is out is done: move on to the next patch.
  while (all.some((r) => r.stage === 'beta' && r.version === formatVersion(version))) {
    version = [version[0], version[1], version[2] + 1];
  }
  const name = formatVersion(version);
  const alphas = all
    .filter((r) => r.stage === 'alpha' && r.version === name)
    .sort((a, b) => a.number - b.number);

  if (channel === 'alpha') {
    const number = (alphas.at(-1)?.number ?? 0) + 1;
    if (number >= BETA) throw new Error(`${name} has had ${number - 1} alphas; cut its beta`);
    return {
      tag: `${prefix}${name}-alpha.${number}`,
      version: `${name}-alpha.${number}`,
      version_code: versionCode(version, number),
    };
  }

  const alpha = from ? alphas.find((r) => r.tag === from) : alphas.at(-1);
  if (!alpha) {
    throw new Error(
      from ? `${from} isn't an alpha of ${name}, the version being released` : `${name} has no alpha to make the beta from`
    );
  }
  return {
    tag: `${prefix}${name}-beta`,
    version: `${name}-beta`,
    version_code: versionCode(version, BETA),
    from_tag: alpha.tag,
  };
}

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i += 1) {
    const key = argv[i];
    if (!key.startsWith('--')) throw new Error(`Unexpected argument: ${key}`);
    if (key === '--github-output') {
      out.githubOutput = true;
      continue;
    }
    const value = argv[i + 1];
    if (value === undefined || value.startsWith('--')) throw new Error(`Missing value for ${key}`);
    out[key.slice(2)] = value;
    i += 1;
  }
  return out;
}

function originTags() {
  return execFileSync('git', ['ls-remote', '--tags', '--refs', 'origin'], { encoding: 'utf8' })
    .split('\n')
    .map((line) => line.split('\trefs/tags/')[1])
    .filter(Boolean);
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.prefix) throw new Error('Give --prefix (app-v or v)');
  const tags = args.tags !== undefined ? args.tags.split(',').map((t) => t.trim()).filter(Boolean) : originTags();
  const next = nextRelease({ prefix: args.prefix, floor: args.floor, channel: args.channel, from: args.from, tags });
  const lines = Object.entries(next).map(([key, value]) => `${key}=${value}`).join('\n');
  console.log(lines);
  if (args.githubOutput) appendFileSync(process.env.GITHUB_OUTPUT, `${lines}\n`);
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
  try {
    main();
  } catch (err) {
    console.error(err.message);
    process.exit(1);
  }
}
