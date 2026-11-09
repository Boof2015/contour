'use strict';
let FS=48000, NYQ=24000, LMIN=Math.log10(20), LSPAN=3; const MAXF=64, MAXL=32;
const TYPES = [['PK', 'Peak', 1], ['LSQ', 'Low shelf', 1], ['HSQ', 'High shelf', 1], ['LPQ', 'Low pass', 0], ['HPQ', 'High pass', 0], ['NO', 'Notch', 0], ['AP', 'All pass', 0], ['BP', 'Band pass', 0]];
const TYPE = Object.fromEntries(TYPES.map(([id, name, g]) => [id, { name, gain: !!g }]));
const hasGain = t => TYPE[t].gain;
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const clone = o => JSON.parse(JSON.stringify(o));
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const $ = s => document.querySelector(s), $$ = s => [...document.querySelectorAll(s)];

const POS = {
  FL: ['L', 'Front left'], FR: ['R', 'Front right'], FC: ['C', 'Center'], LFE: ['LFE', 'LFE'],
  BL: ['BL', 'Back left'], BR: ['BR', 'Back right'], FLC: ['FLC', 'Front left of center'], FRC: ['FRC', 'Front right of center'],
  BC: ['BC', 'Back center'], SL: ['SL', 'Side left'], SR: ['SR', 'Side right'], TC: ['TC', 'Top center'],
  TFL: ['TFL', 'Top front left'], TFC: ['TFC', 'Top front center'], TFR: ['TFR', 'Top front right'],
  TBL: ['TBL', 'Top back left'], TBC: ['TBC', 'Top back center'], TBR: ['TBR', 'Top back right'],
};
// Angle from straight ahead (degrees) and ring (1 = ear level, .52 = height)
const MAP = { FL: [-30, 1], FR: [30, 1], FC: [0, 1], FLC: [-15, 1], FRC: [15, 1], SL: [-90, 1], SR: [90, 1], BL: [-145, 1], BR: [145, 1], BC: [180, 1],
  TFL: [-45, .5], TFR: [45, .5], TFC: [0, .5], TBL: [-135, .5], TBR: [135, .5], TBC: [180, .5] };
const GROUPS = [['Fronts', p => ['FL', 'FR', 'FC', 'FLC', 'FRC'].includes(p)], ['Surrounds', p => ['SL', 'SR', 'BL', 'BR', 'BC'].includes(p)], ['Heights', p => p[0] === 'T'], ['LFE', p => p === 'LFE']];

const chCount = d => d.pos ? d.pos.length : d.count;
const isStereo = d => d.pos && d.pos.join() === 'FL,FR';
const layoutKey = d => d.pos ? d.pos.join() : 'n' + d.count;
function layoutName(key) {
  if (key === 'FL,FR') return 'Stereo';
  if (key === 'FL,FR,FC,LFE,BL,BR' || key === 'FL,FR,FC,LFE,SL,SR') return '5.1';
  if (key === 'FL,FR,FC,LFE,BL,BR,SL,SR') return '7.1';
  if (key === 'FL,FR,FC,LFE,BL,BR,SL,SR,TFL,TFR,TBL,TBR') return '7.1.4';
  const n = key[0] === 'n' ? +key.slice(1) : key.split(',').length;
  return `${n} channels`;
}
function shortName(d, c) { if (!d.pos) return String(c + 1); if (isStereo(d)) return c ? 'R' : 'L'; return POS[d.pos[c]][0]; }
function longName(d, c) { if (!d.pos) return `Channel ${c + 1}`; if (isStereo(d)) return c ? 'Right' : 'Left'; return POS[d.pos[c]][1]; }
const fullMask = d => chCount(d) >= 32 ? 0xffffffff : (1 << chCount(d)) - 1;
function groupsFor(d) {
  if (!d.pos || isStereo(d)) return [];
  return GROUPS.map(([name, test]) => [name, d.pos.reduce((m, p, c) => test(p) ? m | (1 << c) : m, 0)]).filter(([, m]) => m && m !== fullMask(d));
}
const bits = m => { const out = []; for (let c = 0; c < 32; c++) if ((m >>> c) & 1) out.push(c); return out; };
function targetSummary(d, l, long = false) {
  if (l.all) return long ? 'All channels' : 'All';
  const cs = bits(l.mask);
  if (cs.length === 1) return longName(d, cs[0]);
  const g = groupsFor(d).find(([, m]) => m === l.mask);
  if (g) return g[0];
  if (cs.length === chCount(d) && isStereo(d)) return 'Left + Right';
  if (cs.length <= 3) return d.pos ? cs.map(c => shortName(d, c)).join(', ') : 'Channels ' + cs.map(c => c + 1).join(', ');
  return `${cs.length} channels`;
}
const targets = (l, c) => l.all || ((l.mask >>> c) & 1) === 1;

