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
let state={device:'output',devices:[{id:'output',name:'Output',channels:2,rate:48000,mask:3}],profiles:[],closing:{state:'open'},status:{endpoint:'not_attached'},
  editor:{id:'profile',name:'Profile',channels:2,rate:48000,curveRate:48000,revision:1,dirty:false,undo:false,redo:false,work:{bypass:false,layers:[{name:'Layer',all:true,mask:0,gain:0,on:true,filters:[]}]}}};
function reply(request,data=state){messageHandler({data:{id:request.id,ok:true,data:copy(data)}});}
async function take(op){await turn();const request=posts.shift();assert.equal(request?.op,op);return request;}
async function main(){
  // Compiling all files together also catches duplicate global bindings that
  // separate syntax checks miss in classic scripts loaded by index.html.
  const scripts=['editor.js','app.js','interactions.js'].map(f=>fs.readFileSync(path.join(directory,f),'utf8')).join('\n');
  new vm.Script(scripts).runInContext(context);
  evaluate('render=()=>{}; refresh=()=>{}; refreshChrome=()=>{}; statusbar=()=>{}; renderOver=()=>{}; toast=()=>{};');
  reply(await take('state'));await turn();
  const theme=await take('window');assert.equal(theme.action,'theme');assert.equal(theme.theme,'dark');
  reply(theme,{custom:true,maximized:false});await turn();

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
  console.log('PASS scripts load; resize coalescing/caption stability/cache bounds; units; stale replies; Save/close ordering; continuous preview; sidebar selection');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
