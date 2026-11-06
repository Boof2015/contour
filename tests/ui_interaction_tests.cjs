// Real Chromium DOM/layout regression checks with a fake native host. The host
// never enumerates devices, writes the profile library, or touches live audio.
const fs=require('node:fs'),path=require('node:path'),http=require('node:http'),{spawn}=require('node:child_process');
const ui=path.resolve(process.argv[2]),work=path.resolve(process.argv[3]),preview=process.argv.includes('--preview');
const edge=[process.env.CONTOUR_TEST_BROWSER,path.join(process.env['ProgramFiles(x86)']||'', 'Microsoft/Edge/Application/msedge.exe'),path.join(process.env.ProgramFiles||'', 'Microsoft/Edge/Application/msedge.exe')].find(p=>p&&fs.existsSync(p));
if(!edge)throw Error('Microsoft Edge is required for the UI interaction checks');
fs.mkdirSync(work,{recursive:true});
const profile=fs.mkdtempSync(path.join(work,'browser-'));
const fixture=()=>{
  const filter=(gain=0)=>({type:'PK',on:true,freq:1000,gain,q:1});
  const state={device:'test-output',devices:[{id:'test-output',name:'Offline test output',channels:2,rate:48000,mask:3,available:false}],profiles:[],closing:{state:'open'},status:{endpoint:'ready',applied:true,running:'processed_match',saved:'matches',copy:'current'},editor:{id:'test-profile',name:'Test profile',channels:2,rate:48000,curveRate:48000,revision:1,dirty:false,undo:false,redo:false,work:{bypass:false,layers:['A','B','C'].map(name=>({name,all:true,mask:0,gain:0,on:true,filters:[filter()]}))}}};
  state.profiles=[{id:'test-profile',name:'Test profile',channels:2}];
  state.eqEnabled=true;state.masterIssues=[];state.devices[0].available=true;
  state.devices.push({id:'test-second',name:'Second test device',channels:2,rate:48000,mask:3,available:true},{id:'test-disconnected',name:'Disconnected test device',channels:0,rate:0,mask:0,available:false,state:8});
  let receive;window.testEdits=[];window.testRequests=[];window.testState=state;
  window.chrome.webview={addEventListener:(_,fn)=>receive=fn,postMessage:body=>{
    const r=JSON.parse(body);window.testRequests.push(r);let data=state;
    if(r.op==='window')data={custom:false,maximized:false};
    if(r.op==='edit'){state.editor.work=r.work;state.editor.revision++;state.editor.dirty=true;window.testEdits.push(structuredClone(r.work));}
    if(r.op==='eq')state.eqEnabled=r.enabled;
    if(r.op==='setup')data={endpoint:r.endpoint,busy:false,state:r.action==='Install'?'attached':'available',installation:r.action==='Install'?'installed':'not_installed',canInstall:r.action!=='Install',canRemove:r.action==='Install'};
    if(r.op==='setup'&&r.action==='Remove'&&window.holdRemoval)data={endpoint:r.endpoint,busy:true,state:'removing',canInstall:false,canRemove:false};
    if(r.op==='setup'&&r.action==='Inspect'&&window.holdInspection)data={endpoint:r.endpoint,busy:true,state:'checking',canInstall:false,canRemove:false};
    if(r.op==='import')data={valid:true,issues:[],layer:{name:'Imported',all:true,mask:0,gain:0,on:true,filters:[filter(-3)]}};
    if(!Number.isSafeInteger(r.requestId))throw Error('Missing numeric bridge request identity');
    setTimeout(()=>receive({data:{requestId:r.requestId,ok:true,data:structuredClone(data)}}),0);
  }};
  window.testStatus=status=>receive({data:{event:'status',data:{...structuredClone(state),status}}});
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
    $('[data-settings-tab="general"]').click();$('[data-theme-choice="light"]').click();assert(document.documentElement.dataset.theme==='light','General appearance preference failed');
    $('[data-act="back-eq"]').click();assert($('#graph')&&APP.eqEnabled===false&&JSON.stringify(P().work)===workBefore,'Returning from Settings lost the editor or master state');
    results.push('Global EQ is independent of profiles; single Install; uninstall review clears on start and progress stays stable through verification');
    finish();
  }catch(error){finish(error);}
};
const mime={'.js':'text/javascript','.css':'text/css','.ttf':'font/ttf'};
const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://localhost');
  if(url.pathname==='/'){
    const html=fs.readFileSync(path.join(ui,'index.html'),'utf8').replace('<script src="boot.js">','<script src="fixture.js"></script><script src="boot.js">').replace('</body>',preview?'</body>':'<script src="checks.js"></script></body>');
    res.setHeader('Content-Type','text/html');res.end(html);return;
  }
  if(url.pathname==='/fixture.js'){res.setHeader('Content-Type','text/javascript');res.end(`(${fixture.toString()})();`);return;}
  if(url.pathname==='/checks.js'){res.setHeader('Content-Type','text/javascript');res.end(`(${checks.toString()})();`);return;}
  const relative=url.pathname.slice(1);if(!['boot.js','editor.js','settings.js','app.js','interactions.js','styles.css','fonts/Manrope.ttf'].includes(relative)){res.writeHead(404).end();return;}
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