const F = (type, freq, gain, q, on = true) => ({ type, freq, gain, q, on });
const Ly = (name, all, mask, gain, on, filters) => ({ name, all, mask, gain, on, filters });
// ============================================================ Values
const M = '−';
const sgn = v => (v > 0 ? '+' : v < 0 ? M : '');
function show(k, v) {
  if (k === 'freq') return v < 1000 ? [v < 100 && Math.round(v) !== v ? v.toFixed(1) : String(Math.round(v)), 'Hz'] : [(v / 1000).toFixed(v < 10000 ? 2 : 1), 'kHz'];
  if (k === 'q') return [v < 10 ? v.toFixed(2) : v.toFixed(1), ''];
  return [sgn(v) + Math.abs(v).toFixed(1), 'dB'];
}
const roundF = v => v < 100 ? Math.round(v * 10) / 10 : v < 1000 ? Math.round(v) : v < 10000 ? Math.round(v / 10) * 10 : Math.round(v / 100) * 100;
const r1 = v => Math.round(v * 10) / 10, rq = v => v < 10 ? Math.round(v * 100) / 100 : Math.round(v * 10) / 10;
function get(k, i) { return k === 'lgain' ? L().gain : L().filters[i][k]; }
function set(k, i, v) {
  setLayerField(L(),k,i,v);
}
function setLayerField(layer,k,i,v) {
  if (k === 'lgain') { layer.gain = r1(clamp(v, -120, 36)); return; }
  const f = layer.filters[i];
  if (k === 'freq') f.freq = roundF(clamp(v, 1, NYQ - 1));
  else if (k === 'gain') f.gain = r1(clamp(v, -120, 36));
  else f.q = rq(clamp(v, .05, 1000));
}
// A bare number is read in the unit currently shown (3.2 in a kHz field means 3.2 kHz)
function parse(k, text, unit) {
  const t = text.trim().toLowerCase().replace(/−/g, '-').replace(',', '.');
  let m;
  if (k === 'freq') { m = t.match(/^([+-]?\d*\.?\d+)\s*(k|khz|hz)?$/); if (!m) return null; const kilo = m[2] ? m[2][0] === 'k' : unit === 'kHz'; const v = +m[1] * (kilo ? 1000 : 1); return v >= 1 && v < NYQ ? v : null; }
  if (k === 'q') { m = t.match(/^q?\s*(\d*\.?\d+)$/); if (!m) return null; return +m[1] >= .05 && +m[1] <= 1000 ? +m[1] : null; }
  m = t.match(/^([+-]?\d*\.?\d+)\s*(db)?$/); if (!m) return null; return +m[1] >= -120 && +m[1] <= 36 ? +m[1] : null;
}
const step = (k, v, dir, big) => k === 'freq' ? v * Math.pow(2, dir * (big ? 1 / 3 : 1 / 24)) : k === 'q' ? v * Math.pow(big ? 1.25 : 1.05, dir) : v + dir * (big ? 1 : .1);
const scrub = (k, start, dx) => k === 'freq' ? start * Math.pow(2, dx / 90) : k === 'q' ? start * Math.pow(2, dx / 140) : start + dx * .05;

