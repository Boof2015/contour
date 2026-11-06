// The host owns profiles, history, persistence, validation and audio publication.
// Local changes provide immediate visual feedback until acknowledged.
let APP=null, sequence=0, serial=Promise.resolve(), generation=0, pendingEdit=false, editTimer=0, editFlight=null;
const replies=new Map();
let WINDOW={custom:false,maximized:false};
let SETUP={state:'idle',busy:false};
let layerDrag=null,ignoreLayerClickUntil=0;
function windowButtons(){return `<div class="caption"><button data-window="minimize" aria-label="Minimize" title="Minimize"><svg viewBox="0 0 10 10"><path d="M0 5.5h10"/></svg></button><button data-window="maximize" id="maximize" aria-label="${WINDOW.maximized?'Restore':'Maximize'}" title="${WINDOW.maximized?'Restore':'Maximize'}"><svg viewBox="0 0 10 10">${WINDOW.maximized?'<path d="M2.5 2.5v-2h7v7h-2M.5 2.5h7v7h-7z"/>':'<path d="M.5.5h9v9h-9z"/>'}</svg></button><button data-window="close" aria-label="Close window" title="Close"><svg viewBox="0 0 10 10"><path d="m.5.5 9 9m0-9-9 9"/></svg></button></div>`;}
function windowState(data){
  if(WINDOW.custom===data.custom&&WINDOW.maximized===data.maximized)return;
  WINDOW=data;document.documentElement.dataset.customFrame=data.custom;document.documentElement.dataset.maximized=data.maximized;
  const bar=$('.caption');if(bar)bar.outerHTML=windowButtons();
}
function syncWindowTheme(){rpc('window',{action:'theme',theme:S.theme}).catch(e=>toast(e.message));}
const S={settingsTab:'outputs',settingsDevice:null,setupConfirm:null,showDisconnected:false,openEditorDevice:null,theme:localStorage.getItem('theme')||'dark',sel:0,band:0,output:false,outMode:'layer',outMask:0,view:'editor',pop:null,modal:null,tray:false};
const P=()=>APP?.editor;
const D=()=>{
  const d=APP.devices.find(x=>x.id===APP.device), positions=Object.keys(POS);
  const pos=positions.filter((_,i)=>(d.mask>>>i)&1);
  return {...d,count:d.channels,pos:pos.length===d.channels?pos:null};
};
const LAYERS=()=>P()?.work?.layers||[];
const L=()=>LAYERS()[S.sel];
const dirty=p=>!!p?.dirty;
function fixSel(){S.sel=clamp(S.sel,0,Math.max(0,LAYERS().length-1));S.band=clamp(S.band,0,Math.max(0,(L()?.filters.length||0)-1));}
function rawWork(){return JSON.parse(JSON.stringify(P().work,(k,v)=>k==='coefficients'||k==='_gain'?undefined:v));}
function rpc(op,args={}) {
  const run=()=>new Promise((resolve,reject)=>{
    // A command may have its own profile/device `id`. Keep reply correlation
    // separate, and never let command arguments replace the envelope fields.
    const requestId=++sequence; replies.set(requestId,{resolve,reject});
    window.chrome.webview.postMessage(JSON.stringify({...args,op,requestId}));
  });
  const promise=serial.then(run); serial=promise.catch(()=>{}); return promise;
}
function adopt(data,document=true){
  const old=P(); APP={...APP,...data};
  if(data.editor && !data.editor.work && old?.id===data.editor.id) APP.editor={...old,...data.editor};
  if((!document||layerDrag) && old) APP.editor=old;
  if(P()?.work){ FS=P().curveRate||P().rate;NYQ=FS/2;LSPAN=Math.log10(Math.min(20000,NYQ-1))-LMIN; }
  if(document&&data.editor?.work)rememberCurves(P().work,FS);
  fixSel();
}
function queueEdit(){
  if(!P())return; generation++;pendingEdit=true;P().dirty=true;P().undo=true;
  // Throttle, rather than debounce: a continuous drag must reach the engine.
  if(!editTimer)editTimer=setTimeout(()=>flush().catch(()=>{}),25);refreshChrome();
}
async function flush(){
  clearTimeout(editTimer);editTimer=0;
  // Keep at most one native edit in flight; coalesce to the newest draft.
  while(editFlight)await editFlight;
  if(!pendingEdit)return serial;
  pendingEdit=false;const ticket=generation,work=rawWork();
  editFlight=(async()=>{try{
    const data=await rpc('edit',{work});
    if(ticket===generation&&!pendingEdit){adopt(data);refresh(false);}else{APP.status=data.status;statusbar();}
  }catch(error){
    pendingEdit=false;clearTimeout(editTimer);editTimer=0;generation++;
    if(error.state)adopt(error.state);render();toast(error.message);throw error;
  }})();
  try{await editFlight;}finally{editFlight=null;}
  if(pendingEdit)return flush();
}
async function command(op,args={},quiet=false){
  try{
    await flush();const data=await rpc(op,args);adopt(data);fixSel();render();handleClose();
    if(!quiet&&op==='save')toast(APP.status.copy==='current'?'Profile saved':'Profile saved · device update pending');
    return data;
  }catch(error){if(error.state)adopt(error.state);render();toast(error.message);return null;}
}
let gesture=false,nudgeTimer=0;
function push(){} // History is recorded by EditSession::replace, not the web view.
function beginGesture(){if(!gesture){gesture=true;rpc('begin').catch(e=>toast(e.message));}}
function endGesture(){if(gesture){gesture=false;flush().then(()=>rpc('end')).catch(e=>toast(e.message));}}
function pushNudge(){beginGesture();clearTimeout(nudgeTimer);nudgeTimer=setTimeout(endGesture,300);}
function undo(){endGesture();command('undo');}
function redo(){endGesture();command('redo');}
function edit(fn){flush().catch(()=>{});fn();fixSel();queueEdit();render();}
function addBand(f){if(!L()||L().filters.length>=MAXF)return;edit(()=>{L().filters.push(f);S.band=L().filters.length-1;});}
function selectBand(i){S.band=i;$$('[data-band]').forEach(el=>el.classList.toggle('sel',+el.dataset.band===i));draw();}
function refreshChrome(){
  if(!APP)return;const pb=$('#pbox');if(pb)pb.innerHTML=profileBox();
  const ub=$('#undoBox');if(ub)ub.innerHTML=undoBox();statusbar();
}
function refresh(changed=true){
  if(changed)queueEdit();const l=L();if(!l)return;
  $$('.fld input').forEach(el=>{
    if(el===document.activeElement||!el.dataset.k)return;
    const k=el.dataset.k,i=+el.dataset.i,f=l.filters[i];
    if(k!=='lgain'&&(!f||(k==='gain'&&!hasGain(f.type))))return;
    const [v,u]=show(k,k==='lgain'?l.gain:f[k]);el.value=v;el.dataset.unit=u;el.nextElementSibling.textContent=u;
  });
  const pv=$(`.layers li[data-layer="${S.sel}"] .pv`);if(pv)pv.outerHTML=preview(l,true);
  refreshChrome();draw();
}
function profileBox(){
  const p=P();if(!p)return '';
  return `<button class="pbtn" id="profileBtn" title="Switch profile"><span class="pn">${esc(p.name)}</span><span class="pl">${p.channels===2?'Stereo':p.channels+' channels'}</span>${p.dirty?'<span class="dot" title="Unsaved changes"></span>':''}${I('chev')}</button>
    ${p.dirty?'<div class="dirty"><span>Unsaved changes</span><button class="lnk" data-act="revert">Revert</button><button class="sbtn" data-act="save">Save</button></div>':''}`;
}
function undoBox(){return P()?`<button class="ib" data-act="undo" title="Undo (Ctrl+Z)" aria-label="Undo" ${P().undo?'':'disabled'}>${I('undo')}</button><button class="ib" data-act="redo" title="Redo (Ctrl+Y)" aria-label="Redo" ${P().redo?'':'disabled'}>${I('redo')}</button>`:'';}
function titlebar(){return `<div class="tb"><div class="brand">${MARK}contour</div>${S.view==='settings'?'<span class="settings-caption">Settings</span>':`<button class="chip" id="devBtn">${I('speaker')}<span class="device-name">${esc(D().name)}</span>${I('chev')}</button>`}<span class="grow"></span>${S.view==='editor'?`<span id="undoBox" style="display:flex;gap:2px">${undoBox()}</span><button class="ib" data-act="settings" title="Settings" aria-label="Settings">${I('settings')}</button>`:''}<span class="power">EQ<button class="sw${APP.eqEnabled===false?'':' on'}" data-act="eq" aria-label="Enable EQ globally" aria-pressed="${APP.eqEnabled!==false}" ${setupOperation?'disabled':''}></button></span>${windowButtons()}</div>`;}
// Fast preview acknowledgements are diagnostics, not separate user-facing
// connection states. Only a settled mismatch becomes a visible warning.
let statusIssue='',statusIssueSince=0,statusIssueTimer=0;
function statusSummary(s,p,editing,now=Date.now()){
  if(s.endpoint!=='ready'){statusIssue='';clearTimeout(statusIssueTimer);}
  if(APP.masterIssues?.length)return {label:'EQ change pending',kind:'warn'};
  if(s.endpoint==='not_attached')return {label:'Not installed',kind:''};
  if(s.endpoint==='waiting_for_audio'){
    const connection=D().connection;
    if(s.copy==='failed')return {label:'EQ change pending',kind:'warn'};
    if(connection?.detail)return {label:'Check connection',kind:'warn'};
    if(s.pending||!connection?.confirmed)return {label:'Connecting',kind:''};
    return {label:APP.eqEnabled===false?'EQ bypassed':'Ready',kind:'live'};
  }
  if(s.endpoint==='unavailable')return {label:p?'Output disconnected':'Choose a profile',kind:''};
  if(s.endpoint==='incompatible')return {label:'Check channel layout',kind:'warn'};
  if(s.endpoint==='error')return {label:'Output needs attention',kind:'warn'};
  if(s.endpoint!=='ready')return {label:p?'Editing offline':'Choose a profile',kind:''};
  const issue=!editing&&(s.copy==='failed'||s.previewDetail||(s.pending&&D().connection?.detail)||(['loaded_difference','processed_difference','unknown'].includes(s.running)&&!s.applied?'EQ differs':''));
  const key=issue?`${APP.device}:${p?.id}:${issue}`:'';
  if(key!==statusIssue){statusIssue=key;statusIssueSince=now;clearTimeout(statusIssueTimer);if(key)statusIssueTimer=setTimeout(statusbar,1250);}
  if(key&&now-statusIssueSince>=1200)return {label:'EQ needs attention',kind:'warn'};
  return {label:APP.eqEnabled===false&&(s.applied||s.running==='processed_match')?'EQ bypassed':'Connected',kind:'live'};
}
function statusbar(){
  const el=$('#statusbar');if(!el||!APP)return;const s=APP.status;
  const {label,kind}=statusSummary(s,P(),pendingEdit||gesture||!!editFlight||!!layerDrag);
  const copy=P()?(P().dirty?'Unsaved changes':s.copy==='current'||s.saved==='matches'?'Saved':'Saved locally'):'';
  if(!el.querySelector('.status-label'))el.innerHTML='<span class="indicator"></span><span class="status-label" role="status" aria-live="polite"></span><span class="status-copy"></span><span class="status-format"></span>';
  const text=(selector,value)=>{const node=el.querySelector(selector);if(node.textContent!==value)node.textContent=value;};
  text('.status-label',label);text('.status-copy',copy);text('.status-format',P()?`${P().channels} ch · ${(P().curveRate||P().rate)/1000} kHz`:'');
  el.querySelector('.indicator').className='indicator '+kind;
  el.title=label;
}
function outputNotice(){return P()&&APP.status.endpoint==='not_attached'&&D().available?'<div class="output-notice"><span>Contour is not installed on this device.</span><button class="settings-button" data-act="setup">Manage device</button></div>':'';}
function welcome(){return `<div class="body"><aside class="side"><div class="side-h">Profiles</div>${APP.profiles.filter(p=>p.channels===D().channels).map(p=>`<button class="mi" data-prof="${esc(p.id)}">${esc(p.name)}</button>`).join('')}<span class="grow"></span><button class="prim" data-act="new-profile">New profile</button></aside><main class="welcome"><h2>No profile selected</h2><p>Choose a profile or create one for ${esc(D().name)}.</p><button class="prim" data-act="new-profile">Create a profile</button></main></div>`;}
let renderedProfile='';
function render(){
  if(!APP)return;document.documentElement.dataset.theme=S.theme;
  if(S.openEditorDevice===APP.device&&APP.closing?.state==='open'){S.openEditorDevice=null;S.view='editor';}
  if(layerDrag)cancelLayerDrag();
  const key=APP.device+':'+P()?.id,keep=key===renderedProfile,layerScroll=keep?$('.layers')?.scrollTop||0:0,bandScroll=keep?$('#bands')?.scrollTop||0:0;
  renderedProfile=key;
  $('#win').innerHTML='<div class="resize-top" data-resize="resize-top" aria-hidden="true"><i data-resize="resize-top-left"></i><i data-resize="resize-top-right"></i></div>'+titlebar()+(S.view==='settings'?settingsView():outputNotice()+(P()?editorView():welcome()))+'<div class="statusbar" id="statusbar" aria-label="Output status"></div>';
  if($('.layers'))$('.layers').scrollTop=layerScroll;if($('#bands'))$('#bands').scrollTop=bandScroll;
  // Add keyboard semantics to switches inherited from the approved drawing.
  $$('.sw:not(button)').forEach(el=>{el.role='switch';el.tabIndex=0;el.setAttribute('aria-checked',el.classList.contains('on'));el.setAttribute('aria-label','Enable filter');});
  bindGraph();statusbar();renderOver();draw();updateSettingsStatus();
}
function popHtml(){
  const mi=(act,label,icon='',disabled=false)=>`<button class="mi" data-act="${act}" ${disabled?'disabled':''}><span class="ck">${icon?I(icon):''}</span>${label}</button>`;
  if(S.pop==='filtertype')return `<div role="listbox" aria-label="Filter type">${TYPES.map(([id,name])=>`<button class="mi type-option" role="option" aria-selected="${L()?.filters[S.typeBand]?.type===id}" tabindex="${L()?.filters[S.typeBand]?.type===id?0:-1}" data-filter-type="${id}"><span class="ck">${L()?.filters[S.typeBand]?.type===id?I('check'):''}</span>${name}</button>`).join('')}</div>`;
  if(S.pop==='devices')return '<div class="grp">Windows outputs</div>'+APP.devices.filter(d=>d.available||d.id===APP.device).map(d=>`<button class="mi" data-device="${esc(d.id)}"><span class="ck">${d.id===APP.device?I('check'):''}</span><span class="two">${esc(d.name)}<small>${d.channels} channels · ${d.rate/1000} kHz</small></span></button>`).join('')+'<div class="divide"></div>'+mi('setup','Manage devices…','settings');
  if(S.pop==='profiles')return '<div class="grp">Profiles</div>'+APP.profiles.map(p=>`<button class="mi" data-prof="${esc(p.id)}" ${p.channels!==D().channels?'disabled':''}><span class="ck">${p.id===P()?.id?I('check'):''}</span>${esc(p.name)}<span class="meta">${p.channels} ch</span></button>`).join('')+'<div class="divide"></div>'+mi('save','Save','save',!P()?.dirty)+mi('save-as','Save as new profile…')+mi('duplicate-profile','Duplicate profile','copy')+mi('rename-profile','Rename…','pen')+mi('new-profile','New profile…','plus');
  if(S.pop==='channels')return channelPicker(D(),L());
  if(S.pop==='impch')return channelPicker(D(),S.modal);
  if(S.pop==='layermenu')return mi('dup-layer','Duplicate','copy',LAYERS().length>=MAXL)+mi('import-into','Replace with imported filters…','import')+'<div class="divide"></div>'+mi('layer-up','Move up','up',S.sel===0)+mi('layer-down','Move down','down',S.sel===LAYERS().length-1)+'<div class="divide"></div>'+mi('del-layer','Delete layer','trash');
  if(S.pop==='outch')return `<div class="grp">Combined filter channels</div><button class="mi" data-outmode="layer">This layer's channels</button><button class="mi" data-outmode="pick">Choose channels</button><div class="grid12">${Array.from({length:D().channels},(_,i)=>`<button class="spk${((S.outMask>>>i)&1)?' on':''}" data-outspk="${i}">${shortName(D(),i)}</button>`).join('')}</div>`;
  return '';
}
function dialog(title,body,buttons,input,on){S.pop=null;S.modal={title,body,buttons,input,on};renderOver();}
let shownModal=null,shownModalHtml='',shownPopover='';
function popAnchor(){return S.pop==='filtertype'?`#type-${S.typeBand}`:'#'+({devices:'devBtn',profiles:'profileBtn',channels:'chBtn',layermenu:'layerMenuBtn',outch:'outBtn',impch:'impChBtn'}[S.pop]);}
function positionPopover(){
  const pop=$('#pop'),anchor=$(popAnchor());if(!pop||!anchor)return;
  const rect=anchor.getBoundingClientRect(),r=pop.getBoundingClientRect();
  const below=rect.bottom+6,top=below+r.height<=innerHeight-8?below:rect.top-r.height-6;
  pop.style.left=clamp(rect.left,8,Math.max(8,innerWidth-r.width-8))+'px';
  pop.style.top=clamp(top,8,Math.max(8,innerHeight-r.height-8))+'px';
}
function closePopover(restore=false){const anchor=$(popAnchor());S.pop=null;renderOver();if(restore)anchor?.focus({preventScroll:true});}
function renderOver(){
  const over=$('#over');
  const m=S.modal;
  if(m!==shownModal){$('#modalHost')?.remove();shownModal=m;shownModalHtml='';}
  if(m){
    let host=$('#modalHost');const opening=!host;
    if(opening){host=document.createElement('div');host.id='modalHost';over.prepend(host);}
    // An open import owns live textarea/file/canvas nodes. Channel choices and
    // nested menus must never replace them or replay the dialog's animation.
    const html=m.kind==='import'&&!opening?shownModalHtml:m.kind==='import'?importHtml(m):`<div class="scrim"><div class="dlg" role="dialog" aria-modal="true" aria-labelledby="dialogTitle"><div class="dh"><h3 id="dialogTitle">${esc(m.title)}</h3><p class="status-detail">${esc(m.body||'')}</p>${m.input!==undefined?`<input class="txt" id="dlgIn" aria-label="Name" maxlength="120" value="${esc(m.input)}">`:''}</div><div class="df">${m.buttons.map(([id,label,style])=>`<button data-mb="${id}" class="${style||''}">${esc(label)}</button>`).join('')}</div></div></div>`;
    if(html!==shownModalHtml){
      if(opening)host.innerHTML=html;
      else{const template=document.createElement('template');template.innerHTML=html;const next=template.content.querySelector('.dlg');const current=host.querySelector('.dlg');current.innerHTML=next.innerHTML;current.setAttribute('aria-labelledby',next.getAttribute('aria-labelledby'));}
      shownModalHtml=html;
    }
    if(m.kind==='import'){
      if(opening)bindImport();
      const button=$('#impChBtn'),label=targetSummary(D(),m,true)+I('chev');
      if(button.innerHTML!==label)button.innerHTML=label;
    }else if(opening)requestAnimationFrame(()=>{if(S.modal!==m)return;const el=$('#dlgIn')||$('#over [data-mb]');el?.focus();if(el?.select)el.select();});
  }
  const key=S.pop==='filtertype'?S.pop+S.typeBand:S.pop;
  if(key!==shownPopover){$('#pop')?.remove();shownPopover=key;}
  if(S.pop){
    let pop=$('#pop');if(!pop){pop=document.createElement('div');pop.className='pop';pop.id='pop';over.append(pop);}
    const html=popHtml();
    if(pop.innerHTML!==html){
      const active=pop.contains(document.activeElement)?[...pop.querySelectorAll('button')].indexOf(document.activeElement):-1,scroll=pop.scrollTop;
      pop.innerHTML=html;pop.scrollTop=scroll;if(active>=0)pop.querySelectorAll('button')[active]?.focus({preventScroll:true});
    }
    positionPopover();
  }
  $$('[data-type]').forEach(el=>el.setAttribute('aria-expanded',String(S.pop==='filtertype'&&+el.dataset.type===S.typeBand)));
}
function modalAct(id){const m=S.modal;if(!m)return;const value=$('#dlgIn')?.value.trim()||'';if(m.kind==='import'){if(id==='go')commitImport();else{S.modal=null;S.pop=null;renderOver();}return;}S.modal=null;renderOver();m.on?.[id]?.(value);}
let toastTimer;
function toast(message){let el=$('#toast');if(!el){el=document.createElement('div');el.id='toast';el.className='toast';el.role='status';document.body.append(el);}el.textContent=message;el.classList.add('show');clearTimeout(toastTimer);toastTimer=setTimeout(()=>el.remove(),5000);}
async function switchProfile(id,choice){
  S.pop=null;await flush();const data=await command('select',{id,...(choice?{choice}:{})},true);
  if(data?.decision==='select')dialog('Save changes?',`Save your changes to ${P().name} before switching profiles?`,[['cancel','Cancel'],['discard',"Don't save"],['save','Save','go']],undefined,{save:()=>switchProfile(id,'save'),discard:()=>switchProfile(id,'discard')});
  else{S.sel=0;S.band=0;render();}
}
let lastClose='open';
function handleClose(){
  const c=APP?.closing;if(!c)return;
  if(c.state==='open'){if(lastClose!=='open'){S.modal=null;renderOver();}lastClose='open';return;}
  if(c.state===lastClose)return;lastClose=c.state;const cancel=()=>command('close',{action:'cancel'},true);
  if(c.state==='needs_decision')dialog('Save changes?',`Save your changes to ${P()?.name||'this profile'} before ${c.switching?'switching outputs':'closing'}?`,[['cancel','Cancel'],['discard',"Don't save"],['save','Save','go']],undefined,{cancel,save:()=>command('close',{choice:'save'},true),discard:()=>command('close',{choice:'discard'},true)});
  else if(c.state==='waiting')dialog('Updating the device…','Waiting for the audio engine to confirm the final EQ.',[['cancel','Keep editing']],undefined,{cancel});
  else if(c.state==='needs_attention')dialog('Device update pending',c.detail,[['cancel','Keep editing'],['pending',c.switching?'Switch with update pending':'Quit with update pending'],['retry','Retry','go']],undefined,{cancel,pending:()=>command('close',{action:'leave_pending'},true),retry:()=>command('close',{action:'retry'},true)});
}
function nameDialog(op,title,initial){dialog(title,op==='save_as'?'The new profile gets your current edits. The original returns to its last save.':'',[['cancel','Cancel'],['go','Save','go']],initial,{go:name=>{if(name)command(op,{name});}});}

