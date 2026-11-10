// Wait for the page's result using real elapsed time. --dump-dom with a virtual
// time budget can snapshot an unfinished test while native async work is pending.
const fs=require('node:fs'),path=require('node:path'),{spawn}=require('node:child_process');
const {setTimeout:delay}=require('node:timers/promises');

async function runBrowser({edge,profile,url,resultId,timeoutMs=45000,args=[]}) {
  const controller=new AbortController(),{signal}=controller;
  const timer=setTimeout(()=>controller.abort(Error(`Browser test timed out after ${timeoutMs} ms waiting for #${resultId}`)),timeoutMs);
  const child=spawn(edge,['--headless=new','--disable-gpu','--no-first-run','--disable-background-networking',
    '--disable-extensions','--disable-sync','--remote-debugging-port=0',`--user-data-dir=${profile}`,...args,url],{windowsHide:true});
  let stderr='',socket,closing=false,sequence=0,lastPage;
  const pending=new Map();
  const abort=error=>{if(!closing)controller.abort(error);};
  const closed=new Promise(resolve=>child.once('close',(code,exitSignal)=>{
    abort(Error(`Browser exited before reporting a result (code ${code}, signal ${exitSignal})`));resolve();
  }));
  child.on('error',abort);
  child.stdout.resume();
  child.stderr.on('data',b=>stderr+=b);
  signal.addEventListener('abort',()=>{
    for(const p of pending.values())p.reject(signal.reason);
    pending.clear();
  },{once:true});
  const pause=()=>delay(50,undefined,{signal});
  const send=(method,params={})=>new Promise((resolve,reject)=>{
    signal.throwIfAborted();
    const id=++sequence;pending.set(id,{resolve,reject});
    socket.send(JSON.stringify({id,method,params}));
  });
  try {
    // The debugger is bound to this newly launched, isolated browser instance.
    let debuggerUrl;
    while(!(debuggerUrl=stderr.match(/DevTools listening on (ws:\/\/127\.0\.0\.1:\d+\/\S+)/)?.[1]))await pause();
    const port=new URL(debuggerUrl).port;
    let page;
    while(!page){
      const response=await fetch(`http://127.0.0.1:${port}/json/list`,{signal});
      if(!response.ok)throw Error(`Browser target discovery failed: ${response.status}`);
      page=(await response.json()).find(p=>p.type==='page'&&p.url===url);
      if(!page)await pause();
    }
    socket=new WebSocket(page.webSocketDebuggerUrl);
    socket.addEventListener('error',()=>abort(Error('Browser debugger connection failed')));
    socket.addEventListener('close',()=>abort(Error('Browser debugger closed before reporting a result')));
    socket.addEventListener('message',event=>{
      const message=JSON.parse(event.data),request=pending.get(message.id);
      if(!request)return;
      pending.delete(message.id);
      if(message.error)request.reject(Error(message.error.message));else request.resolve(message.result);
    });
    await new Promise((resolve,reject)=>{
      const cancelled=()=>reject(signal.reason);
      signal.addEventListener('abort',cancelled,{once:true});
      socket.addEventListener('open',()=>{signal.removeEventListener('abort',cancelled);resolve();},{once:true});
      if(signal.aborted)cancelled();
    });
    const expression=`({text:document.getElementById(${JSON.stringify(resultId)})?.textContent??null,progress:window.testProgress,html:document.documentElement?.outerHTML||''})`;
    for(;;){
      const reply=await send('Runtime.evaluate',{expression,returnByValue:true});
      if(reply.exceptionDetails)throw Error(reply.exceptionDetails.exception?.description||reply.exceptionDetails.text);
      lastPage=reply.result.value;
      if(lastPage.text!==null)return lastPage.text;
      await pause();
    }
  }catch(error){
    const failure=signal.aborted?signal.reason:error;
    const dump=path.join(profile,'test-page.html');
    if(lastPage)fs.writeFileSync(dump,lastPage.html);
    throw Error(`${failure.message}\nLast completed check: ${lastPage?.progress?.at(-1)||'(none)'}\n`+
      (lastPage?`Page saved to ${dump}\n`:'')+stderr.slice(-1800),{cause:failure});
  }finally{
    clearTimeout(timer);closing=true;
    // Ask Chromium to close all of this instance's processes, including on test
    // failure. A bounded fallback handles startup failures and crashed renderers.
    if(socket?.readyState===WebSocket.OPEN)socket.send(JSON.stringify({id:++sequence,method:'Browser.close'}));
    const killTimer=setTimeout(()=>{child.kill();child.stdout.destroy();child.stderr.destroy();socket?.close();},2000);
    await closed;clearTimeout(killTimer);
    socket?.close();
    fs.writeFileSync(path.join(profile,'browser-stderr.log'),stderr);
  }
}
module.exports={runBrowser};