// ============================================================ Icons, glyphs, previews
const PATHS = {
  settings: '<path d="M6.5 1.5h3l.4 1.8 1.6.9 1.7-.5 1.5 2.6-1.4 1.3v.8l1.4 1.3-1.5 2.6-1.7-.5-1.6.9-.4 1.8h-3l-.4-1.8-1.6-.9-1.7.5-1.5-2.6 1.4-1.3v-.8L1.3 6.3l1.5-2.6 1.7.5 1.6-.9z"/><circle cx="8" cy="8" r="2.5"/>',
  refresh: '<path d="M13 6A5.2 5.2 0 1 0 13 10M13 2v4H9"/>',
  speaker: '<path d="M2.5 6h2.2L8.5 3v10L4.7 10H2.5z"/><path d="M11 5.6a3.4 3.4 0 0 1 0 4.8"/>',
  tv: '<rect x="1.8" y="3" width="12.4" height="8.4" rx="1.4"/><path d="M5.5 13.6h5"/>',
  headphones: '<path d="M3 11.5V8.5a5 5 0 0 1 10 0v3"/><rect x="2" y="10" width="3" height="4" rx="1"/><rect x="11" y="10" width="3" height="4" rx="1"/>',
  up: '<path d="M8 12.5v-9M4.5 7 8 3.5 11.5 7"/>', down: '<path d="M8 3.5v9M4.5 9 8 12.5 11.5 9"/>',
  rack: '<rect x="1.8" y="4" width="12.4" height="8" rx="1.4"/><circle cx="5" cy="8" r="1"/><circle cx="8" cy="8" r="1"/><circle cx="11" cy="8" r="1"/>',
  chev: '<path d="M4.5 6.5 8 10l3.5-3.5"/>', plus: '<path d="M8 3.5v9M3.5 8h9"/>', check: '<path d="M3.5 8.5l3 3 6-7"/>',
  dots: '<circle cx="3.5" cy="8" r=".9" fill="currentColor"/><circle cx="8" cy="8" r=".9" fill="currentColor"/><circle cx="12.5" cy="8" r=".9" fill="currentColor"/>',
  undo: '<path d="M5.5 2.5 2.5 5.5l3 3"/><path d="M2.8 5.5h6.7a4 4 0 0 1 0 8H7"/>', redo: '<path d="M10.5 2.5l3 3-3 3"/><path d="M13.2 5.5H6.5a4 4 0 0 0 0 8H9"/>',
  copy: '<rect x="5.5" y="5.5" width="8" height="8" rx="1.5"/><path d="M3 10.5V3.8c0-.7.6-1.3 1.3-1.3h6.2"/>',
  trash: '<path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.6 8.5h5.8l.6-8.5"/>', x: '<path d="M4.5 4.5l7 7M11.5 4.5l-7 7"/>',
  pen: '<path d="M10.5 3l2.5 2.5L6 12.5H3.5V10z"/>', export: '<path d="M8 10V2.5M5 5.5l3-3 3 3"/><path d="M2.5 11v2.5h11V11"/>', import: '<path d="M8 2.5v7.5M5 7l3 3 3-3"/><path d="M2.5 11v2.5h11V11"/>',
  back: '<path d="M10 3.5 5.5 8l4.5 4.5"/>', grid: '<rect x="2.5" y="2.5" width="4.5" height="4.5" rx="1"/><rect x="9" y="2.5" width="4.5" height="4.5" rx="1"/><rect x="2.5" y="9" width="4.5" height="4.5" rx="1"/><rect x="9" y="9" width="4.5" height="4.5" rx="1"/>',
  save: '<path d="M3 2.5h8l2.5 2.5v8.5H3z"/><path d="M5.5 2.5v3.5h5v-3.5M5.5 13.5v-4h5v4"/>',
};
const I = n => `<svg class="ico" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">${PATHS[n]}</svg>`;
function glyph(f, i) {
  const up = !hasGain(f.type) || f.gain >= 0;
  const flip = d => up ? d : d.replace(/(\d+(?:\.\d+)?) (\d+(?:\.\d+)?)/g, (m, x, y) => `${x} ${(14 - +y).toFixed(1)}`);
  const d = { PK: 'M1 11 C8 11 10 3 13 3 S18 11 25 11', LSQ: 'M1 4 C8 4 11 11 15 11 L25 11', HSQ: 'M1 11 L11 11 C15 11 18 4 25 4', LPQ: 'M1 5 L12 5 C17 5 18 13 25 13', HPQ: 'M1 13 C8 13 9 5 14 5 L25 5', NO: 'M1 4 L10 4 C12 4 12.4 13 13 13 S14 4 16 4 L25 4', AP: 'M1 7 L25 7', BP: 'M1 13 C8 13 10 3 13 3 S18 13 25 13' }[f.type];
  return `<svg class="glyph" data-glyph="${i}" viewBox="0 0 26 14" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="${hasGain(f.type) ? flip(d) : d}"/></svg>`;
}
const WORDMARK = '<span class="wordmark" role="img" aria-label="Contour"></span>';
function pathOf(r, g, w, h, z) {
  const y = v => (h / 2 - clamp(v, -z, z) / z * (h / 2 - 3)).toFixed(2);
  let d = ''; for (let i = 0; i < g.n; i++) d += (i ? 'L' : 'M') + (i / (g.n - 1) * w).toFixed(1) + ' ' + y(r[i]);
  return d;
}
// Same ±9 dB scale on every row, so layers compare at a glance
function preview(l, sel) {
  const g = grid(72), w = 240, h = 34, line = pathOf(resp(l.filters, g), g, w, h, 9);
  const col = !l.on ? 'var(--text-3)' : sel ? 'var(--text)' : 'var(--text-2)';
  return `<svg class="pv" viewBox="0 0 ${w} ${h}" preserveAspectRatio="none"><path d="M0 ${h / 2}H${w}" stroke="var(--g-zero)" stroke-width="1" vector-effect="non-scaling-stroke"/>
    <path d="${line}L${w} ${h / 2}L0 ${h / 2}Z" fill="${col}" opacity="${l.on ? (sel ? .14 : .08) : .04}"/>
    <path d="${line}" fill="none" stroke="${col}" stroke-width="1.6" vector-effect="non-scaling-stroke" stroke-linejoin="round" ${l.on ? '' : 'stroke-dasharray="3 3"'}/></svg>`;
}

