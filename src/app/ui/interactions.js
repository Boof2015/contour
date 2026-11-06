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
document.addEventListener('focusin', e => { if (e.target.matches('.win .fld input')) e.target.dataset.orig = e.target.value;const row=e.target.closest?.('[data-band]');if(row)selectBand(+row.dataset.band); });
document.addEventListener('focusout', e => {
  const el = e.target; if (!el.matches || !el.matches('.win .fld input')) return;
  if (el.dataset.skip) { delete el.dataset.skip; refresh(false); return; }
  if (el.value === el.dataset.orig) return;
  const v = parse(el.dataset.k, el.value, el.dataset.unit), fld = el.closest('.fld');
  if (v === null) { fld.classList.add('bad'); setTimeout(() => { fld.classList.remove('bad'); refresh(); }, 700); return; }
  push(); set(el.dataset.k, +el.dataset.i, v); el.dataset.orig=el.value; refresh();
});
document.addEventListener('keydown', e => {
  if(e.defaultPrevented)return;
  const el = e.target, typing = (el.matches && el.matches('input, select, textarea'))||el.isContentEditable;
  if(layerDrag){if(e.key==='Escape'){e.preventDefault();cancelLayerDrag();}return;}
  if(S.pop==='filtertype'){
    const options=$$('#pop [role="option"]'),index=options.indexOf(document.activeElement);
    let next=-1;
    if(e.key==='ArrowDown')next=(index+1)%options.length;
    if(e.key==='ArrowUp')next=(index-1+options.length)%options.length;
    if(e.key==='Home')next=0;if(e.key==='End')next=options.length-1;
    if(next>=0){e.preventDefault();options.forEach((x,i)=>x.tabIndex=i===next?0:-1);options[next]?.focus();return;}
    if(e.key==='Tab'){closePopover(true);return;}
    if(e.key.length===1&&!e.ctrlKey&&!e.metaKey&&e.key!==' '){const found=[...options.slice(index+1),...options.slice(0,index+1)].find(x=>x.textContent.trim().toLowerCase().startsWith(e.key.toLowerCase()));if(found){e.preventDefault();found.focus();}return;}
  }
  if (e.key === 'Escape' && S.pop) { e.preventDefault();closePopover(true);return; }
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
  if(el.matches?.('[data-type]')&&(e.key==='ArrowDown'||e.key==='ArrowUp')){e.preventDefault();openFilterType(+el.dataset.type);return;}
  if(e.key==='Delete'&&!mod&&!e.altKey&&!e.shiftKey&&!e.repeat&&!S.pop&&!drag&&(el===document.body||el.closest?.('.main'))){e.preventDefault();removeFilter();return;}
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
// Pointer capture keeps the drag alive across cards; only the final drop edits
// the profile. Reordering the preview DOM never publishes intermediate orders.
function cancelLayerDrag(){if(layerDrag)finishLayerDrag(false);}
function finishLayerDrag(commit){
  const d=layerDrag;if(!d)return;
  layerDrag=null;cancelAnimationFrame(d.frame);
  d.list.removeEventListener('pointermove',d.move);d.list.removeEventListener('pointerup',d.up);d.list.removeEventListener('pointercancel',d.cancel);d.list.removeEventListener('lostpointercapture',d.cancel);
  if(d.list.hasPointerCapture(d.id))d.list.releasePointerCapture(d.id);
  document.body.classList.remove('layer-dragging');
  if(!d.active){if(commit){S.sel=d.from;S.band=0;render();$(`[data-layer="${d.from}"]`)?.focus({preventScroll:true});}return;}
  ignoreLayerClickUntil=Date.now()+250;
  const positions=captureLayerPositions(),to=[...d.list.children].indexOf(d.card);
  if(d.ghost)positions.set(LAYERS()[d.from],d.ghost.getBoundingClientRect());
  d.ghost?.remove();d.card.classList.remove('drag-placeholder');
  if(commit&&to!==d.from){moveLayer(d.from,to,positions);$(`[data-layer="${to}"]`)?.focus({preventScroll:true});}
  else{d.original.forEach(node=>d.list.append(node));animateLayerPositions(positions);d.card.focus({preventScroll:true});}
}
document.addEventListener('pointerdown',e=>{
  const card=e.target.closest('.layers [data-layer]');
  if(!card||e.button!==0||e.target.closest('button,input')||S.modal||S.pop||layerDrag)return;
  const list=card.parentElement,rect=card.getBoundingClientRect();
  const d=layerDrag={list,card,from:+card.dataset.layer,id:e.pointerId,original:[...list.children],x:e.clientX,y:e.clientY,startX:e.clientX,startY:e.clientY,offset:e.clientY-rect.top,active:false,ghost:null,frame:0};
  const update=()=>{
    if(layerDrag!==d||!d.active)return;
    d.ghost.style.top=d.y-d.offset+'px';
    const listTop=list.getBoundingClientRect().top;
    const others=[...list.children].filter(n=>n!==card),before=others.find(n=>d.y<listTop+n.offsetTop-list.scrollTop+n.offsetHeight/2)||null;
    if(card.nextElementSibling!==before){const positions=captureLayerPositions();list.insertBefore(card,before);animateLayerPositions(positions);}
  };
  const tick=()=>{
    if(layerDrag!==d||!d.active)return;
    const r=list.getBoundingClientRect(),speed=d.y<r.top+32?-Math.min(14,(r.top+32-d.y)/3):d.y>r.bottom-32?Math.min(14,(d.y-r.bottom+32)/3):0;
    if(speed){list.scrollTop+=speed;update();}
    d.frame=requestAnimationFrame(tick);
  };
  d.move=ev=>{
    d.x=ev.clientX;d.y=ev.clientY;
    if(!d.active){
      if(Math.hypot(d.x-d.startX,d.y-d.startY)<6)return;
      d.active=true;list.setPointerCapture(d.id);document.body.classList.add('layer-dragging');
      d.ghost=card.cloneNode(true);d.ghost.className+=' layer-drag-ghost';d.ghost.removeAttribute('tabindex');d.ghost.setAttribute('aria-hidden','true');
      Object.assign(d.ghost.style,{left:rect.left+'px',width:rect.width+'px',height:rect.height+'px',top:rect.top+'px'});document.body.append(d.ghost);card.classList.add('drag-placeholder');d.frame=requestAnimationFrame(tick);
    }
    ev.preventDefault();update();
  };
  d.up=()=>finishLayerDrag(true);d.cancel=()=>finishLayerDrag(false);
  list.addEventListener('pointermove',d.move);list.addEventListener('pointerup',d.up);list.addEventListener('pointercancel',d.cancel);list.addEventListener('lostpointercapture',d.cancel);
  // Capture from pointerdown so releasing outside the list also ends a click
  // candidate that never crossed the drag threshold.
  list.setPointerCapture(d.id);
});
window.addEventListener('blur',cancelLayerDrag);

