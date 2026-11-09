'use strict';
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

module.exports = async function createDraft({ github, context, core, directory }) {
  const requested = context.payload?.inputs?.create_draft_release;
  if (context.eventName !== 'workflow_dispatch' || (requested !== true && requested !== 'true')) {
    throw new Error('Draft creation requires an explicit manual request.');
  }
  const read = name => fs.readFileSync(path.join(directory, name));
  const build = JSON.parse(read('build-info.json'));
  if (build.schema !== 1 || !/^\d+\.\d+\.\d+$/.test(build.version) ||
      !/^[a-f0-9]{40}$/.test(build.commit) || build.commit !== context.sha ||
      build.configuration !== 'Release' || build.architecture !== 'x64') {
    throw new Error('Installer metadata does not match the tested workflow revision.');
  }
  const installer = `Contour-${build.version}-Setup-x64.exe`;
  const names = [installer, `${installer}.sha256`, `${installer}.json`, 'build-info.json'].sort();
  if (JSON.stringify(fs.readdirSync(directory).sort()) !== JSON.stringify(names) ||
      names.some(name => !fs.lstatSync(path.join(directory, name)).isFile())) {
    throw new Error('Unexpected files in the installer artifact.');
  }
  const files = names.map(name => ({ name, data: read(name) }));
  const hash = crypto.createHash('sha256').update(files.find(f => f.name === installer).data).digest('hex');
  const checksum = read(`${installer}.sha256`).toString().trim();
  const metadata = JSON.parse(read(`${installer}.json`));
  if (checksum.toLowerCase() !== `${hash}  ${installer}`.toLowerCase() ||
      metadata.schema !== 1 || metadata.version !== build.version || metadata.architecture !== 'x64' ||
      metadata.sha256?.toLowerCase() !== hash) {
    throw new Error('Installer checksum or metadata mismatch.');
  }

  const repo = context.repo;
  const tag = `v${build.version}`;
  const releases = await github.paginate(github.rest.repos.listReleases, { ...repo, per_page: 100 });
  if (releases.some(release => release.tag_name === tag)) {
    throw new Error(`${tag} already has a release or draft. It will not be replaced; the workflow artifact is still available.`);
  }
  let ref;
  try { ref = (await github.rest.git.getRef({ ...repo, ref: `tags/${tag}` })).data.object; }
  catch (error) { if (error.status !== 404) throw error; }
  // Existing annotated tags must also resolve to the exact revision we tested.
  for (let depth = 0; ref?.type === 'tag' && depth < 8; ++depth) {
    ref = (await github.rest.git.getTag({ ...repo, tag_sha: ref.sha })).data.object;
  }
  if (ref && (ref.type !== 'commit' || ref.sha !== build.commit)) {
    throw new Error(`${tag} already points to a different revision.`);
  }
  const { data: release } = await github.rest.repos.createRelease({
    ...repo, tag_name: tag, target_commitish: build.commit, name: `Contour ${build.version}`,
    draft: true,
    body: `Installer built and tested from ${build.commit}.\n\nReview the installer and add release notes before publishing manually.`,
  });
  if (release.draft !== true) throw new Error('GitHub did not return a draft; refusing to upload assets.');
  // No publishing, replacement, deletion, or retry that could overwrite a release.
  for (const file of files) {
    await github.rest.repos.uploadReleaseAsset({ ...repo, release_id: release.id, ...file,
      headers: { 'content-type': 'application/octet-stream', 'content-length': file.data.length },
    });
  }
  core.info(`Draft created: ${release.html_url}`);
};