// ============================================================ Templates
function field(k, i, f) {
  const dis = k === 'gain' && !hasGain(f.type);
  const [v, u] = dis ? ['—', ''] : show(k, k === 'lgain' ? L().gain : f[k]);
  return `<label class="fld${dis ? ' dis' : ''}">${k === 'q' ? '<span class="pre">Q</span>' : ''}${k === 'lgain' ? '<span class="pre" style="margin-right:2px">Gain</span>' : ''}<input class="tnum" data-k="${k}" data-i="${i}" data-unit="${u}" value="${v}" ${dis ? 'disabled' : ''} spellcheck="false" aria-label="${k}"><span class="u">${u}</span></label>`;
}
function typeSel(f, i) {
  return `<button class="tsel" data-type="${i}" id="type-${i}" aria-label="Filter ${i + 1} type: ${TYPE[f.type].name}" aria-haspopup="listbox" aria-expanded="${S.pop==='filtertype'&&S.typeBand===i}">${glyph(f, i)}<span>${TYPE[f.type].name}</span></button>`;
}
function row(f, i) {
  return `<div class="row${i === S.band ? ' sel' : ''}${f.on ? '' : ' off'}" data-band="${i}" tabindex="0" aria-label="Filter ${i+1}">
    <span class="sw sm${f.on ? ' on' : ''}" data-bandon="${i}" title="${f.on ? 'Turn off' : 'Turn on'}"></span><span class="badge">${i + 1}</span>${typeSel(f, i)}${field('freq', i, f)}${field('gain', i, f)}${field('q', i, f)}<button class="rm" data-del="${i}" title="Remove filter (Delete)" aria-label="Remove filter ${i+1}">${I('x')}</button></div>`;
}
function bandsInner() {
  const l = L(); if (!l) return '';
  const n = l.filters.length;
  const body = n ? `<div class="rows" style="grid-template-rows:repeat(${Math.max(1, Math.ceil(n / 2))}, auto)">${l.filters.map(row).join('')}</div>`
    : `<div class="no-bands">No filters in this layer yet. Double-click the graph or use Add filter.</div>`;
  return `<div class="bands-h"><span>Filters</span><span class="n">${n} of ${MAXF}</span><button class="add" id="addBand" ${n>=MAXF?'disabled title="This layer has the maximum of '+MAXF+' filters"':''}>${I('plus')}Add filter</button></div>${body}`;
}
function editorView() {
  const d = D(), ls = LAYERS(), l = L();
  const layers = ls.map((x, i) => `<li tabindex="0" role="button" aria-label="${esc(x.name)}" data-layer="${i}" class="${i === S.sel ? 'sel' : ''}${x.on ? '' : ' off'}">
      <span class="nm">${esc(x.name)}</span><button class="sw sm${x.on ? ' on' : ''}" data-layeron="${i}" aria-label="Enable ${esc(x.name)}" aria-pressed="${x.on}"></button>
      <span class="mt">${targetSummary(d, x, true)} · ${x.filters.length} filter${x.filters.length === 1 ? '' : 's'}</span>
      ${preview(x, i === S.sel)}
      <div class="layer-actions" aria-label="Layer actions">${[['up','Move up',i===0],['down','Move down',i===ls.length-1],['trash','Delete layer',false]].map(([action,label,disabled])=>`<button class="ib" data-layer-action="${action}" data-index="${i}" title="${label}" aria-label="${label}: ${esc(x.name)}" ${disabled?'disabled':''}>${I(action)}</button>`).join('')}</div>
    </li>`).join('');
  const main = l ? `
      <main class="main">
        <div class="lh"><input class="ln-in" id="lnIn" value="${esc(l.name)}" spellcheck="false" aria-label="Layer name" maxlength="120">
          <button class="chf${S.pop === 'channels' ? ' open' : ''}" id="chBtn" title="Which channels this layer processes"><span class="k">Channels</span><span class="v">${targetSummary(d, l)}</span>${I('chev')}</button>
          <div class="gainbox">${field('lgain', 0, null)}</div>
          <button class="ib${S.pop === 'layermenu' ? ' open' : ''}" id="layerMenuBtn" title="Layer actions">${I('dots')}</button></div>
        <div class="well"><canvas id="graph" tabindex="0" aria-label="Filter graph. Select a point; Delete removes the selected filter."></canvas><div class="tip" id="tip" hidden></div>
          <div class="tools">${chCount(d) > 2
            ? `<div class="split"><button class="toggle${S.output ? ' on' : ''}" id="outToggle" title="Filter response from all layers, excluding layer gain"><i></i>Combined filters${S.output ? ` · ${S.outMode === 'layer' ? 'this layer\'s channels' : bits(S.outMask).length + ' chosen'}` : ''}</button><button class="toggle more${S.output ? ' on' : ''}${S.pop === 'outch' ? ' open' : ''}" id="outBtn" title="Choose channels to show">${I('chev')}</button></div>`
            : `<button class="toggle${S.output ? ' on' : ''}" id="outToggle" title="Filter response from all layers, excluding layer gain"><i></i>Combined filters</button>`}</div>
        </div>
        <div class="bands" id="bands">${bandsInner()}</div>
      </main>`
    : `<main class="main" style="grid-template-rows:1fr"><div class="well"><div class="empty">This profile has no layers. Add one or import filters.</div></div></main>`;
  return `<div class="body">
      <aside class="side">
        <div id="pbox">${profileBox()}</div>
        <div class="side-h"><span>Layers</span><button class="ib" data-act="add-layer" title="New layer">${I('plus')}</button></div>
        <ul class="layers">${layers || '<li class="empty-l" style="cursor:default">No layers yet.</li>'}</ul>
        <button class="prim" data-act="import">Import filters</button>
      </aside>
      ${main}
    </div>`;
}
function channelPicker(d, l) {
  const n = chCount(d);
  if (isStereo(d)) {
    const cur = l.all ? 'all' : l.mask === 1 ? '0' : l.mask === 2 ? '1' : 'both';
    const opt = (v, label, note) => `<button class="opt${cur === v ? ' on' : ''}" data-chopt="${v}"><span class="rd"></span>${label}${note ? `<small>${note}</small>` : ''}</button>`;
    return `<div style="width:260px">${opt('all', 'All channels', 'Left + Right')}${opt('0', 'Left')}${opt('1', 'Right')}</div>`;
  }
  const modes = `<div class="chp-modes"><button class="${l.all ? 'on' : ''}" data-chmode="all">All channels</button><button class="${l.all ? '' : 'on'}" data-chmode="sel">Selected</button></div>`;
  const on = c => l.all || ((l.mask >>> c) & 1);
  const cls = c => `spk${on(c) ? ' on' : ''}${l.all ? ' all' : ''}`;
  if (!d.pos) {
    const cells = Array.from({ length: n }, (_, c) => `<button class="${cls(c)}" data-spk="${c}" title="Channel ${c + 1}">${c + 1}</button>`).join('');
    return `<div class="chp">${modes}<div class="chp-note">${esc(d.name)} doesn't name its channels, so they're shown by number.</div><div class="grid12">${cells}</div></div>`;
  }
  // Speaker map: positions come from the device; anything without a map position goes along the bottom
  const W = 336, H = 272, cx = W / 2, cy = H / 2 - 4, rx = 106, ry = 106;
  let chips = '', extra = [];
  d.pos.forEach((p, c) => {
    const m = MAP[p];
    if (!m) { extra.push(c); return; }
    const a = m[0] * Math.PI / 180, x = cx + rx * m[1] * Math.sin(a), y = cy - ry * m[1] * Math.cos(a);
    chips += `<button class="${cls(c)}${m[1] < 1 ? ' h' : ''}" style="left:${x.toFixed(1)}px;top:${y.toFixed(1)}px" data-spk="${c}" title="${POS[p][1]}">${POS[p][0]}</button>`;
  });
  extra.forEach((c, k) => { chips += `<button class="${cls(c)}" style="left:${(W - 34 - k * 50).toFixed(1)}px;top:${H - 22}px" data-spk="${c}" title="${longName(d, c)}">${shortName(d, c)}</button>`; });
  const hasH = d.pos.some(p => p[0] === 'T');
  const rings = `<svg class="rings" viewBox="0 0 ${W} ${H}"><circle cx="${cx}" cy="${cy}" r="${rx}" fill="none" stroke="var(--g-major)" stroke-width="1"/>${hasH ? `<circle cx="${cx}" cy="${cy}" r="${rx * .5}" fill="none" stroke="var(--g-major)" stroke-width="1" stroke-dasharray="3 4"/>` : ''}</svg>`;
  const groups = groupsFor(d).map(([name, m]) => `<button class="${!l.all && l.mask === m ? 'on' : ''}" data-grp="${m}">${name}</button>`).join('');
  return `<div class="chp">${modes}<div class="map">${rings}<div class="you"><i></i>You</div>${chips}${hasH ? `<span class="lbl" style="left:12px;bottom:8px">Dashed ring: height speakers</span>` : ''}</div>${groups ? `<div class="qp">${groups}</div>` : ''}</div>`;
}
let nodes = [], hover = -1, drag = null, G = null;
let graphFrame=0;
function draw() {
  // Resize, native acknowledgements and pointer events can arrive in the same
  // display frame. Paint the latest state once, just before browser presentation.
  if(!graphFrame)graphFrame=requestAnimationFrame(()=>{graphFrame=0;paintGraph();});
}
function paintGraph() {
  const cv = $('#graph'); if (!cv || !L()) return;
  const box = cv.parentElement.getBoundingClientRect(), W = box.width, H = box.height;
  if (!W || !H) return;
  const dpr = devicePixelRatio || 1;
  if (cv.width !== Math.round(W * dpr)) cv.width = Math.round(W * dpr);
  if (cv.height !== Math.round(H * dpr)) cv.height = Math.round(H * dpr);
  const x = cv.getContext('2d'); x.setTransform(dpr, 0, 0, dpr, 0, 0); x.clearRect(0, 0, W, H);
  const cs = getComputedStyle(cv), v = k => cs.getPropertyValue(k).trim();
  const Z = 12, pad = { t: 14, b: 24 };
  const fx = f => (Math.log10(f) - LMIN) / LSPAN * W;
  const gy = db => pad.t + (1 - (db + Z) / (2 * Z)) * (H - pad.t - pad.b);
  G = { W, H, Z, pad, fx, gy, xf: px => Math.pow(10, LMIN + clamp(px / W, 0, 1) * LSPAN), yg: py => (1 - (py - pad.t) / (H - pad.t - pad.b)) * 2 * Z - Z };
  const g = grid(Math.round(W / 1.5));
  const trace = (r, close, skipFlat) => {
    x.beginPath(); let pen = false;
    for (let i = 0; i < g.n; i++) {
      if (skipFlat && Math.abs(r[i]) < .04) { pen = false; continue; }
      const px = fx(g.f[i]), py = gy(clamp(r[i], -Z - 30, Z + 30));
      pen ? x.lineTo(px, py) : x.moveTo(px, py); pen = true;
    }
    if (close) { x.lineTo(W, gy(0)); x.lineTo(0, gy(0)); x.closePath(); }
  };
  const band = a => `rgba(${v('--g-band')},${a})`;
  x.lineWidth = 1;
  for (let dec = 10; dec <= 10000; dec *= 10) for (let k = 1; k <= 9; k++) {
    const f = dec * k; if (f < 20 || f > 20000) continue;
    x.strokeStyle = k === 1 ? v('--g-major') : v('--g-grid');
    const px = Math.round(fx(f)) + .5; x.beginPath(); x.moveTo(px, 0); x.lineTo(px, H - pad.b); x.stroke();
  }
  for (let db = -Z; db <= Z; db += 6) {
    x.strokeStyle = db === 0 ? v('--g-zero') : v('--g-major');
    const py = Math.round(gy(db)) + .5; x.beginPath(); x.moveTo(0, py); x.lineTo(W, py); x.stroke();
  }
  x.font = '600 10.5px Manrope, sans-serif'; x.fillStyle = v('--g-label');
  x.textAlign = 'left'; x.textBaseline = 'bottom';
  [6, 0, -6].forEach(db => x.fillText(sgn(db) + Math.abs(db), 10, gy(db) - 4));
  x.textAlign = 'center'; x.textBaseline = 'middle';
  [[50, '50'], [100, '100'], [200, '200'], [500, '500'], [1000, '1k'], [2000, '2k'], [5000, '5k'], [10000, '10k']].forEach(([f, t]) => x.fillText(t, fx(f), H - pad.b / 2));

  const lay = L(), d = D();
  if (S.output) {
    // One line per shown channel, labeled with the device's own channel names.
    // Stereo always shows both; multichannel follows the layer's channels or a chosen set.
    const all = Array.from({ length: chCount(d) }, (_, c) => c);
    const chs = chCount(d) <= 2 ? all : S.outMode === 'layer' ? all.filter(c => targets(lay, c)) : all.filter(c => (S.outMask >>> c) & 1);
    const dashes = [[6, 4], [2, 3], [10, 3, 2, 3], [4, 2]];
    const ends = [];
    // Past four lines, labels would pile up, so lines get quieter and go unlabeled
    const many = chs.length > 4;
    chs.forEach((c, k) => {
      const r = output(LAYERS(), c, g);
      x.setLineDash(dashes[k % dashes.length]); x.strokeStyle = v('--g-out'); x.lineWidth = many ? 1 : 1.3;
      x.globalAlpha = many ? .6 : 1;
      trace(r, false); x.stroke(); x.setLineDash([]); x.globalAlpha = 1;
      if (many) return;
      let ly = gy(r[g.n - 1]) - 10;
      while (ends.some(e => Math.abs(e - ly) < 12)) ly += 12;
      ends.push(ly);
      x.font = '700 10.5px Manrope, sans-serif'; x.fillStyle = v('--g-out'); x.textAlign = 'right';
      x.fillText(longName(d, c), W - 12, ly);
    });
  }
  lay.filters.forEach((f, i) => { if (i !== S.band) { trace(one(f, g), true); x.fillStyle = band(f.on ? .06 : .02); x.fill(); } });
  const sf = lay.filters[S.band];
  if (sf) {
    const r = one(sf, g);
    trace(r, true); x.fillStyle = band(sf.on ? .13 : .04); x.fill();
    trace(r, false, true); x.lineWidth = 1.25; x.strokeStyle = band(.55); if (!sf.on) x.setLineDash([3, 3]); x.stroke(); x.setLineDash([]);
  }
  const layerResponse=lay.on ? resp(lay.filters,g) : new Float64Array(g.n); trace(layerResponse, false);
  x.lineWidth = 2; x.lineJoin = 'round'; x.strokeStyle = v('--g-curve'); x.stroke();

  nodes = [];
  x.textAlign = 'center'; x.textBaseline = 'middle'; x.font = '800 10px Manrope, sans-serif';
  const order = lay.filters.map((_, i) => i).sort((a, b) => (a === S.band) - (b === S.band));
  for (const i of order) {
    const f = lay.filters[i], sel = i === S.band;
    const db = hasGain(f.type) ? f.gain : (f.type === 'LPQ' || f.type === 'HPQ') ? 20 * Math.log10(f.q) : 0;
    const nx = clamp(fx(clamp(f.freq, 20, 20000)), 11, W - 11), ny = clamp(gy(clamp(db, -Z, Z)), pad.t + 2, H - pad.b - 11);
    nodes.push([i, nx, ny]);
    if (sel || i === hover) { x.beginPath(); x.arc(nx, ny, 13.5, 0, Math.PI * 2); x.lineWidth = 1.5; x.strokeStyle = band(sel ? .35 : .18); x.stroke(); }
    x.beginPath(); x.arc(nx, ny, 9, 0, Math.PI * 2);
    x.fillStyle = sel ? v('--hi') : v('--g-node'); x.fill();
    if (!sel) { x.lineWidth = 1.2; x.strokeStyle = v('--g-node-ring'); if (!f.on) x.setLineDash([2, 2]); x.stroke(); x.setLineDash([]); }
    x.fillStyle = sel ? v('--hi-ink') : v('--text');
    x.fillText(String(i + 1), nx, ny + .5);
  }
  tip();
}
function tip() {
  const t = $('#tip'); if (!t) return;
  const i = drag ? drag.i : hover, n = nodes.find(n => n[0] === i);
  if (i < 0 || !n || !L().filters[i]) { t.hidden = true; return; }
  const f = L().filters[i];
  t.innerHTML = `<span>${TYPE[f.type].name}</span><span>${show('freq', f.freq).join(' ')}</span>${hasGain(f.type) ? `<span>${show('gain', f.gain).join(' ')}</span>` : ''}<span>Q ${show('q', f.q)[0]}</span>`;
  t.style.left = clamp(n[1], 100, G.W - 100) + 'px'; t.style.top = Math.max(n[2], 44) + 'px'; t.hidden = false;
}
const hit = (px, py) => { let best = -1, bd = 14; nodes.forEach(([i, nx, ny]) => { const dd = Math.hypot(nx - px, ny - py); if (dd < bd) { bd = dd; best = i; } }); return best; };
function bindGraph() {
  const cv = $('#graph'); if (!cv) return;
  const pt = e => { const r = cv.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  cv.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    cv.focus({preventScroll:true});
    const [px, py] = pt(e), i = hit(px, py); if (i < 0) return;
    selectBand(i);
    const f = L().filters[i], n = nodes.find(n => n[0] === i);
    drag = { i, x0: px, y0: py, nx: n[1], ny: n[2], q0: f.q, moved: false };
    cv.setPointerCapture(e.pointerId); cv.style.cursor = 'grabbing';
  });
  cv.addEventListener('pointermove', e => {
    const [px, py] = pt(e);
    if (drag) {
      const k = e.shiftKey ? .25 : 1, dx = (px - drag.x0) * k, dy = (py - drag.y0) * k, f = L().filters[drag.i];
      if (!drag.moved) { if (Math.hypot(dx, dy) < 2) return; drag.moved = true; beginGesture(); }
      f.freq = roundF(clamp(G.xf(drag.nx + dx), 20, Math.min(20000,NYQ-1)));
      if (hasGain(f.type)) f.gain = r1(clamp(G.yg(drag.ny + dy), -G.Z, G.Z));
      else if (f.type === 'LPQ' || f.type === 'HPQ') f.q = rq(clamp(Math.pow(10, G.yg(drag.ny + dy) / 20), .05, 1000));
      else f.q = rq(clamp(drag.q0 * Math.pow(2, -dy / 40), .05, 1000));
      refresh(); return;
    }
    const h = hit(px, py);
    if (h !== hover) { hover = h; draw(); }
    cv.style.cursor = h >= 0 ? 'grab' : 'crosshair';
  });
  const end = () => { if (drag) { drag = null; endGesture(); cv.style.cursor = 'grab'; refreshChrome(); draw(); } };
  cv.addEventListener('pointerup', end); cv.addEventListener('pointercancel', end);
  cv.addEventListener('pointerleave', () => { if (!drag && hover >= 0) { hover = -1; draw(); } });
  cv.addEventListener('dblclick', e => {
    const [px, py] = pt(e); if (hit(px, py) >= 0) return;
    addBand(F('PK', roundF(G.xf(px)), r1(clamp(G.yg(py), -G.Z, G.Z)), 1));
  });
  cv.addEventListener('wheel', e => {
    const i = hover >= 0 ? hover : S.band; if (i < 0 || !L().filters[i]) return;
    e.preventDefault();
    pushNudge('wheelq' + i);
    const f = L().filters[i]; f.q = rq(clamp(f.q * Math.pow(e.shiftKey ? 1.02 : 1.08, e.deltaY < 0 ? 1 : -1), .05, 1000));
    if (i !== S.band) selectBand(i);
    refresh();
  }, { passive: false });
}