// Imported PEQ uses the native strict importer. Its result becomes ordinary
// layer/filter data through the same replace/undo/live-preview route as typing.
let importTicket=0,importTimer;
function openImport(mode){S.pop=null;S.modal={kind:'import',mode,text:'',name:'Imported EQ',all:L()?.all??true,mask:L()?.mask??0,result:null};renderOver();$('#impText')?.focus();}
function importHtml(m){return `<div class="scrim"><div class="dlg wide" role="dialog" aria-modal="true" aria-labelledby="importTitle"><div class="dh"><h3 id="importTitle">${m.mode==='replace'?'Replace filters':'Import filters'}</h3><p>Paste REW or Equalizer APO parametric text, or choose a text file.</p></div><div class="imp"><div class="src" id="impSrc"><div class="gut" id="impGut"></div><textarea id="impText" aria-label="Filter text" spellcheck="false">${esc(m.text)}</textarea></div><div class="imp-side"><div class="imp-graph"><canvas id="impGraph"></canvas></div><div class="issues" id="impIss">Paste filters to preview them.</div><label><span class="lbl2">Layer name</span><input class="txt" id="impName" aria-label="Imported layer name" maxlength="120" value="${esc(m.name)}" ${m.mode==='replace'?'disabled':''}></label><div><span class="lbl2">Channels</span><button class="chf" id="impChBtn" ${m.mode==='replace'?'disabled':''}>${targetSummary(D(),m,true)}${I('chev')}</button></div><button class="lnk choose-file" id="chooseFile">Choose file</button><input id="impFile" type="file" accept=".txt,text/plain" hidden></div></div><div class="df"><span class="sum" id="impSum"></span><button data-mb="cancel">Cancel</button><button data-mb="go" class="go" id="impGo" disabled>Import</button></div></div></div>`;}
function bindImport(){
  $('#impText').oninput=e=>{S.modal.text=e.target.value;S.modal.result=null;$('#impGo').disabled=true;clearTimeout(importTimer);importTimer=setTimeout(updateImport,180);};
  $('#impName').oninput=e=>S.modal.name=e.target.value;
  const file=async f=>{if(!f)return;const m=S.modal;if(f.size>1024*1024){toast('The filter file must be smaller than 1 MB');return;}const text=await f.text();if(S.modal!==m)return;m.text=text;$('#impText').value=text;updateImport();};
  $('#chooseFile').onclick=()=>$('#impFile').click();
  $('#impFile').onchange=e=>file(e.target.files[0]);
  $('#impSrc').ondragover=e=>e.preventDefault();$('#impSrc').ondrop=e=>{e.preventDefault();file(e.dataTransfer.files[0]);};
  if(S.modal.result)showImport();else if(S.modal.text)updateImport();
}
async function updateImport(){
  const m=S.modal;if(m?.kind!=='import')return;const ticket=++importTicket;
  if(!m.text.trim()){m.result=null;$('#impGo').disabled=true;return;}
  try{const r=await rpc('import',{text:m.text});if(S.modal!==m||ticket!==importTicket)return;m.result=r;showImport();}catch(e){if(S.modal===m){$('#impIss').textContent=e.message;$('#impGo').disabled=true;}}
}
function showImport(){
  const r=S.modal?.result;if(!r)return;$('#impGo').disabled=!r.valid;
  $('#impIss').innerHTML=r.issues.map(i=>`<div>${i.error?'Error':'Note'} · line ${i.line}: ${esc(i.message)}</div>`).join('')||(r.valid?'Ready to import.':'No supported filters found.');
  $('#impSum').textContent=r.valid?`${r.layer.filters.length} filters · ${show('gain',r.layer.gain).join(' ')} gain`:'';
  const cv=$('#impGraph'),box=cv.parentElement.getBoundingClientRect();cv.width=box.width*devicePixelRatio;cv.height=box.height*devicePixelRatio;cv.style.width='100%';cv.style.height='100%';
  const ctx=cv.getContext('2d');ctx.scale(devicePixelRatio,devicePixelRatio);ctx.clearRect(0,0,box.width,box.height);
  if(r.valid){const g=grid(200),response=resp(r.layer.filters,g);ctx.strokeStyle=getComputedStyle(cv).getPropertyValue('--g-curve');ctx.lineWidth=1.5;ctx.stroke(new Path2D(pathOf(response,g,box.width,box.height,12)));}
}
function commitImport(){
  const m=S.modal;if(!m?.result?.valid)return;const layer=clone(m.result.layer);
  if(m.mode==='replace'){layer.name=L().name;layer.all=L().all;layer.mask=L().mask;layer.on=L().on;}else{if(LAYERS().length>=MAXL){toast('This profile already has 32 layers');return;}layer.name=m.name.trim()||'Imported EQ';layer.all=m.all;layer.mask=m.mask;}
  S.modal=null;S.pop=null;edit(()=>{if(m.mode==='replace')LAYERS()[S.sel]=layer;else{LAYERS().push(layer);S.sel=LAYERS().length-1;}S.band=0;});
}

