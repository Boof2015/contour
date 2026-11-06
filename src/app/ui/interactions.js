document.addEventListener('pointerdown', e => {
  const fld = e.target.closest('.win .fld'); if (!fld || fld.classList.contains('dis') || e.button !== 0) return;
  const el = fld.querySelector('input'); if (el === document.activeElement) return;
  e.preventDefault();
  const k = el.dataset.k, i = +el.dataset.i;
  if (k !== 'lgain' && i !== S.band) selectBand(i);
  const start = get(k, i), x0 = e.clientX; let moved = false;
  fld.setPointerCapture(e.pointerId);
  const mv = ev => {
    const dx = (ev.clientX - x0) * (ev.shiftKey ? .2 : 1);
    if (!moved && Math.abs(dx) < 3) return;
    if (!moved) beginGesture();
    moved = true; fld.classList.add('scrub'); document.body.style.cursor = 'ew-resize';
    set(k, i, scrub(k, start, dx)); refresh();
  };
  const up = () => {
    fld.removeEventListener('pointermove', mv); fld.removeEventListener('pointerup', up); fld.removeEventListener('pointercancel', up);
    fld.classList.remove('scrub'); document.body.style.cursor = '';
    if (moved) { endGesture(); refreshChrome(); } else { el.focus(); el.select(); }
  };
  fld.addEventListener('pointermove', mv); fld.addEventListener('pointerup', up); fld.addEventListener('pointercancel', up);
});
document.addEventListener('focusin', e => { if (e.target.matches('.win .fld input')) e.target.dataset.orig = e.target.value; });
document.addEventListener('focusout', e => {
  const el = e.target; if (!el.matches || !el.matches('.win .fld input')) return;
  if (el.dataset.skip) { delete el.dataset.skip; refresh(false); return; }
  if (el.value === el.dataset.orig) return;
  const v = parse(el.dataset.k, el.value, el.dataset.unit), fld = el.closest('.fld');
  if (v === null) { fld.classList.add('bad'); setTimeout(() => { fld.classList.remove('bad'); refresh(); }, 700); return; }
  push(); set(el.dataset.k, +el.dataset.i, v); el.dataset.orig=el.value; refresh();
});
document.addEventListener('keydown', e => {
  const el = e.target, typing = el.matches && el.matches('input, select, textarea');
  if (e.key === 'Escape' && S.pop) { S.pop = null; S.popAt = null; render(); return; }
  if (S.modal) {
    if (e.key === 'Escape') modalAct('cancel');
    if (e.key === 'Enter' && el.id === 'dlgIn') { modalAct('go'); }
    return;
  }
  if (e.key === 'Escape' && S.tray) { S.tray = false; render(); return; }
  if (el.id === 'lnIn' && (e.key === 'Enter' || e.key === 'Escape')) { if (e.key === 'Escape') { el.value = L().name; } el.blur(); return; }
  if (el.matches && el.matches('.fld input')) {
    if (e.key === 'Enter') { el.blur(); return; }
    if (e.key === 'Escape') { el.dataset.skip = '1'; el.blur(); return; }
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      e.preventDefault();
      const k = el.dataset.k, i = +el.dataset.i;
      pushNudge(k + i);
      set(k, i, step(k, get(k, i), e.key === 'ArrowUp' ? 1 : -1, e.shiftKey));
      const [v, u] = show(k, get(k, i)); el.value = v; el.dataset.unit = u; el.dataset.orig = v; el.nextElementSibling.textContent = u;
      refresh();
      return;
    }
  }
  const mod = e.ctrlKey || e.metaKey;
  if (mod && e.key.toLowerCase() === 's') { e.preventDefault(); el.blur?.(); endGesture(); if (P()) command('save'); return; }
  if (typing || S.view !== 'editor') return;
  if (mod && e.key.toLowerCase() === 'z' && !e.shiftKey) { e.preventDefault(); undo(); return; }
  if (mod && (e.key.toLowerCase() === 'y' || (e.key.toLowerCase() === 'z' && e.shiftKey))) { e.preventDefault(); redo(); }
});
document.addEventListener('wheel', e => {
  const fld = e.target.closest('.win .fld'); if (!fld || fld.classList.contains('dis')) return;
  e.preventDefault();
  const el = fld.querySelector('input'), k = el.dataset.k, i = +el.dataset.i;
  pushNudge(k + i);
  set(k, i, step(k, get(k, i), e.deltaY < 0 ? 1 : -1, e.shiftKey));
  if (el === document.activeElement) { const [v, u] = show(k, get(k, i)); el.value = v; el.dataset.orig = v; el.nextElementSibling.textContent = u; }
  refresh();
}, { passive: false });
document.addEventListener('change', e => {
  const s = e.target.closest('[data-type]'); if (!s) return;
  const i = +s.dataset.type;
  edit(() => {
    const f = L().filters[i], t = s.value;
    if (!hasGain(t) && hasGain(f.type)) { f._gain = f.gain; f.gain = 0; }
    if (hasGain(t) && !hasGain(f.type)) f.gain = f._gain ?? 0;
    f.type = t; S.band = i;
  });
});

