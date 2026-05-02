#!/usr/bin/env node
import { createSign } from 'node:crypto';
import { request as httpsRequest } from 'node:https';

function usage() {
  console.error(`Usage:
  node scripts/update-firebase-current-release.mjs --app-tag app-v0.0.130-alpha [--app-channel firebase-app-distribution] [--status-label "..."] [--project commaview]
  node scripts/update-firebase-current-release.mjs --runtime-tag v0.0.47-alpha [--project commaview]

Requires FIREBASE_SERVICE_ACCOUNT_JSON unless --dry-run is set.`);
}

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i += 1) {
    const key = argv[i];
    if (!key.startsWith('--')) throw new Error(`Unexpected positional argument: ${key}`);
    if (key === '--dry-run') {
      out.dryRun = 'true';
      continue;
    }
    const value = argv[i + 1];
    if (!value || value.startsWith('--')) throw new Error(`Missing value for ${key}`);
    out[key.slice(2)] = value;
    i += 1;
  }
  return out;
}

function appVersionFromTag(tag) {
  const match = /^app-v(\d+\.\d+\.\d+-alpha)$/.exec(tag);
  if (!match) throw new Error(`Invalid app tag: ${tag}`);
  return match[1];
}

function runtimeVersionFromTag(tag) {
  const match = /^v(\d+\.\d+\.\d+-alpha)$/.exec(tag);
  if (!match) throw new Error(`Invalid runtime tag: ${tag}`);
  return match[1];
}

function base64urlJson(value) {
  return Buffer.from(JSON.stringify(value)).toString('base64url');
}

function firestoreString(value) {
  return { stringValue: value };
}

function requestJson(url, { method = 'GET', headers = {}, body } = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? undefined : Buffer.from(body);
    const req = httpsRequest(url, {
      method,
      headers: {
        ...headers,
        ...(payload ? { 'Content-Length': String(payload.length) } : {}),
      },
    }, (res) => {
      const chunks = [];
      res.on('data', (chunk) => chunks.push(chunk));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        if ((res.statusCode ?? 500) >= 300) {
          reject(new Error(`${method} ${url} failed with ${res.statusCode}: ${text}`));
          return;
        }
        resolve(text ? JSON.parse(text) : {});
      });
    });
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}

async function accessToken(serviceAccount) {
  const now = Math.floor(Date.now() / 1000);
  const header = base64urlJson({ alg: 'RS256', typ: 'JWT' });
  const claim = base64urlJson({
    iss: serviceAccount.client_email,
    scope: 'https://www.googleapis.com/auth/datastore',
    aud: 'https://oauth2.googleapis.com/token',
    iat: now,
    exp: now + 3600,
  });
  const unsigned = `${header}.${claim}`;
  const signature = createSign('RSA-SHA256').update(unsigned).sign(serviceAccount.private_key).toString('base64url');
  const assertion = `${unsigned}.${signature}`;
  const tokenResponse = await requestJson('https://oauth2.googleapis.com/token', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      grant_type: 'urn:ietf:params:oauth:grant-type:jwt-bearer',
      assertion,
    }).toString(),
  });
  if (!tokenResponse.access_token) throw new Error('OAuth token response did not include access_token');
  return tokenResponse.access_token;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const fields = {};

  if (args['app-tag']) {
    fields.appTag = firestoreString(args['app-tag']);
    fields.appVersion = firestoreString(appVersionFromTag(args['app-tag']));
    fields.channel = firestoreString(args['app-channel'] ?? 'firebase-app-distribution');
  }

  if (args['runtime-tag']) {
    fields.runtimeTag = firestoreString(args['runtime-tag']);
    fields.runtimeVersion = firestoreString(runtimeVersionFromTag(args['runtime-tag']));
  }

  if (args['status-label']) fields.statusLabel = firestoreString(args['status-label']);
  fields.updatedAt = firestoreString(new Date().toISOString());

  const fieldNames = Object.keys(fields);
  if (fieldNames.length === 1) {
    usage();
    throw new Error('Provide --app-tag and/or --runtime-tag');
  }

  const serviceAccountJson = process.env.FIREBASE_SERVICE_ACCOUNT_JSON;
  const serviceAccount = serviceAccountJson ? JSON.parse(serviceAccountJson) : undefined;
  const projectId = args.project ?? serviceAccount?.project_id ?? 'commaview';
  const documentUrl = new URL(`https://firestore.googleapis.com/v1/projects/${projectId}/databases/(default)/documents/publicConfig/currentRelease`);
  for (const field of fieldNames) documentUrl.searchParams.append('updateMask.fieldPaths', field);

  if (args.dryRun === 'true') {
    console.log(JSON.stringify({ projectId, document: 'publicConfig/currentRelease', fields }, null, 2));
    return;
  }

  if (!serviceAccount?.client_email || !serviceAccount?.private_key) {
    throw new Error('FIREBASE_SERVICE_ACCOUNT_JSON must include client_email and private_key');
  }

  const token = await accessToken(serviceAccount);
  await requestJson(documentUrl, {
    method: 'PATCH',
    headers: {
      Authorization: `Bearer ${token}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({ fields }),
  });
  console.log(`Updated Firestore current release manifest in project ${projectId}: ${fieldNames.join(', ')}`);
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : String(error));
  usage();
  process.exit(1);
});
