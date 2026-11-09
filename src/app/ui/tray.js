// A view/action relay only. The editor owns the draft, command ordering,
// history, dirty decisions and all communication with the native model.
let state=null,busy=false,markup='',lastFocus=null;
const post=data=>window.chrome.webview.postMessage(JSON.stringify(data));
const root=document.getElementById('quick');
function renderTray(){
  if(!state)return;
  document.documentElement.dataset.theme=state.theme;
  const disabled=busy||state.busy,profile=state.profile;
  const outputs=state.devices.map(d=>`<button class="opt${d.id===state.device?' on':''}" data-action="device" data-id="${esc(d.id)}" role="radio" aria-checked="${d.id===state.device}" ${disabled||!d.available?'disabled':''}><span class="rd"></span><span class="tray-name">${esc(d.name)}</span><small>${d.available?(d.channels===2?'Stereo':d.channels+' ch'):'Disconnected'}</small></button>`).join('');
  const profiles=state.profiles.map(p=>`<button class="opt${p.id===profile?.id?' on':''}" data-action="profile" data-id="${esc(p.id)}" role="radio" aria-checked="${p.id===profile?.id}" ${disabled||state.gated?'disabled':''}><span class="rd"></span><span class="tray-name">${esc(p.name)}</span>${p.id===profile?.id&&profile.dirty?'<span class="dot"></span>':''}</button>`).join('');
  const layers=(profile?.layers||[]).map((l,i)=>`<button class="lyr${l.on?'':' off'}" data-action="layer" data-layer="${i}" aria-pressed="${l.on}" ${disabled||state.gated?'disabled':''}><span class="tray-name">${esc(l.name)}</span><small>${esc(l.target)}</small><span class="sw sm${l.on?' on':''}" aria-hidden="true"></span></button>`).join('');
  const html=`<div class="tray-h">${WORDMARK}<button class="sw${state.eqEnabled?' on':''}" data-action="eq" title="EQ on or off for every device" aria-label="Enable EQ globally" aria-pressed="${state.eqEnabled}" ${disabled?'disabled':''}></button></div><div class="tray-body"><div class="sec" id="outputsLabel">Output</div><div role="radiogroup" aria-labelledby="outputsLabel">${outputs||'<p class="tray-empty">No installed outputs. Open Contour to install it on a device.</p>'}</div>${profiles?`<div class="sec" id="profilesLabel">Profile</div><div role="radiogroup" aria-labelledby="profilesLabel">${profiles}</div>`:''}${layers?`<div class="sec">Layers<span class="tray-name">${esc(profile.name)}</span></div>${layers}`:''}${state.note?`<p class="tray-empty" role="status">${esc(state.note)}</p>`:''}</div>${profile?.dirty?`<div class="unsaved"><span>Unsaved changes</span><button class="lnk" data-action="revert" ${disabled||state.gated?'disabled':''}>Revert</button><button class="sbtn" data-action="save" ${disabled||state.gated?'disabled':''}>Save</button></div>`:''}<div class="tray-ft"><button data-action="open-editor">Open Contour</button></div>`;
  if(html!==markup){
    const active=document.activeElement?.dataset,scroll=root.querySelector('.tray-body')?.scrollTop||0;
    if(active?.action)lastFocus={...active};
    const focus=lastFocus;
    root.innerHTML=html;markup=html;root.querySelector('.tray-body').scrollTop=scroll;
    if(focus?.action)[...root.querySelectorAll('button')].find(b=>b.dataset.action===focus.action&&b.dataset.id===focus.id&&b.dataset.layer===focus.layer)?.focus({preventScroll:true});
  }
  const body=root.querySelector('.tray-body');
  post({action:'layout',height:Math.min(640,Math.ceil(root.getBoundingClientRect().height+body.scrollHeight-body.clientHeight))});
}
root.addEventListener('click',e=>{
  const b=e.target.closest('button[data-action]');if(!b||b.disabled)return;
  const action=b.dataset.action;
  if(action!=='open-editor'){busy=true;renderTray();}
  post({action,id:b.dataset.id,layer:b.dataset.layer===undefined?undefined:+b.dataset.layer,device:state.device,profile:state.profile?.id,revision:state.profile?.revision});
});
document.addEventListener('keydown',e=>{
  if(e.key==='Escape'){e.preventDefault();post({action:'dismiss'});return;}
  if(e.key==='Tab'){
    const buttons=[...root.querySelectorAll('button:not(:disabled)')],index=buttons.indexOf(document.activeElement);
    if((e.shiftKey&&index<=0)||(!e.shiftKey&&index===buttons.length-1)){e.preventDefault();(e.shiftKey?buttons.at(-1):buttons[0])?.focus();}
  }
  if(['ArrowDown','ArrowUp','Home','End'].includes(e.key)&&e.target.matches('[role="radio"]')){
    const buttons=[...e.target.parentElement.querySelectorAll('button:not(:disabled)')],index=buttons.indexOf(e.target);
    const next=e.key==='Home'?0:e.key==='End'?buttons.length-1:(index+(e.key==='ArrowDown'?1:-1)+buttons.length)%buttons.length;
    e.preventDefault();buttons[next]?.focus();
  }
});
window.chrome.webview.addEventListener('message',e=>{state=e.data;busy=false;renderTray();});
window.addEventListener('resize',renderTray);
post({action:'ready'});
