// Exercise the real frontend's asynchronous editing flow with delayed host
// replies. Rendering is replaced; native DSP/storage are covered by app_tests.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path'),assert=require('node:assert/strict');
const directory=process.argv[2];
const posts=[],handlers={},events={};
const frames=[];
let messageHandler;
const context=vm.createContext({setTimeout,clearTimeout,console,requestAnimationFrame:fn=>{frames.push(fn);return frames.length;},
  localStorage:{getItem:()=>null,setItem:()=>{}},
  document:{documentElement:{dataset:{}},activeElement:{blur(){}},querySelector:()=>null,querySelectorAll:()=>[],addEventListener:(n,fn)=>(handlers[n]??=[]).push(fn)},
  window:{addEventListener:(n,fn)=>events[n]=fn,chrome:{webview:{addEventListener:(_,fn)=>messageHandler=fn,postMessage:body=>posts.push(JSON.parse(body))}}}
});
const evaluate=s=>vm.runInContext(s,context);
const copy=o=>JSON.parse(JSON.stringify(o));
const turn=()=>new Promise(r=>setImmediate(r));
let state={device:'output',devices:[{id:'output',name:'Output',channels:2,rate:48000,mask:3,available:true}],profiles:[],closing:{state:'open'},status:{endpoint:'not_attached'},
  editor:{id:'profile',name:'Profile',channels:2,rate:48000,curveRate:48000,revision:1,dirty:false,undo:false,redo:false,work:{bypass:false,layers:[{name:'Layer',all:true,mask:0,gain:0,on:true,filters:[]}]}}};