const grids=new Map();
// Display-only RBJ preview. Audio always uses the native, validated DSP.
// Native coefficients seed this cache after acknowledgement; local edits are
// drawn synchronously, so the point, band, total and sidebar share one frame.
const curveCache=new WeakMap();
const curveKey=(f,rate)=>[rate,f.type,f.freq,f.gain,f.q].join(':');
function rememberCurves(work,rate){
  work.layers.forEach(l=>l.filters.forEach(f=>curveCache.set(f,{key:curveKey(f,rate),sections:f.coefficients||[]})));
}
function designCurve(f,rate){
  if(f.freq<=0||f.freq>=rate/2)return [];
  if(hasGain(f.type)&&f.gain===0)return [];
  const w=2*Math.PI*f.freq/rate,c=Math.cos(w),alpha=Math.sin(w)/(2*f.q),a=10**(f.gain/40),beta=2*Math.sqrt(a)*alpha;
  let b0,b1,b2,a0=1+alpha,a1=-2*c,a2=1-alpha;
  switch(f.type){
    case 'PK':b0=1+alpha*a;b1=-2*c;b2=1-alpha*a;a0=1+alpha/a;a2=1-alpha/a;break;
    case 'LSQ':b0=a*((a+1)-(a-1)*c+beta);b1=2*a*((a-1)-(a+1)*c);b2=a*((a+1)-(a-1)*c-beta);a0=(a+1)+(a-1)*c+beta;a1=-2*((a-1)+(a+1)*c);a2=(a+1)+(a-1)*c-beta;break;
    case 'HSQ':b0=a*((a+1)+(a-1)*c+beta);b1=-2*a*((a-1)+(a+1)*c);b2=a*((a+1)+(a-1)*c-beta);a0=(a+1)-(a-1)*c+beta;a1=2*((a-1)-(a+1)*c);a2=(a+1)-(a-1)*c-beta;break;
    case 'LPQ':b0=(1-c)/2;b1=1-c;b2=b0;break;
    case 'HPQ':b0=(1+c)/2;b1=-(1+c);b2=b0;break;
    case 'NO':b0=1;b1=-2*c;b2=1;break;
    case 'AP':b0=1-alpha;b1=-2*c;b2=1+alpha;break;
    case 'BP':b0=alpha;b1=0;b2=-alpha;break;
    default:return [];
  }
  const section=[b0,b1,b2,a1,a2].map(v=>v/a0);
  return section.every(Number.isFinite)?[section]:[];
}
function curveCoefficients(f,rate=FS){
  const key=curveKey(f,rate),cached=curveCache.get(f);
  if(cached?.key===key)return cached.sections;
  const sections=designCurve(f,rate);curveCache.set(f,{key,sections});return sections;
}
function grid(n,rate=FS) {
  const span=Math.log10(Math.min(20000,rate/2-1))-LMIN;
  n=Math.max(2,n); const key=rate+':'+span+':'+n;
  if(grids.has(key)){const g=grids.get(key);grids.delete(key);grids.set(key,g);return g;}
  const g={n,rate,f:[],c1:[],s1:[],c2:[],s2:[]};
  for(let i=0;i<n;i++){ const f=10**(LMIN+span*i/(n-1)),w=2*Math.PI*f/rate;g.f.push(f);g.c1.push(Math.cos(w));g.s1.push(Math.sin(w));g.c2.push(Math.cos(2*w));g.s2.push(Math.sin(2*w)); }
  // A resize can visit hundreds of widths; do not retain a grid for each one.
  if(grids.size>=8)grids.delete(grids.keys().next().value);
  grids.set(key,g);return g;
}
function add(o,f,g) {
  for(const [b0,b1,b2,a1,a2] of curveCoefficients(f,g.rate)) for(let i=0;i<g.n;i++) {
    const nr=b0+b1*g.c1[i]+b2*g.c2[i],ni=-(b1*g.s1[i]+b2*g.s2[i]),dr=1+a1*g.c1[i]+a2*g.c2[i],di=-(a1*g.s1[i]+a2*g.s2[i]);
    o[i]+=10*Math.log10(Math.max(1e-30,(nr*nr+ni*ni)/(dr*dr+di*di)));
  }
}
const resp=(fs,g)=>{const o=new Float64Array(g.n);fs.forEach(f=>f.on&&add(o,f,g));return o;};
const one=(f,g)=>{const o=new Float64Array(g.n);add(o,f,g);return o;};
function output(layers,c,g){const o=new Float64Array(g.n);layers.forEach(l=>{if(l.on&&targets(l,c)){const r=resp(l.filters,g);for(let i=0;i<g.n;i++)o[i]+=r[i];}});return o;}

function profileCurvePreview(work,channels,rate){
  const g=grid(192,rate),paths=new Set();
  const layers=work.layers.filter(l=>l.on).map(layer=>({layer,response:resp(layer.filters,g)}));
  for(let c=0;c<channels;c++){
    const response=new Float64Array(g.n);
    for(const {layer,response:r} of layers)if(targets(layer,c))for(let i=0;i<g.n;i++)response[i]+=r[i];
    // Identical visible responses share a line, including flat channels.
    paths.add(pathOf(response,g,300,68,24));
  }
  const opacity=Math.max(.24,.8/Math.sqrt(paths.size)).toFixed(2);
  return `<svg viewBox="0 0 300 68" preserveAspectRatio="none" role="img" aria-label="Combined filter response${paths.size>1?'s':''}; layer gain excluded"><path class="profile-zero" d="M0 34H300"/>${[...paths].map(d=>`<path class="profile-curve" d="${d}" opacity="${opacity}"/>`).join('')}</svg>`;
}
