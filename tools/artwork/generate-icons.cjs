// Regenerate the checked-in Windows icon from its SVG with Chromium's SVG
// renderer. Only artwork changes need Node and Edge; normal builds use the ICO.
// Usage: node tools/artwork/generate-icons.cjs
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const {spawn} = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const assets = path.join(root, 'src/app/assets');
const work = path.join(root, 'build/icon-artwork');
const edge = [process.env.CONTOUR_TEST_BROWSER,
  path.join(process.env['ProgramFiles(x86)'] || '', 'Microsoft/Edge/Application/msedge.exe'),
  path.join(process.env.ProgramFiles || '', 'Microsoft/Edge/Application/msedge.exe')
].find(file => file && fs.existsSync(file));
if (!edge) throw Error('Microsoft Edge is required to render the icon');
fs.mkdirSync(work, {recursive: true});
const profile = fs.mkdtempSync(path.join(work, 'browser-'));
const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
const svg = fs.readFileSync(path.join(assets, 'icon.svg'), 'utf8');
const render = async (source, sizes) => {
  try {
    const image = new Image();
    image.src = 'data:image/svg+xml;base64,' + btoa(source);
    await image.decode();
    const images = sizes.map(size => {
      const canvas = document.createElement('canvas');
      canvas.width = canvas.height = size;
      canvas.getContext('2d').drawImage(image, 0, 0, size, size);
      return canvas.toDataURL('image/png').split(',')[1];
    });
    await fetch('/result', {method: 'POST', body: JSON.stringify({images})});
  } catch (error) {
    await fetch('/result', {method: 'POST', body: JSON.stringify({error: String(error)})});
  }
};
let browser, timeout, finished = false;
const finish = error => {
  if (finished) return;
  finished = true;
  clearTimeout(timeout);
  if (browser) browser.kill();
  server.close();
  if (error) { console.error(error); process.exitCode = 1; }
};
const server = http.createServer((req, res) => {
  if (req.url === '/' && req.method === 'GET') {
    res.setHeader('Content-Type', 'text/html');
    res.end(`<script>(${render.toString()})(${JSON.stringify(svg)},${JSON.stringify(sizes)});</script>`);
  } else if (req.url === '/result' && req.method === 'POST') {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      try {
        const result = JSON.parse(body);
        if (result.error) throw Error(result.error);
        if (result.images.length !== sizes.length) throw Error('Incomplete icon render');
        const images = result.images.map(data => Buffer.from(data, 'base64'));
        const header = Buffer.alloc(6 + sizes.length * 16);
        header.writeUInt16LE(1, 2);
        header.writeUInt16LE(sizes.length, 4);
        let offset = header.length;
        images.forEach((png, i) => {
          const size = sizes[i], entry = 6 + i * 16;
          if (png.toString('hex', 0, 8) !== '89504e470d0a1a0a' ||
              png.readUInt32BE(16) !== size || png.readUInt32BE(20) !== size) {
            throw Error('Invalid rendered PNG');
          }
          header[entry] = header[entry + 1] = size === 256 ? 0 : size;
          header.writeUInt16LE(1, entry + 4);
          header.writeUInt16LE(32, entry + 6);
          header.writeUInt32LE(png.length, entry + 8);
          header.writeUInt32LE(offset, entry + 12);
          offset += png.length;
          fs.writeFileSync(path.join(work, `icon-${size}.png`), png);
        });
        fs.writeFileSync(path.join(assets, 'contour.ico'), Buffer.concat([header, ...images]));
        console.log(`Generated contour.ico (${sizes.join(', ')} px)`);
        res.end('OK'); finish();
      } catch (error) { res.writeHead(500).end(); finish(error); }
    });
  } else res.writeHead(404).end();
});
server.listen(0, '127.0.0.1', () => {
  browser = spawn(edge, ['--headless=new', '--disable-gpu', '--no-first-run',
    '--disable-background-networking', '--disable-extensions', '--disable-sync',
    `--user-data-dir=${profile}`, `http://127.0.0.1:${server.address().port}/`],
  {windowsHide: true, stdio: 'ignore'});
  browser.on('error', finish);
  browser.on('exit', () => { if (!finished) finish(Error('Browser exited before rendering')); });
  timeout = setTimeout(() => finish(Error('Icon rendering timed out')), 30000);
});