function reply(request,data=state){messageHandler({data:{requestId:request.requestId,ok:true,data:copy(data)}});}
async function take(op){await turn();const request=posts.shift();assert.equal(request?.op,op);return request;}
async function main(){
  // Compiling all files together also catches duplicate global bindings that
  // separate syntax checks miss in classic scripts loaded by index.html.
  const scripts=['editor.js','settings.js','app.js','interactions.js'].map(f=>fs.readFileSync(path.join(directory,f),'utf8')).join('\n');
  new vm.Script(scripts).runInContext(context);
  evaluate('render=()=>{}; refresh=()=>{}; refreshChrome=()=>{}; statusbar=()=>{}; renderOver=()=>{}; toast=()=>{};');
  reply(await take('state'));await turn();
  const theme=await take('window');assert.equal(theme.action,'theme');assert.equal(theme.theme,'dark');
  reply(theme,{custom:true,maximized:false});await turn();

  // Entity IDs must not overwrite the bridge's numeric reply identity. A lost
  // reply here used to strand every later command, including window Close.
  evaluate("switchProfile('profile');");
  const selected=await take('select');
  assert.ok(Number.isSafeInteger(selected.requestId),'Profile selection overwrote the request identity');
  assert.equal(selected.id,'profile');reply(selected);await turn();
  evaluate("command('device',{id:'output'},true);");
  const chosenOutput=await take('device');
  assert.ok(Number.isSafeInteger(chosenOutput.requestId),'Output selection overwrote the request identity');
  assert.equal(chosenOutput.id,'output');reply(chosenOutput);await turn();
  evaluate("command('select',{id:'missing-profile'},true);");
  const rejected=await take('select');
  messageHandler({data:{requestId:rejected.requestId,ok:false,error:'Unknown profile',state:copy(state)}});
  await turn();
  evaluate("rpc('window',{action:'close'});");
  const stillResponsive=await take('window');assert.equal(stillResponsive.action,'close');
  reply(stillResponsive,{custom:true,maximized:false});await turn();

  // A resize burst should schedule one graph paint and must not repeatedly
  // destroy the caption DOM (including focused window controls).
  let captionWrites=0;
  context.document.querySelector=s=>s==='.caption'?{set outerHTML(_){captionWrites++;}}:null;
  for(let i=0;i<200;i++){
    messageHandler({data:{event:'window',data:{custom:true,maximized:false}}});events.resize();
  }
  assert.equal(captionWrites,1,'Ordinary resize rebuilt the caption');
  assert.equal(frames.length,1,'Resize queued more than one graph paint per frame');
  frames.shift()();
  messageHandler({data:{event:'window',data:{custom:true,maximized:true}}});
  assert.equal(captionWrites,2,'Maximize did not update the window controls');
  context.document.querySelector=()=>null;

  // Empty title-bar space asks the host for a native move and the system menu;
  // presses on title-bar controls stay with the page.
  const press=onControl=>({button:0,pointerId:1,preventDefault(){},target:{closest:s=>s==='button,input'?(onControl?{}:null):s==='.tb'?{hasPointerCapture:()=>false}:null}});
  handlers.pointerdown.forEach(fn=>fn(press(false)));const move=await take('window');assert.equal(move.action,'move');reply(move,{custom:true,maximized:false});
  handlers.pointerdown.forEach(fn=>fn(press(true)));await turn();assert.equal(posts.length,0,'A title-bar control started a window move');
  handlers.contextmenu.forEach(fn=>fn(press(false)));const menu=await take('window');assert.equal(menu.action,'menu');reply(menu,{custom:true,maximized:false});

  for(let i=0;i<200;i++)evaluate(`grid(${300+i});`);
  assert.ok(evaluate('grids.size')<=8,'Continuous resize retained unbounded frequency grids');
  assert.equal(evaluate("parse('freq','3.2','kHz')"),3200);
  assert.equal(evaluate("parse('freq','3200 Hz','kHz')"),3200);
  assert.equal(evaluate("parse('q','-1','')"),null);

  evaluate('P().work.layers[0].gain=-3; queueEdit(); flush();');const first=await take('edit');
  evaluate('P().work.layers[0].gain=-6; queueEdit();');
  const stale=copy(state);delete stale.editor.work;
  messageHandler({data:{event:'status',data:stale}});
  assert.equal(evaluate('P().work.layers[0].gain'),-6,'Status replaced an unsent newer edit');
  state.editor.work=first.work;state.editor.revision++;state.editor.dirty=true;reply(first);await turn();
  assert.equal(evaluate('P().work.layers[0].gain'),-6,'Older edit acknowledgement replaced newer drag');
  evaluate('flush();');const second=await take('edit');assert.equal(second.work.layers[0].gain,-6);
  state.editor.work=second.work;state.editor.revision++;reply(second);await turn();
  assert.equal(evaluate('P().work.layers[0].gain'),-6);

  // Save while a numeric field still has focus must commit that field first.
  const input={matches:s=>s.includes('input'),id:'',dataset:{},blur:()=>evaluate('P().work.layers[0].gain=-8; queueEdit();')};
  for(const handler of handlers.keydown)handler({target:input,key:'s',ctrlKey:true,preventDefault(){}});
  const edit=await take('edit');assert.equal(edit.work.layers[0].gain,-8);
  state.editor.work=edit.work;reply(edit);const save=await take('save');
  state.editor.dirty=false;reply(save);await turn();

  // Closing also flushes a focused field before submitting a close decision.
  context.document.activeElement={blur:()=>evaluate('P().work.layers[0].gain=-10; queueEdit();')};
  messageHandler({data:{event:'close'}});
  const finalEdit=await take('edit');assert.equal(finalEdit.work.layers[0].gain,-10);
  state.editor.work=finalEdit.work;reply(finalEdit);reply(await take('close'));await turn();
  assert.equal(posts.length,0);

  // Device inspection can target a row without switching the editor output.
  context.document.activeElement={blur(){}};
  evaluate("S.settingsDevice='output';setupRequest('Inspect','output');");
  const inspectSetup=await take('setup');assert.equal(inspectSetup.action,'Inspect');assert.equal(inspectSetup.endpoint,'output');
  reply(inspectSetup,{endpoint:'output',state:'checking',busy:true});await turn();
  messageHandler({data:{event:'setup',data:{endpoint:'output',state:'available',busy:false,canInstall:true,canRemove:false}}});
  assert.ok(evaluate("deviceDetails(D(),setupReports.get('output'))").includes('Audio will briefly stop'));
  evaluate("setupAct('install');setupAct('install');");
  const installSetup=await take('setup');assert.equal(installSetup.action,'Install');assert.equal(installSetup.endpoint,'output');
  assert.equal(posts.length,0,'Repeated click started a second installation');
  assert.equal(evaluate('S.setupConfirm'),null,'Install added another confirmation');
  reply(installSetup,{endpoint:'output',state:'installing',busy:true});await turn();
  state.eqEnabled=false;evaluate('APP.eqEnabled=false;');
  messageHandler({data:{event:'setup',data:{endpoint:'output',state:'attached',busy:false,canInstall:false,canRemove:true}}});
  await new Promise(resolve=>setTimeout(resolve,5));
  const masterAfterInstall=await take('eq');assert.equal(masterAfterInstall.enabled,false,'Installation re-enabled globally bypassed EQ');reply(masterAfterInstall);await turn();
  assert.ok(evaluate("deviceMessage(D(),setupReports.get('output'))").includes('no playback is needed'),'Installed still asks the user to play audio');
  evaluate("APP.devices[0].connection={confirmed:true};");
  assert.equal(evaluate("statusSummary({endpoint:'waiting_for_audio'},P(),false).label"),'EQ bypassed');
  assert.equal(evaluate("compatibilityLabel(D(),setupReports.get('output'))"),'Confirmed');
  evaluate("APP.devices[0].connection={confirmed:false,detail:'Cannot connect'};");
  assert.equal(evaluate("statusSummary({endpoint:'waiting_for_audio'},P(),false).label"),'Check connection');
  assert.ok(evaluate("deviceMessage(D(),setupReports.get('output'))").includes('could not connect'));
  evaluate("APP.devices[0].connection={confirmed:false,checking:true};");
  assert.equal(evaluate("statusSummary({endpoint:'waiting_for_audio',pending:true},P(),false).label"),'Connecting');
  evaluate("setupAct('uninstall');");assert.equal(posts.length,0,'Uninstall skipped review');
  assert.ok(evaluate("deviceDetails(D(),setupReports.get('output'))").includes('Uninstall Contour from this device?'));
  evaluate("setupAct('cancel');");assert.equal(evaluate('S.setupConfirm'),null);
  evaluate("setupAct('uninstall');setupAct('confirm');");
  const removeSetup=await take('setup');assert.equal(removeSetup.action,'Remove');
  assert.equal(evaluate('S.setupConfirm'),null,'Removal kept its completed confirmation');
  reply(removeSetup,{endpoint:'output',state:'removing',busy:true});await turn();
  messageHandler({data:{event:'setup',data:{endpoint:'output',state:'checking',busy:true,canInstall:false,canRemove:false}}});
  assert.equal(evaluate("installationLabel(setupReports.get('output'))"),'Uninstalling…','Verification interrupted the uninstall status');
  assert.ok(!evaluate("deviceDetails(D(),setupReports.get('output'))").includes('data-setup-act='),'Verification restored device actions');
  messageHandler({data:{event:'setup',data:{endpoint:'output',state:'available',busy:false,canInstall:true,canRemove:false}}});
  assert.equal(evaluate('setupOperation'),null,'Removal never finished');
  assert.ok(evaluate("deviceDetails(D(),setupReports.get('output'))").includes('data-setup-act="install"'),'Completed removal did not restore Install');

  // Continuous input must publish before it stops, and a slow host must not
  // accumulate a queue of obsolete parameter snapshots.
  const interval=setInterval(()=>evaluate('P().work.layers[0].gain-=.1; queueEdit();'),5);
  await new Promise(r=>setTimeout(r,70));
  const during=await take('edit');assert.ok(during,'Continuous drag starved native preview');
  await new Promise(r=>setTimeout(r,45));clearInterval(interval);
  assert.equal(posts.length,0,'Slow host accumulated obsolete edits');
  state.editor.work=during.work;reply(during);
  const latest=await take('edit');assert.ok(latest.work.layers[0].gain<during.work.layers[0].gain);
  state.editor.work=latest.work;reply(latest);await turn();
  assert.equal(posts.length,0);

  // Sidebar actions on another layer keep the current layer selected.
  evaluate("edit=fn=>{fn();fixSel();};P().work.layers=[Ly('A',true,0,0,true,[]),Ly('B',true,0,0,true,[]),Ly('C',true,0,0,true,[])];S.sel=1;layerAction(0,'down');");
  assert.equal(evaluate('L().name'),'B');assert.equal(evaluate('S.sel'),0);
  evaluate("layerAction(1,'trash');");assert.equal(evaluate('L().name'),'B');
  evaluate("layerAction(0,'trash');");assert.equal(evaluate('L().name'),'C');
  evaluate("layerAction(0,'trash');");assert.equal(evaluate('LAYERS().length'),0);
  console.log('PASS scripts load; profile/output reply identity and rejected-command recovery; resize coalescing/caption stability/cache bounds; units; stale replies; Save/close ordering; setup lifecycle; continuous preview; sidebar selection');
}
main().catch(e=>{console.error(e);process.exit(1);});
