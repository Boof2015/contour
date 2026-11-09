// Real Chromium DOM/layout regression checks with a fake native host. The host
// never enumerates devices, writes the profile library, or touches live audio.
const fs=require('node:fs'),path=require('node:path'),http=require('node:http'),{spawn}=require('node:child_process');
const ui=path.resolve(process.argv[2]),work=path.resolve(process.argv[3]),preview=process.argv.includes('--preview');
const edge=[process.env.CONTOUR_TEST_BROWSER,path.join(process.env['ProgramFiles(x86)']||'', 'Microsoft/Edge/Application/msedge.exe'),path.join(process.env.ProgramFiles||'', 'Microsoft/Edge/Application/msedge.exe')].find(p=>p&&fs.existsSync(p));
if(!edge)throw Error('Microsoft Edge is required for the UI interaction checks');
fs.mkdirSync(work,{recursive:true});
const profile=fs.mkdtempSync(path.join(work,'browser-'));
const fixture=()=>{
  if(new URL(location.href).searchParams.has('light'))localStorage.setItem('theme','light');
  const filter=(gain=0)=>({type:'PK',on:true,freq:1000,gain,q:1});
  const state={device:'test-output',devices:[{id:'test-output',name:'Offline test output',channels:2,rate:48000,mask:3,available:false}],profiles:[],closing:{state:'open'},status:{endpoint:'ready',applied:true,running:'processed_match',saved:'matches',copy:'current'},editor:{id:'test-profile',name:'Test profile',channels:2,rate:48000,curveRate:48000,revision:1,dirty:false,undo:false,redo:false,work:{bypass:false,layers:['A','B','C'].map(name=>({name,all:true,mask:0,gain:0,on:true,filters:[filter()]}))}}};
  state.profiles=[{id:'test-profile',name:'Test profile',channels:2,revision:1,layers:3,filters:3,canDelete:false,usedBy:[{id:'test-output',name:'Offline test output',available:true}]},
    {id:'test-unused',name:'Unused <profile>',channels:2,revision:7,layers:2,filters:5,canDelete:true,usedBy:[]},
    {id:'test-assigned',name:'Surround profile',channels:6,revision:1,layers:1,filters:4,canDelete:false,usedBy:[{id:'test-disconnected',name:'Disconnected test device',available:false}]}];
  state.eqEnabled=true;state.masterIssues=[];state.devices[0].available=true;state.startInTray=false;state.closeToTray=true;state.recoveries=[];
  let startWithWindows=false;
  state.devices.push({id:'test-second',name:'Second test device',channels:2,rate:48000,mask:3,available:true},{id:'test-disconnected',name:'Disconnected test device',channels:0,rate:0,mask:0,available:false,state:8});
  const installed=new Set(['test-output']);
  let engineUpdate=new URL(location.href).searchParams.has('engine-update');
  if(new URL(location.href).searchParams.has('frame'))window.addEventListener('load',()=>windowState({custom:true,maximized:false}));
  if(engineUpdate)window.addEventListener('load',()=>openSettings('outputs'));
  if(new URL(location.href).searchParams.has('profiles'))window.addEventListener('load',()=>openProfiles());
  if(new URL(location.href).searchParams.has('adapt'))window.addEventListener('load',async()=>{await openProfiles();await openAdapt('test-assigned');});
  if(new URL(location.href).searchParams.has('layer-menu'))window.addEventListener('load',()=>{S.pop='layermenu';renderOver();});
  if(new URL(location.href).searchParams.has('general'))window.addEventListener('load',()=>openSettings('general'));
  if(new URL(location.href).searchParams.has('recovery'))state.recoveries=[{id:'recovery-1',name:'My EQ',device:'Offline test output'}];
  if(new URL(location.href).searchParams.has('uninstalled')){
    installed.delete('test-output');state.editor=null;state.status={endpoint:'unavailable'};
    state.devices[0].name='Speakers (Steam Streaming Speakers)';
  }
  let receive;window.testEdits=[];window.testRequests=[];window.testState=state;
  window.chrome.webview={addEventListener:(_,fn)=>receive=fn,postMessage:body=>{
    const r=JSON.parse(body);window.testRequests.push(r);let data=state;
    if(r.op==='window')data={custom:new URL(location.href).searchParams.has('frame'),maximized:false};
    if(r.op==='edit'){state.editor.work=r.work;state.editor.revision++;state.editor.dirty=true;window.testEdits.push(structuredClone(r.work));}
    if(r.op==='eq')state.eqEnabled=r.enabled;
    if(r.op==='device'&&window.holdDeviceSwitch)state.closing={state:'waiting',switching:true};
    if(r.op==='preferences'){for(const key of ['startInTray','closeToTray'])if(key in r)state[key]=r[key];}
    if(r.op==='startup'){
      if(r.action==='set'&&window.failStartup){setTimeout(()=>receive({data:{requestId:r.requestId,ok:false,error:'Test startup registration failure'}}),0);return;}
      if(r.action==='set')startWithWindows=r.enabled;
      data={available:true,enabled:startWithWindows,note:'Open Contour when you sign in. Windows can also disable it in Startup apps.'};
    }
    if(['recovery_keep','recovery_discard'].includes(r.op)){
      state.recoveries=state.recoveries.filter(item=>item.id!==r.id);
      if(r.op==='recovery_keep')state.profiles.push({id:'kept-recovery',name:'My EQ (recovered)',channels:2,revision:1,layers:1,filters:1,canDelete:true,usedBy:[]});
    }
    if(r.op==='profile_export')data=window.cancelExport?{cancelled:true}:{cancelled:false,filename:'Test profile.contour'};
    if(r.op==='layer_export')data=window.cancelExport?{cancelled:true}:{cancelled:false,filename:'Filters.txt'};
    if(r.op==='profile_preview'){
      const profile=state.profiles.find(p=>p.id===r.id),layers=[
        {name:'Left',all:false,mask:1,gain:-8,on:true,filters:[{...filter(-10),freq:120,q:2},{...filter(3),freq:3600}]},
        {name:'Right',all:false,mask:2,gain:-5,on:true,filters:[{...filter(-7),freq:190,q:2},{...filter(2),freq:2200}]},
        {name:'All',all:true,mask:0,gain:-4,on:true,filters:[{...filter(4),type:'LSQ',freq:90}]}];
      for(const l of layers)for(const f of l.filters)f.coefficients=designCurve(f,44100);
      data={id:r.id,revision:profile.revision,channels:profile.channels,rate:44100,work:{layers}};
    }
    if(r.op==='profile_delete')state.profiles=state.profiles.filter(p=>p.id!==r.id);
    if(r.op==='profile_adapt_preview')data={id:r.id,name:'Surround profile',sourceRevision:1,draftRevision:null,sourceChannels:6,target:{id:'test-output',channels:2,rate:48000,mask:3},work:{layers:[
      {name:'All',all:true,mask:0,gain:-3,on:true,filters:[filter()]},
      {name:'Rear pair',all:false,mask:48,gain:-4,on:false,filters:[filter(-2)]},
      {name:'First channel',all:false,mask:1,gain:0,on:true,filters:[filter(1)]}]}};
    if(r.op==='profile_adapt'){
      if(window.failAdapt){setTimeout(()=>receive({data:{requestId:r.requestId,ok:false,error:'The profile or device changed. Reopen Adapt a copy to review it again.'}}),0);return;}
      state.profiles.push({id:'adapted-copy',name:r.name,channels:2,revision:1,layers:3,filters:3,canDelete:true,usedBy:[]});data={...state,createdProfileId:'adapted-copy'};
    }
    if(r.op==='profile_import'){state.profiles.push({id:'test-imported',name:r.name,channels:2,revision:1,layers:0,filters:0,canDelete:true,usedBy:[]});data={...state,createdProfileId:'test-imported'};}
    if(r.op==='setup'){
      if(r.action==='Install')installed.add(r.endpoint);
      if(r.action==='Remove')installed.delete(r.endpoint);
      const attached=installed.has(r.endpoint);
      data={endpoint:r.endpoint,busy:false,state:attached?'attached':'available',installation:attached?'installed':'not_installed',canInstall:!attached,canRemove:attached};
      if(r.action==='Update')engineUpdate=false;
      if(engineUpdate)Object.assign(data,{engineState:'update_available',canUpdate:true,engineDevices:['Speakers (Focusrite USB Audio)','Realtek Digital Output (Realtek USB Audio)']});
    }
    if(r.op==='setup'&&r.action==='Remove'&&window.holdRemoval)data={endpoint:r.endpoint,busy:true,state:'removing',canInstall:false,canRemove:false};
    if(r.op==='setup'&&['Update','RecoverUpdate'].includes(r.action)&&window.holdUpdate)data={endpoint:r.endpoint,busy:true,state:r.action==='Update'?'updating':'recovering',canInstall:false,canRemove:false};
    if(r.op==='setup'&&r.action==='Inspect'&&window.holdInspection)data={endpoint:r.endpoint,busy:true,state:'checking',canInstall:false,canRemove:false};
    if(r.op==='import')data={valid:true,issues:[],layer:{name:'Imported',all:true,mask:0,gain:0,on:true,filters:[filter(-3)]}};
    if(!Number.isSafeInteger(r.requestId))throw Error('Missing numeric bridge request identity');
    setTimeout(()=>receive({data:{requestId:r.requestId,ok:true,data:structuredClone(data)}}),0);
  }};
  window.testStatus=(status,lightweight=false)=>{const data={...structuredClone(state),status};if(lightweight&&data.editor)delete data.editor.work;receive({data:{event:'status',data}});};
  window.testSetup=data=>receive({data:{event:'setup',data}});
};
const checks=async()=>{
  const results=[],assert=(ok,msg)=>{if(!ok)throw Error(msg);},wait=ms=>new Promise(r=>setTimeout(r,ms)),key=(el,value)=>el.dispatchEvent(new KeyboardEvent('keydown',{key:value,bubbles:true,cancelable:true}));
  const finish=error=>{document.body.insertAdjacentHTML('beforeend','<pre id="test-results"></pre>');document.getElementById('test-results').textContent=JSON.stringify({passed:!error,checks:results,error:error?.stack});};
  try{
    for(let i=0;i<50&&!P();i++)await wait(20);
    assert(P(),'Editor did not load');
    $('#profileBtn').click();$('[data-prof="test-profile"]').click();await wait(60);
    let selected=window.testRequests.find(r=>r.op==='select');
    assert(selected?.id==='test-profile'&&Number.isSafeInteger(selected.requestId),'Profile menu lost entity or reply identity');
    $('#devBtn').click();$('[data-device="test-output"]').click();await wait(60);
    selected=window.testRequests.find(r=>r.op==='device');
    assert(selected?.id==='test-output'&&Number.isSafeInteger(selected.requestId),'Output menu lost entity or reply identity');
    assert(!S.pop&&!replies.size,'Selection stranded the command queue');
    results.push('Profile and output menu selections retain separate entity and reply IDs, and the command queue drains');
    openImport('new');
    const area=$('#impText'),name=$('#impName'),canvas=$('#impGraph'),dialogNode=$('.dlg');
    area.value=Array.from({length:60},(_,i)=>'Filter '+i+' test text').join('\n');area.dispatchEvent(new Event('input'));await wait(250);
    name.value='Room right';name.dispatchEvent(new Event('input'));
    area.scrollTop=180;area.setSelectionRange(20,33);const scroll=area.scrollTop;
    $('#impChBtn').click();const picker=$('#pop');$('[data-chopt="1"]').click();
    assert($('#impText')===area&&$('#impName')===name&&$('#impGraph')===canvas&&$('.dlg')===dialogNode,'Channel choice rebuilt import controls');
    assert($('#pop')===picker,'Channel choice recreated the popover');
    assert(area.scrollTop===scroll&&area.selectionStart===20&&area.selectionEnd===33,'Import text scroll/caret was reset');
    assert(name.value==='Room right'&&S.modal.mask===2&&!S.modal.all&&!$('#impGo').disabled,'Import state was lost');
    $('#impChBtn').click();assert(!$('#pop')&&$('.dlg')===dialogNode,'Closing nested picker rebuilt its parent');
    $('#impChBtn').click();key($('#pop button'),'Escape');assert(!$('#pop')&&$('.dlg')===dialogNode,'Escape dismissed the import');
    assert(document.activeElement===$('#impChBtn'),'Escape did not restore trigger focus');
    modalAct('cancel');results.push('Import keeps dialog, textarea, selection, scroll, name and canvas while channels change');

    $('#type-0').click();assert(!document.querySelector('select[data-type]'),'Native filter select remains');
    assert(document.activeElement.getAttribute('aria-selected')==='true','Filter menu did not focus current type');
    key(document.activeElement,'ArrowDown');assert(document.activeElement.dataset.filterType==='LSQ','Filter menu arrow navigation failed');
    document.activeElement.click();await wait(50);assert(L().filters[0].type==='LSQ','Menu choice did not update PEQ');
    assert(document.activeElement===$('#type-0'),'Type choice lost focus');
    $('#type-0').click();const dark=getComputedStyle($('#pop')).backgroundColor;key(document.activeElement,'Escape');
    S.theme='light';render();$('#type-0').click();assert(getComputedStyle($('#pop')).backgroundColor!==dark,'Filter menu ignored theme');
    key(document.activeElement,'Escape');S.theme='dark';render();results.push('Filter menu follows theme, supports arrows and Escape, and edits the actual draft');

    const statusNode=$('.status-label'),statusRoot=$('#statusbar');
    for(const running of ['processed_match','transitioning','unknown','loaded_difference','processed_match']){
      window.testStatus({endpoint:'ready',running,applied:running==='processed_match'});
      assert($('.status-label').textContent==='Connected','Ordinary acknowledgement changed visible status');
      assert($('.status-label')===statusNode&&$('#statusbar')===statusRoot,'Status updates replaced buttons');
    }
    window.testStatus({endpoint:'ready',running:'processed_difference',applied:false});await wait(1300);
    assert($('.status-label').textContent==='EQ needs attention','Persistent mismatch was hidden');
    window.testStatus({endpoint:'error',detail:'Test device failure'});assert($('.status-label').textContent==='Output needs attention','Device error was hidden');
    window.testStatus(window.testState.status={endpoint:'ready',running:'processed_match',applied:true});results.push('Status is stable through acknowledgements; persistent mismatch and device failure remain visible');

    // Synthetic pointers use real layout, with only OS pointer ownership stubbed.
    // Native mouse/pen ownership is covered by the manual window check.
    const pointer=(node,type,x,y)=>node.dispatchEvent(new PointerEvent(type,{bubbles:true,cancelable:true,pointerId:11,button:0,clientX:x,clientY:y}));
    const prepare=()=>{const list=$('.layers');let captured=false;list.setPointerCapture=()=>captured=true;list.hasPointerCapture=()=>captured;list.releasePointerCapture=()=>captured=false;return list;};
    let list=prepare(),card=list.children[0],r=card.getBoundingClientRect(),bottom=list.children[2].getBoundingClientRect();
    let count=window.testEdits.length;
    pointer(card,'pointerdown',r.left+30,r.top+20);pointer(list,'pointermove',r.left+30,bottom.bottom-10);
    assert(layerDrag?.active&&$('.layer-drag-ghost'),'Layer drag never started');
    assert(window.testEdits.length===count,'Drag published an intermediate order');
    pointer(list,'pointerup',r.left+30,bottom.bottom-10);await wait(60);
    assert(LAYERS().map(l=>l.name).join('')==='BCA','Drop did not reorder layers');
    assert(L().name==='A','Drop changed the selected layer');
    assert(window.testEdits.length===count+1,'Drop was not a single native edit');
    assert(!$('.layer-drag-ghost')&&!layerDrag,'Drag cleanup left a floating layer');
    list=prepare();card=list.children[0];r=card.getBoundingClientRect();bottom=list.children[2].getBoundingClientRect();count=window.testEdits.length;
    pointer(card,'pointerdown',r.left+30,r.top+20);pointer(list,'pointermove',r.left+30,bottom.bottom-10);key(list,'Escape');await wait(30);
    assert(LAYERS().map(l=>l.name).join('')==='BCA'&&window.testEdits.length===count,'Cancelled drag changed EQ');
    list=prepare();card=list.children[0];r=card.getBoundingClientRect();pointer(card,'pointerdown',r.left+30,r.top+20);pointer(list,'pointerup',r.left+30,r.top+20);
    assert(L().name==='B','Clicking a draggable card no longer selected it');
    layerAction(0,'down');await wait(60);assert(LAYERS().map(l=>l.name).join('')==='CBA'&&L().name==='B','Button reorder lost selection');
    results.push('Layer drop publishes once, Escape cancels, clicks select, and buttons preserve selection');

    addBand(F('PK',2000,2,1));await wait(60);selectBand(1);const input=$('.fld input');input.focus();key(input,'Delete');
    assert(L().filters.length===2,'Delete in an input removed a filter');
    const graph=$('#graph');graph.focus();key(graph,'Delete');await wait(60);assert(L().filters.length===1,'Delete on graph did not remove selected filter');
    $('.layers [data-layer]').focus();key(document.activeElement,'Delete');assert(L().filters.length===1,'Delete in layer pane removed a filter');
    results.push('Delete removes the selected filter without stealing text editing or layer-pane keys');

    const focusedGain=$('.gainbox input'),inputRequests=window.testRequests.length,gainBefore=L().gain;
    focusedGain.focus();focusedGain.value='-7.5';focusedGain.dispatchEvent(new Event('input',{bubbles:true}));await wait(60);
    const typingCopy=window.testRequests.slice(inputRequests).find(r=>r.op==='recovery_input'&&!r.clear);
    assert(typingCopy?.work.layers[S.sel].gain===-7.5&&L().gain===gainBefore&&!window.testRequests.slice(inputRequests).some(r=>r.op==='edit'),'Typing recovery auditioned a value or lost the pending field');
    key(focusedGain,'Escape');await wait(40);
    assert(window.testRequests.slice(inputRequests).some(r=>r.op==='recovery_input'&&r.clear)&&L().gain===gainBefore,'Escape kept the recovery-only field');
    results.push('Valid focused fields have a recovery copy without auditioning; Escape clears that copy');

    const layerBefore=structuredClone(L()),layerIndex=S.sel;
    $('#layerMenuBtn').click();$('[data-act="rename-layer"]').click();
    assert(document.activeElement===$('#lnIn')&&$('#lnIn').selectionEnd===L().name.length,'Rename did not focus and select the layer name');
    $('#lnIn').value='Renamed layer';$('#lnIn').dispatchEvent(new Event('change',{bubbles:true}));await wait(60);
    $('#layerMenuBtn').click();const exportBefore=window.testRequests.length;$('[data-act="export-layer"]').click();await wait(60);
    const exportedLayer=window.testRequests.slice(exportBefore).find(r=>r.op==='layer_export');
    assert(exportedLayer?.id===P().id&&exportedLayer.layer===S.sel&&exportedLayer.revision===P().revision,'Layer menu exported a stale or different layer');
    assert(!window.testRequests.slice(exportBefore).some(r=>['save','select','edit','eq'].includes(r.op)),'Layer export changed the draft or EQ');
    $('#addBand').click();await wait(50);assert(L().filters.at(-1).freq===500,'New band overlapped the existing 1 kHz band');
    edit(()=>L().filters=Array.from({length:MAXF},()=>F('PK',1000,0,1)));await wait(50);
    assert($('#addBand').disabled&&$('#addBand').title.includes('64'),'Full layer left Add filter looking available');
    edit(()=>LAYERS()[layerIndex]=layerBefore);await wait(50);
    results.push('Layer menu renames and exports the selected draft; new bands start apart; full layers disable Add filter');

    const profilesBefore=APP.profiles;
    APP.profiles=Array.from({length:50},(_,i)=>({...profilesBefore[0],id:'menu-'+i,name:'Profile '+i}));
    $('#profileBtn').click();const menu=$('#pop'),menuRect=menu.getBoundingClientRect();
    assert(menuRect.top>=0&&menuRect.bottom<=innerHeight&&menu.scrollHeight>menu.clientHeight,'Long profile menu escaped the window');
    menu.querySelector('[data-act="profiles"]').focus();assert(menu.scrollTop>0,'Last menu action could not be reached by keyboard');
    closePopover();APP.profiles=profilesBefore;
    results.push('Long menus stay within the window and scroll to focused actions');

    const editCount=window.testEdits.length,workBefore=JSON.stringify(P().work),dirtyBefore=P().dirty;
    $('[data-act="eq"]').click();await wait(60);
    assert(APP.eqEnabled===false&&JSON.stringify(P().work)===workBefore&&P().dirty===dirtyBefore&&window.testEdits.length===editCount,'Global toggle became a profile edit');
    assert(!$('#statusbar button'),'Settings actions still clutter the footer');
    $('[data-act="settings"]').click();await wait(60);
    assert(S.view==='settings'&&$('.settings-main')&&!$('#graph'),'Settings did not replace the editor body');
    acceptSetup({endpoint:'test-output',state:'attached',installation:'installed',canRemove:true});
    acceptSetup({endpoint:'test-second',state:'available',installation:'not_installed',canInstall:true});updateSettingsStatus();
    const profileBefore=P().id,deviceBefore=APP.device;
    const manage=$('[data-manage-device="test-second"]');manage.focus();window.holdInspection=true;
    setupRequest('Inspect','test-output');await wait(30);
    assert(document.activeElement===manage,'Background check stole focus');
    assert(!$('[data-act="eq"]').disabled,'Background check disabled global EQ');
    assert($('[data-manage-device="test-second"]').textContent==='Manage','Device expander is misleadingly labelled Install');
    $('[data-manage-device="test-second"]').click();
    assert(APP.device===deviceBefore&&P().id===profileBefore&&S.settingsDevice==='test-second','Inspecting a settings row switched the editor');
    assert(!window.testRequests.some(r=>r.op==='setup'&&r.action==='Install'),'Expanding a device started installation');
    assert($('[data-device-detail="test-second"]').textContent.includes('Audio will briefly stop during installation.'),'Installation notice was not visible before the action');
    const installButton=$('[data-setup-act="install"]');assert(!installButton.disabled,'Another device check disabled this Install button');
    installButton.click();installButton.click();await wait(30);
    assert(!window.testRequests.some(r=>r.op==='setup'&&r.action==='Install'),'Installation ran concurrently with the inspector');
    assert($('[data-setup-act="cancel-pending"]'),'Queued installation cannot be cancelled');
    window.holdInspection=false;window.testSetup({endpoint:'test-output',state:'attached',installation:'installed',busy:false,canRemove:true});await wait(80);
    const installs=window.testRequests.filter(r=>r.op==='setup'&&r.action==='Install');
    assert(installs.length===1&&installs[0].endpoint==='test-second','Single Install action did not target the chosen device exactly once');
    assert(!$('.install-review'),'Install required another confirmation');
    $('[data-setup-act="uninstall"]').click();
    assert($('.install-review').textContent.includes('Uninstall Contour from this device?'),'Removal assumed a device type');
    $('[data-setup-act="cancel"]').click();assert(!S.setupConfirm,'Removal confirmation did not cancel');
    window.holdRemoval=true;
    $('[data-setup-act="uninstall"]').click();$('[data-setup-act="confirm"]').click();await wait(30);
    assert(!$('.install-review')&&!$('[data-setup-act]'),'Removal retained its confirmation or device actions');
    const progress=$('[data-device-detail="test-second"] .device-explanation');
    window.testSetup({endpoint:'test-second',state:'checking',busy:true,canInstall:false,canRemove:false});
    assert($('[data-install-state="test-second"]').textContent==='Uninstalling…','Verification flashed a different uninstall status');
    assert(!$('.install-review')&&!$('[data-setup-act]')&&$('[data-device-detail="test-second"] .device-explanation')===progress,'Verification restored controls or rebuilt the progress message');
    window.testSetup({endpoint:'test-second',state:'available',installation:'not_installed',busy:false,canInstall:true,canRemove:false});await wait(30);
    assert($('[data-setup-act="install"]')&&!$('[data-setup-act="uninstall"]')&&!S.setupConfirm,'Finished removal left stale uninstall UI');
    $('[data-settings-tab="general"]').click();await wait(50);
    const preferenceWork=JSON.stringify(P().work),preferenceDirty=P().dirty;
    assert($('[data-start-with-windows]').getAttribute('aria-pressed')==='false'&&$('[data-start-in-tray]').getAttribute('aria-pressed')==='false','Startup defaults were enabled');
    $('[data-start-with-windows]').click();await wait(50);assert(STARTUP.enabled,'Start with Windows did not register');
    $('[data-start-in-tray]').click();await wait(50);assert(APP.startInTray&&APP.closeToTray,'Start in tray changed close behavior');
    window.failStartup=true;$('[data-start-with-windows]').click();await wait(50);
    assert(STARTUP.enabled&&!$('[data-start-with-windows]').disabled&&$('#toast').textContent.includes('failure'),'Startup failure claimed success or stranded the toggle');window.failStartup=false;
    $('[data-start-with-windows]').click();await wait(50);assert(!STARTUP.enabled&&APP.startInTray,'Startup toggles were coupled');
    assert(JSON.stringify(P().work)===preferenceWork&&P().dirty===preferenceDirty,'Startup preferences edited the profile');
    results.push('Startup defaults are off, controls remain independent, and registration failure preserves the actual state');
    $('[data-theme-choice="light"]').click();assert(document.documentElement.dataset.theme==='light','General appearance preference failed');
    $('[data-act="back-eq"]').click();await wait(60);assert($('#graph')&&APP.eqEnabled===false&&JSON.stringify(P().work)===workBefore,'Returning from Settings lost the editor or master state');
    results.push('Global EQ is independent of profiles; single Install; uninstall review clears on start and progress stays stable through verification');

    $('[data-act="profiles"]').click();await wait(50);
    assert(S.view==='library'&&$$('[data-profile-card]').length===3&&!$('#graph'),'Profile library did not open');
    assert($$('.profile-preview svg').length===3,'Profile previews did not load');
    assert($('[data-profile-card="test-unused"] .profile-preview').querySelectorAll('.profile-curve').length===2,'Independent channel previews were lost');
    const previewRequests=window.testRequests.filter(r=>r.op==='profile_preview').length;
    render();await wait(30);
    assert(window.testRequests.filter(r=>r.op==='profile_preview').length===previewRequests,'Unchanged thumbnails were fetched again');
    const activeCard=$('[data-profile-card="test-profile"]'),unusedCard=$('[data-profile-card="test-unused"]'),assignedCard=$('[data-profile-card="test-assigned"]');
    assert(activeCard.querySelector('[data-profile-action="delete"]').disabled&&assignedCard.querySelector('[data-profile-action="delete"]').disabled,'Assigned or disconnected profiles can be deleted');
    assert(assignedCard.textContent.includes('(disconnected)')&&unusedCard.textContent.includes('Unused <profile>'),'Usage or escaped profile names missing');
    const managementBefore=window.testRequests.length;
    activeCard.querySelector('[data-profile-action="export"]').click();await wait(40);
    assert(window.testRequests.slice(managementBefore).some(r=>r.op==='profile_export'&&r.id==='test-profile'),'Export did not target the chosen profile');
    assert(!window.testRequests.slice(managementBefore).some(r=>['save','select','edit','eq'].includes(r.op))&&JSON.stringify(P().work)===workBefore,'Export saved, switched or altered the draft');
    const toastBefore=$('#toast').textContent;window.cancelExport=true;
    activeCard.querySelector('[data-profile-action="export"]').click();await wait(40);
    assert($('#toast').textContent===toastBefore&&!profileExportBusy,'Cancelled export claimed success or left the editor blocked');window.cancelExport=false;
    unusedCard.querySelector('[data-profile-action="delete"]').click();
    assert($('.dlg').textContent.includes('Delete Unused <profile>?'),'Delete did not identify the chosen profile');
    $('[data-mb="cancel"]').click();assert(APP.profiles.some(p=>p.id==='test-unused'),'Cancel deleted a profile');
    unusedCard.querySelector('[data-profile-action="delete"]').click();$('[data-mb="delete"]').click();await wait(40);
    const deletion=window.testRequests.find(r=>r.op==='profile_delete');
    assert(deletion?.id==='test-unused'&&deletion.revision===7&&!$('[data-profile-card="test-unused"]'),'Confirmed deletion lost identity/revision or remained visible');
    await importProfileFile(new File(['CONTOUR 1 48000 2\nBypass 0\nEnd\n'],'Imported EQ.contour',{type:'text/plain'}));await wait(40);
    assert($('[data-profile-card="test-imported"]')&&APP.device===deviceBefore&&P().id===profileBefore&&JSON.stringify(P().work)===workBefore,'Import selected itself or lost the current draft');
    $('[data-act="back-eq"]').click();await wait(40);
    results.push('Profile library shows device usage, guards assigned deletion, confirms exact unused revision, exports without saving, cancels cleanly, and imports without applying');

    await openProfiles();const adaptBefore=window.testRequests.length;
    const adaptButton=$('[data-profile-card="test-assigned"] [data-prof]');
    assert(!adaptButton.disabled&&adaptButton.textContent==='Adapt a copy…','Different channel count has no adaptation action');adaptButton.click();await wait(50);
    const adaptDialog=$('.dlg'),adaptName=$('#adaptName');
    assert(S.modal.kind==='adapt'&&$('#modalHost [data-mb="go"]').disabled,'Unmapped source channels were silently discarded');
    assert($('[data-adapt-ch="0"]').textContent.includes('All 2 channels')&&$('[data-adapt-ch="1"]').textContent.includes('Choose channels'),'All/unmapped channel meaning is unclear');
    adaptName.value='Stereo room';adaptName.dispatchEvent(new Event('input'));
    $('[data-adapt-ch="1"]').click();$('[data-chopt="1"]').click();
    assert($('.dlg')===adaptDialog&&$('#adaptName')===adaptName&&adaptName.value==='Stereo room','Mapping rebuilt dialog or lost name');
    assert(S.modal.layers[1].mask===2&&!$('#modalHost [data-mb="go"]').disabled,'Mapping did not enable creation');
    key($('#pop button'),'Escape');assert(!S.pop&&S.modal,'Escape from picker dismissed adaptation');
    $('[data-adapt-ch="1"]').click();$('[data-adapt-ch="1"]').click();assert(!S.pop,'Channel button cannot close its own picker');
    window.failAdapt=true;$('[data-mb="go"]').click();await wait(40);
    assert($('.dlg')===adaptDialog&&$('#adaptError').textContent.includes('changed')&&adaptName.value==='Stereo room','Failed adaptation lost review or hid error');
    window.failAdapt=false;$('[data-mb="go"]').click();await wait(60);
    assert(!S.modal&&$('[data-profile-card="adapted-copy"]')&&P().id===profileBefore&&JSON.stringify(P().work)===workBefore,'Copy changed the original draft or selected itself');
    const adaptRequest=window.testRequests.find(r=>r.op==='profile_adapt');
    assert(adaptRequest.sourceRevision===1&&adaptRequest.draftRevision===null&&adaptRequest.target.id==='test-output'&&adaptRequest.targets[1].mask===2,'Adaptation lost identity or reviewed routing');
    assert(!window.testRequests.slice(adaptBefore).some(r=>['save','edit','select','eq'].includes(r.op)),'Adaptation changed live audio');
    window.testState.profiles[0].usedBy.push({id:'test-second',name:'Second test device',available:true,pending:true});
    const cardBefore=$('[data-profile-card="test-profile"]');window.testStatus(window.testState.status);await wait(20);
    assert($('[data-profile-card="test-profile"]')===cardBefore&&cardBefore.querySelector('.profile-usage').textContent.includes('update pending'),'Pending device state rebuilt library or was hidden');
    window.testState.profiles[0].usedBy[1].pending=false;window.testStatus(window.testState.status);await wait(20);
    assert(!cardBefore.querySelector('.profile-usage').textContent.includes('update pending'),'Confirmed delivery remained pending');
    $('[data-act="back-eq"]').click();await wait(30);
    assert($('.profile-devices').textContent==='Used on 2 devices','Editor hides multi-device profile usage');
    $('#profileBtn').click();$('[data-prof="test-assigned"]').click();await wait(40);
    assert(S.modal?.kind==='adapt','Profile dropdown did not offer adaptation');$('[data-mb="cancel"]').click();
    results.push('Adapt a copy reviews every layer, keeps controls stable, retains failed reviews, preserves source/draft/audio and reports shared-device delivery without redraw');

    await openProfiles();window.holdDeviceSwitch=true;
    const switchProfileId=P().id,switchRate=P().curveRate;
    for(const id of ['test-second','test-output']){
      const previousDevice=APP.device;$('#devBtn').click();$(`[data-device="${id}"]`).click();await wait(30);
      assert(APP.device===previousDevice&&S.modal,'Output switch did not wait for completion');
      window.testState.device=id;window.testState.closing={state:'open'};
      window.testStatus(window.testState.status,true);await wait(50);
      assert(S.view==='library'&&APP.device===id&&P().id===switchProfileId&&P().curveRate===switchRate,'Shared-profile switch left Profiles or changed the profile');
      assert($('#devBtn .device-name').textContent===D().name&&!S.modal,'Completed output switch left the Profiles dropdown stale');
      $('#devBtn').click();assert($(`[data-device="${id}"] .ck svg`),'Output menu checkmark still points to the previous device');$('#devBtn').click();
      const unchangedCard=$('[data-profile-card="test-profile"]'),requests=window.testRequests.length;
      window.testStatus(window.testState.status,true);await wait(20);
      assert($('[data-profile-card="test-profile"]')===unchangedCard&&!window.testRequests.slice(requests).some(r=>r.op==='state'),'Unchanged status rebuilt Profiles');
    }
    window.holdDeviceSwitch=false;
    results.push('Asynchronous output switches refresh Profiles even with the same profile/rate; unchanged status keeps cards stable');

    window.testState.recoveries=[{id:'recover-1',name:'Earlier EQ',device:'Offline test output'},{id:'recover-2',name:'Another EQ',device:'Second test device'}];
    APP.recoveries=structuredClone(window.testState.recoveries);await openProfiles();
    const recoveryBefore=window.testRequests.length;
    $('[data-recovery="keep"]').click();await wait(60);
    assert(APP.recoveries.length===1&&$('[data-profile-card="kept-recovery"]'),'Keep did not create a separate profile');
    assert(P().id===profileBefore&&JSON.stringify(P().work)===workBefore&&!window.testRequests.slice(recoveryBefore).some(r=>['edit','save','select','eq'].includes(r.op)),'Keeping recovery changed current EQ');
    $('[data-recovery="discard"]').click();$('[data-mb="cancel"]').click();assert(APP.recoveries.length===1,'Cancel discarded recovery');
    $('[data-recovery="discard"]').click();$('[data-mb="discard"]').click();await wait(40);assert(!$('.recovery-list'),'Discard left recovery visible');
    results.push('Recovery is reviewed separately, kept without applying, or discarded with confirmation');

    await openSettings('outputs');await wait(40);
    const engineOffer={canUpdate:true,engineState:'update_available',engineDevices:['Offline test output','Second test device']};
    acceptSetup({endpoint:'test-output',state:'attached',installation:'installed',canRemove:true,...engineOffer});
    acceptSetup({endpoint:'test-second',state:'available',installation:'not_installed',canInstall:true,...engineOffer});
    assert($('.engine-update').textContent.includes('all installed devices')&&$('.engine-update').textContent.includes('Second test device'),'Engine update concealed its shared scope');
    acceptSetup({endpoint:'test-disconnected',state:'blocked',installation:'not_installed',...engineOffer,canUpdate:false});
    assert($('[data-engine-act="update"]'),'Checking an ineligible device hid the shared update');
    acceptSetup({endpoint:'test-disconnected',state:'blocked',installation:'not_installed',engineState:'current',canUpdate:false});
    assert(!$('.engine-update'),'A newer shared engine status left an older device update offer visible');
    acceptSetup({endpoint:'test-second',state:'available',installation:'not_installed',canInstall:true,...engineOffer});
    assert(!S.settingsDevice&&!$('.device-detail:not([hidden])')&&$$('.engine-update').length===1&&!$('.engine-update').closest('.device-entry'),'Shared update depends on an expanded device row or is duplicated');
    assert($('.engine-update').getBoundingClientRect().bottom<$('.device-columns').getBoundingClientRect().top,'Shared update is not above the device list');
    const offerNode=$('.engine-update');
    $('[data-manage-device="test-output"]').click();$('[data-manage-device="test-output"]').click();
    assert($('.engine-update')===offerNode&&!S.settingsDevice,'Expanding or collapsing a device rebuilt the shared update');
    window.holdInspection=true;setupRequest('Inspect','test-second');await wait(30);
    $('[data-engine-act="update"]').click();await wait(20);
    assert(pendingSetup?.action==='Update'&&$('[data-engine-act="cancel"]'),'Global update cannot wait for a running device check');
    $('[data-engine-act="cancel"]').click();
    assert(!setupOperation&&!pendingSetup&&$('[data-engine-act="update"]'),'Queued global update cannot be cancelled with all rows collapsed');
    window.holdUpdate=true;const engineDraft=JSON.stringify(P().work),beforeUpdate=window.testRequests.length;
    $('[data-engine-act="update"]').click();await wait(20);
    assert(!window.testRequests.slice(beforeUpdate).some(r=>r.action==='Update'),'Global update overlapped the running inspection');
    window.holdInspection=false;window.testSetup({endpoint:'test-second',state:'available',installation:'not_installed',canInstall:true,...engineOffer});await wait(40);
    const engineRequests=window.testRequests.slice(beforeUpdate).filter(r=>r.op==='setup'&&r.action==='Update');
    assert(engineRequests.length===1&&engineRequests[0].endpoint==='test-second'&&!S.settingsDevice,'Global action did not use its latest inspected installation independently of the open row');
    const engineProgress=$('.engine-update');
    assert($('#engineUpdateTitle').textContent==='Updating the audio engine…'&&!$('[data-engine-act]'),'Engine update lost its global progress or left stale actions');
    window.testSetup({endpoint:'test-second',state:'checking',busy:true,canInstall:false,canRemove:false});
    assert($('.engine-update')===engineProgress&&$('#engineUpdateTitle').textContent==='Updating the audio engine…','Verification rebuilt or changed the global update progress');
    window.holdUpdate=false;window.testSetup({endpoint:'test-second',state:'available',installation:'not_installed',engineState:'current',busy:false,canInstall:true,canUpdate:false});await wait(40);
    assert(!$('.engine-update')&&JSON.stringify(P().work)===engineDraft,'Engine update lost a draft or retained its offer');
    acceptSetup({endpoint:'test-output',state:'recovery',installation:'incomplete',engineState:'recovery',canRecoverUpdate:true,engineDevices:['Offline test output']});
    assert($('[data-engine-act="recover"]')&&!S.settingsDevice&&!$('[data-setup-act="uninstall"]'),'Interrupted update needs an open device row or offers device uninstall');
    $('[data-engine-act="recover"]').click();await wait(40);
    assert(window.testRequests.some(r=>r.op==='setup'&&r.action==='RecoverUpdate'),'Engine recovery did not use its fixed action');
    $('[data-act="back-eq"]').click();await wait(40);
    results.push('One shared update above collapsed device rows; queue/cancel and recovery independent of row selection; latest inspection targets one action; stable global progress and drafts');

    // Installed/idle is editable. A confirmed uninstall wins over stale live
    // status, hides both empty-profile and existing-profile controls, and never
    // discards the draft. Keyboard shortcuts cannot bypass the installation UI.
    const draft=JSON.stringify(P().work),profile=structuredClone(P()),revision=P().revision;
    acceptSetup({endpoint:'test-output',state:'attached',installation:'installed',canRemove:true});
    window.testStatus({endpoint:'waiting_for_audio',running:'unavailable'});
    assert($('#graph')&&!$('.installation-view'),'Installed idle device was blocked');
    acceptSetup({endpoint:'test-output',state:'available',installation:'not_installed',canInstall:true});
    assert($('.installation-view')&&!$('#graph')&&!$('#profileBtn')&&!$('[data-act="new-profile"]')&&!$('[data-act="eq"]'),'Uninstalled device retained EQ controls');
    assert($('#installationTitle').textContent==='Contour isn’t installed on this device','Missing installation was not explained');
    const blockedCount=window.testRequests.length;
    for(const value of ['s','z','y'])document.body.dispatchEvent(new KeyboardEvent('keydown',{key:value,ctrlKey:true,bubbles:true,cancelable:true}));
    key(document.body,'Delete');addBand(F('PK',1200,4,1));await wait(40);
    assert(window.testRequests.length===blockedCount,'Blocked editor issued a command: '+JSON.stringify(window.testRequests.slice(blockedCount)));
    assert(JSON.stringify(P().work)===draft&&P().revision===revision,'Blocked editor still changed the profile');
    window.testStatus({endpoint:'ready',running:'processed_match',applied:true});
    assert($('.installation-view')&&!$('#graph'),'Stale engine report overrode confirmed removal');
    const gatedNode=$('.installation-view');window.testStatus({endpoint:'waiting_for_audio',running:'unavailable'});
    assert($('.installation-view')===gatedNode,'Runtime updates rebuilt the installation screen');
    $('[data-act="device-setup"]').click();await wait(60);
    assert(S.view==='settings'&&S.settingsDevice==='test-output'&&$('[data-device-detail="test-output"]')&&!$('[data-device-detail="test-output"]').hidden,'Installation action did not open the selected device');
    assert(APP.device==='test-output'&&JSON.stringify(P().work)===draft,'Opening installation changed the draft or output');
    $('[data-act="back-eq"]').click();
    // A device without a profile must still show installation first.
    APP.editor=null;render();assert($('.installation-view')&&!$('[data-act="new-profile"]'),'No-profile welcome bypassed installation');
    acceptSetup({endpoint:'test-output',state:'attached',installation:'installed',canRemove:true});
    assert($('[data-act="new-profile"]')&&!$('.installation-view'),'Installed device did not offer profile creation');
    APP.editor=profile;render();assert($('#graph')&&JSON.stringify(P().work)===draft,'Restoring installation lost the existing draft');
    results.push('Installation gates profile creation, editing, global-toggle display and shortcuts; idle installed outputs stay editable; device setup targets the current output and preserves drafts');

    // No profile means there is no EndpointSession yet. Inspect the selected
    // output independently instead of mistaking that for an installed device.
    APP.device='test-second';APP.editor=null;APP.status={endpoint:'unavailable'};
    setupReports.delete(APP.device);setupCache.delete(APP.device);
    editorChecks.set(APP.device,{stamp:setupDeviceStamp(D()),time:Date.now()});
    window.holdInspection=true;const checksBefore=window.testRequests.length;render();await wait(40);
    assert(editorGate()==='checking'&&$('.installation-view')&&!$('[data-act="new-profile"]'),'Unknown installation exposed profile creation');
    const inspection=window.testRequests.slice(checksBefore).filter(r=>r.op==='setup');
    assert(inspection.length===1&&inspection[0].action==='Inspect'&&inspection[0].endpoint==='test-second','Selected-device check was skipped or scanned unrelated devices');
    window.holdInspection=false;window.testSetup({endpoint:'test-second',state:'blocked',installation:'unknown',reason:'inspection_failed',busy:false,canInstall:false});
    assert(editorGate()==='unknown'&&$('#installationTitle').textContent==='Couldn’t check this device','Failed check claimed the device was uninstalled');
    window.testSetup({endpoint:'test-second',state:'blocked',installation:'not_installed',reason:'existing_effects',busy:false,canInstall:false});
    assert($('.installation-view').textContent.includes('existing audio effects')&&$('[data-act="device-setup"]').textContent==='Manage device','Unsupported device offered installation');
    results.push('Unassigned outputs get one selected-device inspection; invalidated reports are rechecked; unknown and unsupported results stay distinct');
    finish();
  }catch(error){finish(error);}
};
const mime={'.js':'text/javascript','.css':'text/css','.ttf':'font/ttf','.svg':'image/svg+xml'};
const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://localhost');
  if(url.pathname==='/'){
    const html=fs.readFileSync(path.join(ui,'index.html'),'utf8').replace('<script src="boot.js">','<script src="fixture.js"></script><script src="boot.js">').replace('</body>',preview?'</body>':'<script src="checks.js"></script></body>');
    res.setHeader('Content-Type','text/html');res.end(html);return;
  }
  if(url.pathname==='/fixture.js'){res.setHeader('Content-Type','text/javascript');res.end(`(${fixture.toString()})();`);return;}
  if(url.pathname==='/checks.js'){res.setHeader('Content-Type','text/javascript');res.end(`(${checks.toString()})();`);return;}
  const relative=url.pathname.slice(1);if(!['boot.js','editor.js','settings.js','app.js','interactions.js','styles.css','wordmark.svg','fonts/Manrope.ttf'].includes(relative)){res.writeHead(404).end();return;}
  res.setHeader('Content-Type',mime[path.extname(relative)]);res.end(fs.readFileSync(path.join(ui,relative)));
});
server.listen(0,'127.0.0.1',()=>{
  if(preview){console.log(`Offline UI fixture: http://127.0.0.1:${server.address().port}/`);return;}
  const child=spawn(edge,['--headless=new','--disable-gpu','--no-first-run','--disable-background-networking','--disable-extensions','--disable-sync',`--user-data-dir=${profile}`,'--dump-dom','--virtual-time-budget=12000',`http://127.0.0.1:${server.address().port}/`],{windowsHide:true});
  let stdout='',stderr='';child.stdout.on('data',b=>stdout+=b);child.stderr.on('data',b=>stderr+=b);
  const timeout=setTimeout(()=>{child.kill();},45000);
  child.on('error',e=>{clearTimeout(timeout);server.close();console.error(e);process.exitCode=1;});
  child.on('exit',()=>{
    clearTimeout(timeout);server.close();
    const match=stdout.match(/<pre id="test-results">([\s\S]*?)<\/pre>/);
    if(!match){console.error('Browser did not finish interaction checks:',stderr.slice(-1800));process.exitCode=1;return;}
    const result=JSON.parse(match[1].replaceAll('&amp;','&').replaceAll('&lt;','<').replaceAll('&gt;','>').replaceAll('&quot;','"'));
    for(const check of result.checks)console.log('PASS '+check);
    if(!result.passed){console.error(result.error);process.exitCode=1;}
  });
});
