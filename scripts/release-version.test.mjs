// node --test scripts/release-version.test.mjs
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { nextRelease, versionCode } from './release-version.mjs';

const next = (channel, tags, extra = {}) =>
  nextRelease({ prefix: 'app-v', floor: '0.0.159', channel, tags, ...extra });

test('the first alpha of a version is .1, then each one is the next number', () => {
  assert.deepEqual(next('alpha', ['app-v0.0.158-alpha']), {
    tag: 'app-v0.0.159-alpha.1',
    version: '0.0.159-alpha.1',
    version_code: 15901,
  });
  assert.equal(next('alpha', ['app-v0.0.159-alpha.1', 'app-v0.0.159-alpha.3']).tag, 'app-v0.0.159-alpha.4');
});

test('the beta is built from the newest alpha, or the one chosen', () => {
  const tags = ['app-v0.0.159-alpha.1', 'app-v0.0.159-alpha.2'];
  assert.deepEqual(next('beta', tags), {
    tag: 'app-v0.0.159-beta',
    version: '0.0.159-beta',
    version_code: 15999,
    from_tag: 'app-v0.0.159-alpha.2',
  });
  assert.equal(next('beta', tags, { from: 'app-v0.0.159-alpha.1' }).from_tag, 'app-v0.0.159-alpha.1');
  assert.throws(() => next('beta', tags, { from: 'app-v0.0.158-alpha' }), /isn't an alpha of 0.0.159/);
  assert.throws(() => next('beta', []), /no alpha/);
});

test('after its beta, the next alpha is the next patch', () => {
  const tags = ['app-v0.0.159-alpha.2', 'app-v0.0.159-beta'];
  assert.equal(next('alpha', tags).tag, 'app-v0.0.160-alpha.1');
  assert.throws(() => next('beta', tags), /0.0.160 has no alpha/);
});

test('the old single alpha tag counts as alpha 0', () => {
  const runtime = nextRelease({ prefix: 'v', floor: '0.0.56', channel: 'alpha', tags: ['v0.0.56-alpha'] });
  assert.equal(runtime.tag, 'v0.0.56-alpha.1');
  const beta = nextRelease({ prefix: 'v', floor: '0.0.56', channel: 'beta', tags: ['v0.0.56-alpha'] });
  assert.equal(beta.from_tag, 'v0.0.56-alpha');
});

test('other prefixes and unrelated tags are ignored', () => {
  assert.equal(next('alpha', ['v0.0.159-alpha.7', 'app-v0.0.159-alpha.2-rc', 'app-v0.0.159']).tag, 'app-v0.0.159-alpha.1');
});

test('version codes always grow', () => {
  const order = [
    versionCode([0, 0, 159], 1),
    versionCode([0, 0, 159], 98),
    versionCode([0, 0, 159], 99),
    versionCode([0, 0, 160], 1),
    versionCode([0, 1, 0], 1),
    versionCode([1, 0, 0], 1),
  ];
  assert.deepEqual([...order].sort((a, b) => a - b), order);
  assert.ok(versionCode([0, 0, 159], 1) > 159, 'above the old scheme');
  assert.throws(() => versionCode([21, 0, 0], 1));
});