function captureLayerPositions(){return new Map($$('.layers [data-layer]').map(el=>[LAYERS()[+el.dataset.layer],el.getBoundingClientRect()]));}
function animateLayerPositions(before){
  if(window.matchMedia?.('(prefers-reduced-motion: reduce)').matches)return;
  $$('.layers [data-layer]').forEach(el=>{const from=before.get(LAYERS()[+el.dataset.layer]);if(!from)return;el.getAnimations?.().forEach(a=>a.cancel());const to=el.getBoundingClientRect(),dx=from.left-to.left,dy=from.top-to.top;if(Math.abs(dx)+Math.abs(dy)>.5)el.animate?.([{transform:`translate(${dx}px,${dy}px)`},{transform:'translate(0,0)'}],{duration:170,easing:'cubic-bezier(.2,.7,.3,1)'});});
}
function moveLayer(from,to,before=captureLayerPositions()){
  const layers=LAYERS(),selected=L();
  if(from===to||!layers[from]||to<0||to>=layers.length)return;
  S.pop=null;edit(()=>{layers.splice(to,0,layers.splice(from,1)[0]);S.sel=layers.indexOf(selected);});
  animateLayerPositions(before);
}
function removeFilter(i=S.band){
  if(!L()?.filters[i])return;
  endGesture();edit(()=>{L().filters.splice(i,1);if(i<S.band)S.band--;});
  $('#graph')?.focus({preventScroll:true});
}
function openFilterType(i){
  if(!L()?.filters[i])return;
  const same=S.pop==='filtertype'&&S.typeBand===i;
  selectBand(i);S.typeBand=i;S.pop=same?null:'filtertype';renderOver();
  if(S.pop)$('#pop [aria-selected="true"]')?.focus({preventScroll:true});
}
function chooseFilterType(type){
  const i=S.typeBand,f=L()?.filters[i];if(!f||!TYPE[type])return;
  S.pop=null;
  if(f.type!==type)edit(()=>{if(!hasGain(type)&&hasGain(f.type)){f._gain=f.gain;f.gain=0;}if(hasGain(type)&&!hasGain(f.type))f.gain=f._gain??0;f.type=type;S.band=i;});
  else renderOver();
  $(`#type-${i}`)?.focus({preventScroll:true});
}
function layerAction(i,action){
  const layers=LAYERS(),selected=L();if(!layers[i])return;
  const j=i+(action==='up'?-1:1);
  if(action!=='trash'&&(j<0||j>=layers.length))return;
  if(action!=='trash'){moveLayer(i,j);$(`[data-layer="${j}"] [data-layer-action="${action}"]`)?.focus({preventScroll:true});return;}
  S.pop=null;edit(()=>{
    if(action==='trash')layers.splice(i,1);else [layers[i],layers[j]]=[layers[j],layers[i]];
    const next=layers.indexOf(selected);S.sel=next<0?Math.min(i,layers.length-1):next;
    if(next<0)S.band=0;
  });
}

