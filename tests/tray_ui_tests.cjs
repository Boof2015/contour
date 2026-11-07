// The actual tray document with an isolated editor relay; no audio endpoints.
const fs=require('node:fs'),path=require('node:path'),http=require('node:http'),{spawn}=require('node:child_process');
const ui=path.resolve(process.argv[2]),work=path.resolve(process.argv[3]),preview=process.argv.includes('--preview');
const edge=[process.env.CONTOUR_TEST_BROWSER,path.join(process.env['ProgramFiles(x86)']||'','Microsoft/Edge/Application/msedge.exe')].find(p=>p&&fs.existsSync(p));
if(!edge)throw Error('Microsoft Edge is required');
fs.mkdirSync(work,{recursive:true});const profile=fs.mkdtempSync(path.join(work,'browser-'));
const fixture=()=>{
  const data={theme:'dark',eqEnabled:true,device:'focusrite',busy:false,gated:false,attention:false,note:'',
    devices:[{id:'focusrite',name:'Speakers (Focusrite USB Audio)',available:true,channels:2},{id:'other',name:'Other installed output',available:true,channels:2},{id:'gone',name:'Disconnected output',available:false,channels:2}],
    profiles:[{id:'one',name:'My EQ'},{id:'two',name:'Alternative <EQ>'}],
    profile:{id:'one',name:'My EQ',revision:7,dirty:false,layers:[{name:'Left correction',target:'Left',on:true},{name:'Right correction',target:'Right',on:true},{name:'Personal EQ',target:'All',on:true}]}};
  let receive;window.requests=[];window.fixtureState=data;window.emit=()=>receive({data:structuredClone(data)});
  window.chrome.webview={addEventListener:(_,fn)=>receive=fn,postMessage:body=>{
    const r=JSON.parse(body);window.requests.push(r);
    if(r.action==='ready')setTimeout(window.emit,0);
    if(['layer','eq','save','revert','profile','device'].includes(r.action)&&!window.hold){
      if(r.action==='layer'){data.profile.layers[r.layer].on=!data.profile.layers[r.layer].on;data.profile.dirty=true;data.profile.revision++;}
      if(r.action==='eq')data.eqEnabled=!data.eqEnabled;
      if(['save','revert'].includes(r.action))data.profile.dirty=false;
      setTimeout(window.emit,0);
    }
  }};
};
const checks=async()=>{
  const assert=(b,m)=>{if(!b)throw Error(m);},wait=()=>new Promise(r=>setTimeout(r,30)),q=s=>document.querySelector(s);
  try{
    await wait();assert(q('.tray-h')&&q('[data-action="eq"]'),'Flyout did not render');
    assert(q('[data-id="gone"]').disabled,'Disconnected output is selectable');
    for(const node of document.querySelectorAll('.sw'))assert(node.getBoundingClientRect().right<=368,'A toggle is clipped outside the flyout');
    assert(q('[data-id="two"]').textContent.includes('<EQ>'),'Profile names were not escaped');
    q('[data-layer="1"]').focus();q('[data-layer="1"]').click();await wait();
    const action=window.requests.find(r=>r.action==='layer');assert(action.layer===1&&action.device==='focusrite'&&action.profile==='one'&&action.revision===7,'Action lost its reviewed draft identity');
    assert(q('[data-layer="1"]').getAttribute('aria-pressed')==='false'&&q('.unsaved'),'Layer change did not show dirty state');
    assert(document.activeElement===q('[data-layer="1"]'),'Toggle lost keyboard focus after acknowledgement');
    q('[data-action="save"]').click();await wait();assert(!q('.unsaved'),'Save did not clear dirty marker');
    q('[data-action="eq"]').click();await wait();assert(q('[data-action="eq"]').getAttribute('aria-pressed')==='false'&&!q('.unsaved'),'Global EQ altered profile state');
    const button=q('[data-layer="0"]');button.focus();window.emit();assert(q('[data-layer="0"]')===button&&document.activeElement===button,'Unchanged update replaced or unfocused controls');
    window.hold=true;button.click();assert(q('[data-layer="0"]').disabled,'Repeated click allowed while action is pending');window.hold=false;window.emit();
    window.fixtureState.theme='light';window.emit();assert(document.documentElement.dataset.theme==='light','Flyout ignored theme');
    window.fixtureState.profile.layers=Array.from({length:32},(_,i)=>({name:'Layer '+i,target:'All',on:true}));window.emit();
    assert(q('.tray-body').scrollHeight>q('.tray-body').clientHeight,'Long layer list is not scrollable');
    assert(q('[data-action="open-editor"]').getBoundingClientRect().bottom<=innerHeight,'Open Contour fell outside the window');
    document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));assert(window.requests.at(-1).action==='dismiss','Escape did not dismiss');
    window.fixtureState.profile=null;window.fixtureState.profiles=[];window.fixtureState.devices=[];window.fixtureState.gated=true;window.emit();
    assert(q('.tray-empty')&&!q('[data-action="layer"]')&&q('[data-action="open-editor"]'),'Empty installation state is broken');
    document.body.insertAdjacentHTML('beforeend','<pre id="result">PASS</pre>');
  }catch(e){document.body.insertAdjacentHTML('beforeend','<pre id="result"></pre>');q('#result').textContent=e.stack;}
};
const server=http.createServer((req,res)=>{
  const pathname=new URL(req.url,'http://localhost').pathname;
  if(pathname==='/'){
    res.setHeader('Content-Type','text/html');res.end(fs.readFileSync(path.join(ui,'tray.html'),'utf8').replace('</head>','<style>body.tray-surface{width:368px;max-width:100%}</style></head>').replace('<script src="editor.js">','<script src="fixture.js"></script><script src="editor.js">').replace('</body>',preview?'</body>':'<script src="checks.js"></script></body>'));return;
  }
  if(pathname==='/fixture.js'||pathname==='/checks.js'){res.setHeader('Content-Type','text/javascript');res.end(`(${(pathname==='/fixture.js'?fixture:checks).toString()})();`);return;}
  const relative=pathname.slice(1);if(!['styles.css','editor.js','tray.js','fonts/Manrope.ttf'].includes(relative)){res.writeHead(404).end();return;}
  res.setHeader('Content-Type',relative.endsWith('.css')?'text/css':relative.endsWith('.js')?'text/javascript':'font/ttf');res.end(fs.readFileSync(path.join(ui,relative)));
});
server.listen(0,'127.0.0.1',()=>{
  const url=`http://127.0.0.1:${server.address().port}/`;
  if(preview){console.log(url);return;}
  const child=spawn(edge,['--headless=new','--disable-gpu','--no-first-run','--disable-background-networking','--disable-extensions','--disable-sync',`--user-data-dir=${profile}`,'--window-size=368,640','--dump-dom','--virtual-time-budget=3000',url],{windowsHide:true});
  let output='';child.stdout.on('data',b=>output+=b);child.stderr.resume();
  const timeout=setTimeout(()=>child.kill(),20000);
  child.on('exit',()=>{clearTimeout(timeout);server.close();const match=output.match(/<pre id="result">([\s\S]*?)<\/pre>/);if(match?.[1]==='PASS')console.log('PASS tray actions, draft identity, global bypass, focus stability, theme, scrolling, Escape and empty state');else{console.error(match?.[1]||'Tray test did not finish');process.exitCode=1;}});
});
