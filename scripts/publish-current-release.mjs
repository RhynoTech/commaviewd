#!/usr/bin/env node
/**
 * Promotes a runtime release: its tag becomes the current runtime in the account services'
 * /api/current-release (RhynoTech/commaview-web, docs/plans/app-api.md), which commaview.com serves
 * and installed apps and the comma's install.sh read. Refuses an older tag than the current one
 * unless --allow-runtime-downgrade.
 *
 *   node scripts/publish-current-release.mjs --runtime-tag v0.0.57-alpha \
 *     --publish-url https://my.commaview.com,https://my-staging.commaview.com \
 *     [--allow-runtime-downgrade] [--dry-run]
 *
 * The first address is the one checked for the current runtime (production). Needs
 * GOOGLE_ACCESS_TOKEN and RELEASE_ACCOUNT unless --dry-run: the release workflow signs in as the
 * release account keylessly (google-github-actions/auth) and passes its access token.
 */

function usage() {
  console.error(`Usage:
  node scripts/publish-current-release.mjs --runtime-tag v0.0.57-alpha --publish-url https://my.commaview.com[,...] [--allow-runtime-downgrade] [--dry-run]`);
}

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i += 1) {
    const key = argv[i];
    if (!key.startsWith('--')) throw new Error(`Unexpected positional argument: ${key}`);
    if (key === '--dry-run' || key === '--allow-runtime-downgrade') {
      out[key.slice(2)] = 'true';
      continue;
    }
    const value = argv[i + 1];
    if (!value || value.startsWith('--')) throw new Error(`Missing value for ${key}`);
    out[key.slice(2)] = value;
    i += 1;
  }
  return out;
}

function runtimeTagParts(tag) {
  const match = /^v(\d+)\.(\d+)\.(\d+)-alpha$/.exec(tag);
  if (!match) throw new Error(`Invalid runtime tag: ${tag}`);
  return match.slice(1).map((part) => Number.parseInt(part, 10));
}

function compareRuntimeTags(left, right) {
  const a = runtimeTagParts(left);
  const b = runtimeTagParts(right);
  for (let i = 0; i < a.length; i += 1) {
    if (a[i] !== b[i]) return a[i] - b[i];
  }
  return 0;
}

function publishOrigins(value) {
  return (value ?? '').split(',').map((item) => item.trim()).filter(Boolean).map((item) => {
    const url = new URL(item);
    if (url.protocol !== 'https:' || url.username || url.password) throw new Error(`Invalid publish URL: ${item}`);
    return url.origin;
  });
}

/** An ID token for the release account, made for an account service's address (IAM Credentials). */
async function idToken(accessToken, account, audience) {
  const response = await fetch(
    `https://iamcredentials.googleapis.com/v1/projects/-/serviceAccounts/${account}:generateIdToken`,
    {
      method: 'POST',
      headers: { Authorization: `Bearer ${accessToken}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({ audience, includeEmail: true }),
    }
  );
  const body = await response.json().catch(() => ({}));
  if (!response.ok || !body.token) {
    throw new Error(`Google refused an ID token for ${audience} (${response.status}): ${body.error?.message ?? ''}`);
  }
  return body.token;
}

/** The runtime tag the account service serves now, or null before its first release. */
async function currentRuntimeTag(origin) {
  const response = await fetch(`${origin}/api/current-release`, { headers: { Accept: 'application/json' } });
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`GET ${origin}/api/current-release failed with ${response.status}`);
  return (await response.json()).runtimeTag ?? null;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const tag = args['runtime-tag'];
  runtimeTagParts(tag ?? '');
  const origins = publishOrigins(args['publish-url']);
  if (!origins.length) throw new Error('Give --publish-url');
  const body = { runtimeTag: tag };

  if (args['dry-run'] === 'true') {
    for (const origin of origins) console.log(`Would publish to ${origin}/api/current-release: ${JSON.stringify(body)}`);
    return;
  }

  const current = await currentRuntimeTag(origins[0]);
  if (current && compareRuntimeTags(tag, current) < 0 && args["allow-runtime-downgrade"] !== "true") {
    throw new Error(`Refusing to promote older runtime tag ${tag} over current ${current}; rerun with --allow-runtime-downgrade only for an intentional rollback`);
  }

  const accessToken = process.env.GOOGLE_ACCESS_TOKEN?.trim();
  const account = process.env.RELEASE_ACCOUNT?.trim();
  if (!accessToken || !account) {
    throw new Error('GOOGLE_ACCESS_TOKEN and RELEASE_ACCOUNT are needed (the release workflow signs in keylessly)');
  }
  await publishToAccountServices(origins, accessToken, account, body);
}

async function publishToAccountServices(origins, accessToken, account, body) {
  for (const origin of origins) {
    const token = await idToken(accessToken, account, origin);
    const response = await fetch(`${origin}/api/current-release`, {
      method: 'PATCH',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!response.ok) {
      throw new Error(`Publishing to ${origin} failed with ${response.status}: ${await response.text()}`);
    }
    console.log(`Published runtime ${body.runtimeTag} to ${origin}`);
  }
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : String(error));
  usage();
  process.exit(1);
});