// The host starts native move/size loops; the title bar's controls stay clickable.
const captionSpace=t=>!t.closest('button,input')&&t.closest('.tb');
document.addEventListener('pointerdown',e=>{
  if(e.button!==0||!WINDOW.custom)return;
  const edge=e.target.closest('[data-resize]'),target=edge?(WINDOW.maximized?null:edge):captionSpace(e.target);
  if(!target)return;
  e.preventDefault();
  if(target.hasPointerCapture(e.pointerId))target.releasePointerCapture(e.pointerId);
  rpc('window',{action:edge?edge.dataset.resize:'move'}).then(windowState).catch(e=>toast(e.message));
});
document.addEventListener('contextmenu',e=>{
  if(!WINDOW.custom||!captionSpace(e.target))return;
  e.preventDefault();rpc('window',{action:'menu'}).catch(e=>toast(e.message));
});
document.addEventListener('click',async e=>{
  const windowButton=e.target.closest('[data-window]');if(windowButton){rpc('window',{action:windowButton.dataset.window}).then(windowState).catch(e=>toast(e.message));return;}
  const t=e.target,mb=t.closest('[data-mb]');if(mb){modalAct(mb.dataset.mb);return;}
  if(t.closest('[data-layer]')&&Date.now()<ignoreLayerClickUntil)return;
  const type=t.closest('[data-type]');if(type){openFilterType(+type.dataset.type);return;}
  const typeChoice=t.closest('[data-filter-type]');if(typeChoice){chooseFilterType(typeChoice.dataset.filterType);return;}
  const anchors={devBtn:'devices',profileBtn:'profiles',chBtn:'channels',layerMenuBtn:'layermenu',outBtn:'outch',impChBtn:'impch'};
  const a=t.closest('#devBtn,#profileBtn,#chBtn,#layerMenuBtn,#outBtn,#impChBtn');if(a){S.pop=S.pop===anchors[a.id]?null:anchors[a.id];renderOver();return;}
  if(S.pop&&!t.closest('#pop'))closePopover();
  const device=t.closest('[data-device]');if(device){S.pop=null;await command('device',{id:device.dataset.device},true);return;}
  const pf=t.closest('[data-prof]');if(pf){switchProfile(pf.dataset.prof);return;}
  const target=S.pop==='impch'?S.modal:L(),ch=fn=>{if(S.pop==='impch'){fn(target);renderOver();}else edit(()=>fn(target));};
  const co=t.closest('[data-chopt]');if(co){ch(l=>{l.all=co.dataset.chopt==='all';l.mask=l.all?0:(1<<+co.dataset.chopt)>>>0;});return;}
  const cm=t.closest('[data-chmode]');if(cm){ch(l=>{l.all=cm.dataset.chmode==='all';l.mask=l.all?0:fullMask(D())>>>0;});return;}
  const sp=t.closest('[data-spk]');if(sp){const mask=(target.all?(1<<+sp.dataset.spk):target.mask^(1<<+sp.dataset.spk))>>>0;if(!mask){toast('Choose at least one channel');return;}ch(l=>{l.all=false;l.mask=mask;});return;}
  const grp=t.closest('[data-grp]');if(grp){ch(l=>{l.all=false;l.mask=+grp.dataset.grp>>>0;});return;}
  const om=t.closest('[data-outmode]');if(om){S.outMode=om.dataset.outmode;S.output=true;if(!S.outMask)S.outMask=fullMask(D())>>>0;render();return;}
  const os=t.closest('[data-outspk]');if(os){const mask=(S.outMask^(1<<+os.dataset.outspk))>>>0;if(mask){S.outMask=mask;S.outMode='pick';S.output=true;render();}return;}
  if(t.closest('#outToggle')){S.output=!S.output;render();return;}
  if(t.closest('#addBand')){addBand(F('PK',1000,0,1));return;}
  const del=t.closest('[data-del]');if(del){removeFilter(+del.dataset.del);return;}
  const lo=t.closest('[data-layeron]');if(lo){edit(()=>{const l=LAYERS()[+lo.dataset.layeron];l.on=!l.on;});return;}
  const bo=t.closest('[data-bandon]');if(bo){edit(()=>{const f=L().filters[+bo.dataset.bandon];f.on=!f.on;});return;}
  const la=t.closest('[data-layer-action]');if(la){layerAction(+la.dataset.index,la.dataset.layerAction);return;}
  const layer=t.closest('[data-layer]');if(layer){S.sel=+layer.dataset.layer;S.band=0;render();return;}
  const band=t.closest('[data-band]');if(band&&!t.closest('.fld,button')){selectBand(+band.dataset.band);band.focus({preventScroll:true});return;}
  const act=t.closest('[data-act]')?.dataset.act;if(!act)return;
  if(!['theme','status','setup','settings','back-eq','refresh-devices','retry-master','eq','new-profile'].includes(act)&&!P())return;
  switch(act){
    case 'settings':case 'setup':openSetup();break;
    case 'back-eq':if(!setupOperation){S.view='editor';S.pop=null;S.setupConfirm=null;render();}break;
    case 'refresh-devices':refreshDevices(true);break;
    case 'retry-master':command('eq',{enabled:APP.eqEnabled!==false},true);break;
    case 'theme':S.theme=S.theme==='dark'?'light':'dark';localStorage.setItem('theme',S.theme);render();syncWindowTheme();break;
    case 'undo':undo();break;case 'redo':redo();break;
    case 'save':S.pop=null;command('save');break;
    case 'apply':command('apply');break;
    case 'revert':command('revert');break;
    case 'new-profile':if(P()?.dirty){toast('Save or revert this profile before creating another.');break;}nameDialog('new','New profile','My EQ');break;
    case 'rename-profile':nameDialog('rename','Rename profile',P().name);break;
    case 'save-as':nameDialog('save_as','Save as new profile',P().name+' copy');break;
    case 'duplicate-profile':nameDialog('duplicate','Duplicate profile',P().name+' copy');break;
    case 'eq':document.activeElement?.blur();endGesture();command('eq',{enabled:APP.eqEnabled===false},true);break;
    case 'add-layer':if(LAYERS().length<MAXL)edit(()=>{LAYERS().push(Ly('Layer '+(LAYERS().length+1),true,0,0,true,[]));S.sel=LAYERS().length-1;S.band=0;});break;
    case 'import':openImport('new');break;case 'import-into':openImport('replace');break;
    case 'dup-layer':S.pop=null;if(LAYERS().length<MAXL)edit(()=>{const l=clone(L());l.name+=' copy';LAYERS().splice(++S.sel,0,l);});break;
    case 'del-layer':layerAction(S.sel,'trash');break;
    case 'layer-up':case 'layer-down':layerAction(S.sel,act==='layer-up'?'up':'down');break;
    case 'status':{
      await command('inspect',{},true);
      const saved={not_checked:'Not checked',unavailable:'Unavailable',missing:'No saved EQ',matches:'Matches this profile',differs:'Different from this profile',invalid:'Saved EQ is invalid',unreadable:'Cannot read saved EQ'};
      const running={unavailable:'No current engine report',unknown:'The engine has not identified its EQ',loaded_match:'This EQ is loaded; waiting for audio',loaded_difference:'A different EQ is loaded',processed_match:'This EQ has processed audio',processed_difference:'A different EQ has processed audio',transitioning:'An EQ change is in progress',incompatible:'This profile does not match the device format'};
      dialog('Output status',`${D().name}\n\nWindows saved EQ: ${saved[APP.status.saved]}\nRunning EQ: ${running[APP.status.running]}\n\n${[APP.status.detail,APP.status.copyDetail,APP.status.previewDetail].filter(Boolean).join('\n')}`,[['cancel','Close']],undefined,{});break;
    }
  }
});
document.addEventListener('change',e=>{if(e.target.id==='lnIn'){const value=e.target.value.trim();if(value)edit(()=>L().name=value);else e.target.value=L().name;}});
document.addEventListener('keydown',e=>{
  if(e.key==='Tab'&&S.modal){const items=$$('.dlg button:not(:disabled),.dlg input:not(:disabled):not([hidden]),.dlg textarea');const first=items[0],last=items.at(-1);if(e.shiftKey&&document.activeElement===first){e.preventDefault();last?.focus();}else if(!e.shiftKey&&document.activeElement===last){e.preventDefault();first?.focus();}}
  if((e.key==='Enter'||e.key===' ')&&(e.target.matches('.sw:not(button)')||e.target.matches('[data-layer]'))){e.preventDefault();e.target.click();}
});
window.addEventListener('resize',()=>{draw();if(S.pop)positionPopover();if(S.modal?.kind==='import')showImport();});
if(window.chrome?.webview){
  window.chrome.webview.addEventListener('message',e=>{
    const message=e.data;
    if(message.requestId!==undefined){const pending=replies.get(message.requestId);if(!pending)return;replies.delete(message.requestId);if(message.ok)pending.resolve(message.data);else{const error=new Error(message.error);error.state=message.state;pending.reject(error);}return;}
    if(message.event==='window'){windowState(message.data);return;}
    if(message.event==='setup'){acceptSetup(message.data);return;}
    if(message.event==='close'){document.activeElement?.blur();endGesture();command('close',{},true);return;}
    if(message.event==='error'){toast(message.error);return;}
    if(message.event==='status'&&APP){
      const before=P()?.id,rate=P()?.curveRate,canAdopt=!pendingEdit&&!gesture&&replies.size===0;
      const rateChanged=canAdopt&&message.data.editor?.curveRate!==rate;
      adopt(message.data,canAdopt);statusbar();handleClose();
      updateSettingsStatus();
      if(before!==P()?.id||rateChanged)command('state',{},true);
    }
  });
  rpc('state').then(data=>{adopt(data);render();syncWindowTheme();}).catch(error=>{$('#win').innerHTML=`<div class="fatal">${esc(error.message)}</div>`;});
}else $('#win').innerHTML='<div class="fatal">Open Contour.exe to use this editor.</div>';

