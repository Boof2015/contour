// Delayed native replies exercise scheduling without touching real devices.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path'),assert=require('node:assert/strict');
const posts=[],handlers={};let receive,blurs=0;
const context=vm.createContext({console,setTimeout,clearTimeout,requestAnimationFrame:()=>0,
  localStorage:{getItem:()=>null,setItem:()=>{}},
  document:{documentElement:{dataset:{}},activeElement:{blur(){blurs++;}},querySelector:()=>null,querySelectorAll:()=>[],addEventListener:(n,fn)=>(handlers[n]??=[]).push(fn)},
  window:{addEventListener(){},chrome:{webview:{addEventListener:(_,fn)=>receive=fn,postMessage:body=>posts.push(JSON.parse(body))}}}});
const evaluate=code=>vm.runInContext(code,context),wait=()=>new Promise(r=>setTimeout(r,5));
const ids=[1,2,3,4].map(n=>`{0.0.0.00000000}.{00000000-0000-0000-0000-00000000000${n}}`);
const state={device:ids[2],devices:ids.map((id,i)=>({id,name:`Device ${i}`,available:true,state:1,channels:2,rate:48000,mask:3})),profiles:[],editor:null,status:{endpoint:'not_attached'},closing:{state:'open'}};
const reply=(request,data)=>receive({data:{requestId:request.requestId,ok:true,data:structuredClone(data)}});
const report=(endpoint,extra={})=>receive({data:{event:'setup',data:{endpoint,state:'available',busy:false,canInstall:true,canRemove:false,...extra}}});
async function take(op){for(let n=0;n<40&&!posts.length;n++)await wait();const request=posts.shift();assert.equal(request?.op,op);return request;}
async function inspect(id){const request=await take('setup');assert.equal(request.action,'Inspect');assert.equal(request.endpoint,id);reply(request,{endpoint:id,state:'checking',busy:true,canInstall:false,canRemove:false});await wait();return request;}
async function refresh(force=false){evaluate(`refreshDevices(${force});`);reply(await take('devices'),state);await wait();}
async function main(){
  const scripts=['editor.js','settings.js','app.js','interactions.js'].map(f=>fs.readFileSync(path.join(process.argv[2],f),'utf8')).join('\n');
  new vm.Script(scripts).runInContext(context);
  evaluate('render=()=>{};refresh=()=>{};refreshChrome=()=>{};statusbar=()=>{};renderOver=()=>{};toast=()=>{};');
  reply(await take('state'),state);reply(await take('window'),{custom:false,maximized:false});await wait();
  evaluate("S.view='settings';gesture=true;");
  await refresh();await inspect(ids[2]);
  assert.equal(blurs,0,'Background inspection blurred the current input');
  assert.equal(evaluate('gesture'),true,'Background inspection ended an editing gesture');
  evaluate('gesture=false;');
  evaluate(`prioritizeCheck(${JSON.stringify(ids[1])});`);
  report(ids[2]);await inspect(ids[1]);
  const details=evaluate(`deviceDetails(APP.devices[2],setupReports.get(APP.device))`);
  assert.match(details,/data-setup-act="install"\s*>/,'Another device check disabled Install on a completed row');
  evaluate(`S.settingsDevice=APP.device;setupAct('install');setupAct('install');`);
  assert.equal(posts.length,0,'A mutation started concurrently with inspection');
  assert.equal(evaluate('pendingSetup.endpoint'),ids[2]);
  evaluate("setupAct('cancel-pending');");
  assert.equal(evaluate('pendingSetup'),null,'Queued action could not be cancelled');
  report(ids[1]);await inspect(ids[0]);
  evaluate("setupAct('install');");
  report(ids[0]);const install=await take('setup');
  assert.equal(install.action,'Install','An explicit action waited behind the rest of the scan');
  assert.equal(install.endpoint,ids[2]);
  reply(install,{endpoint:ids[2],state:'installing',busy:true});await wait();
  evaluate("S.view='editor';");
  report(ids[2],{notice:'Administrator approval was cancelled.'});await wait();
  assert.equal(evaluate('setupOperation'),null,'Cancelled approval left setup blocked');
  assert.equal(posts.length,0,'Scanning continued after leaving Settings');
  console.log('PASS selected output first; row priority; background checks preserve focus/gestures; completed rows usable; explicit action takes next slot; cancellation');

  // Fresh success and stable compatibility results survive reopening; failures
  // are retryable. Device state/format changes invalidate only affected rows.
  ids.forEach(id=>report(id));
  report(ids[3],{state:'blocked',reason:'another_device',canInstall:false});await wait();
  evaluate("S.view='settings';S.settingsDevice=null;");
  const before=blurs;await refresh();assert.equal(posts.length,0,'Reopening rechecked fresh devices');assert.equal(blurs,before);
  state.devices[1].rate=96000;await refresh();await inspect(ids[1]);report(ids[1]);await wait();
  assert.equal(posts.length,0,'Format change rechecked unrelated devices');
  report(ids[0],{state:'blocked',reason:'inspection_failed',canInstall:false});
  await refresh();await inspect(ids[0]);report(ids[0]);await wait();
  evaluate('for(const cached of setupCache.values())cached.time-=60001;');
  await refresh();await inspect(ids[2]);
  evaluate("S.view='editor';");report(ids[2]);await wait();
  assert.equal(posts.length,0,'Navigation did not pause the scan queue');
  ids.forEach(id=>report(id));await wait();
  evaluate("S.view='settings';");await refresh(true);await inspect(ids[2]);
  evaluate("S.view='editor';");report(ids[2]);await wait();
  assert.equal(posts.length,0);
  console.log('PASS recent checks reused; format invalidation; inspection failures retried; cache expiry; manual Refresh forces checks; navigation pauses background work');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
