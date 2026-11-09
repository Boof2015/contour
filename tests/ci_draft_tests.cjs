'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const createDraft = require('../tools/ci/create-draft.cjs');

function fixture(t, options = {}) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'contour-ci-draft-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const commit = 'a'.repeat(40), version = '0.1.0', name = `Contour-${version}-Setup-x64.exe`;
  const data = Buffer.from('isolated installer fixture');
  const sha256 = crypto.createHash('sha256').update(data).digest('hex');
  fs.writeFileSync(path.join(directory, name), data);
  fs.writeFileSync(path.join(directory, `${name}.sha256`), `${sha256}  ${name}\r\n`);
  fs.writeFileSync(path.join(directory, `${name}.json`), JSON.stringify({ schema: 1, version, architecture: 'x64', sha256 }));
  fs.writeFileSync(path.join(directory, 'build-info.json'), JSON.stringify({ schema: 1, version, commit, architecture: 'x64', configuration: 'Release' }));
  const mutations = [];
  const github = {
    paginate: async () => options.releases || [],
    rest: {
      git: {
        getRef: async () => {
          if (options.ref) return { data: { object: options.ref } };
          throw Object.assign(new Error('No tag'), { status: options.refError || 404 });
        },
        getTag: async () => ({ data: { object: { type: 'commit', sha: commit } } }),
      },
      repos: {
        listReleases: () => {},
        createRelease: async args => {
          mutations.push(['create', args]);
          return { data: { id: 42, draft: true, html_url: 'https://example.test/draft' } };
        },
        uploadReleaseAsset: async args => {
          mutations.push(['upload', args]);
          if (options.failUpload) throw new Error('Upload failed');
        },
      },
    },
  };
  const args = { directory, github, core: { info() {} }, context: {
    eventName: 'workflow_dispatch', payload: { inputs: { create_draft_release: 'true' } },
    sha: commit, repo: { owner: 'fixture', repo: 'contour' },
  } };
  return { args, mutations, name, commit };
}

test('creates only a draft at the tested commit and uploads exactly the verified files', async t => {
  const f = fixture(t);
  await createDraft(f.args);
  assert.equal(f.mutations[0][0], 'create');
  assert.equal(f.mutations[0][1].draft, true);
  assert.equal(f.mutations[0][1].target_commitish, f.commit);
  assert.equal(f.mutations[0][1].tag_name, 'v0.1.0');
  assert.deepEqual(f.mutations.slice(1).map(m => m[1].name).sort(),
    [f.name, `${f.name}.sha256`, `${f.name}.json`, 'build-info.json'].sort());
});
for (const requested of [false, 'false', undefined]) {
  test(`artifact-only request (${requested}) cannot create a release`, async t => {
    const f = fixture(t);
    f.args.context.payload.inputs.create_draft_release = requested;
    await assert.rejects(createDraft(f.args), /explicit manual request/);
    assert.equal(f.mutations.length, 0);
  });
}
test('non-manual events cannot create a draft', async t => {
  const f = fixture(t); f.args.context.eventName = 'push';
  await assert.rejects(createDraft(f.args), /explicit manual request/);
  assert.equal(f.mutations.length, 0);
});
for (const damage of ['checksum', 'revision', 'extra file']) {
  test(`rejects ${damage} before creating a draft`, async t => {
    const f = fixture(t);
    if (damage === 'checksum') fs.appendFileSync(path.join(f.args.directory, f.name), 'changed');
    if (damage === 'revision') f.args.context.sha = 'b'.repeat(40);
    if (damage === 'extra file') fs.writeFileSync(path.join(f.args.directory, 'internal.txt'), 'private');
    await assert.rejects(createDraft(f.args), /mismatch|revision|Unexpected/);
    assert.equal(f.mutations.length, 0);
  });
}
for (const draft of [false, true]) {
  test(`preserves existing ${draft ? 'draft' : 'published release'}`, async t => {
    const f = fixture(t, { releases: [{ tag_name: 'v0.1.0', draft }] });
    await assert.rejects(createDraft(f.args), /will not be replaced/);
    assert.equal(f.mutations.length, 0);
  });
}
test('refuses an existing tag for another commit', async t => {
  const f = fixture(t, { ref: { type: 'commit', sha: 'b'.repeat(40) } });
  await assert.rejects(createDraft(f.args), /different revision/);
  assert.equal(f.mutations.length, 0);
});
test('accepts an annotated tag pointing to the tested commit', async t => {
  const f = fixture(t, { ref: { type: 'tag', sha: 'c'.repeat(40) } });
  await createDraft(f.args);
  assert.equal(f.mutations[0][1].draft, true);
});
test('API failure is not mistaken for a missing tag', async t => {
  const f = fixture(t, { refError: 403 });
  await assert.rejects(createDraft(f.args), /No tag/);
  assert.equal(f.mutations.length, 0);
});
test('upload failure leaves the draft for review and never publishes or deletes it', async t => {
  const f = fixture(t, { failUpload: true });
  await assert.rejects(createDraft(f.args), /Upload failed/);
  assert.deepEqual(f.mutations.map(m => m[0]), ['create', 'upload']);
  assert.equal(f.mutations[0][1].draft, true);
});
