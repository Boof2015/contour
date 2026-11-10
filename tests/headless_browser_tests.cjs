// Exercise completion, failure reporting and timeout cleanup in real Chromium.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http');
const {runBrowser}=require('./headless_browser.cjs');
const work=path.resolve(process.argv[2]);fs.mkdirSync(work,{recursive:true});
const edge=[process.env.CONTOUR_TEST_BROWSER,
  path.join(process.env['ProgramFiles(x86)']||'','Microsoft/Edge/Application/msedge.exe'),
  path.join(process.env.ProgramFiles||'','Microsoft/Edge/Application/msedge.exe')].find(p=>p&&fs.existsSync(p));
if(!edge)throw Error('Microsoft Edge is required for browser runner checks');
const expected={passed:true,message:'Preserve <tags> & "quotes", Unicode: − →, and\nnewlines'};
const server=http.createServer((req,res)=>{
  const route=new URL(req.url,'http://localhost').pathname;
  const result=route==='/failure'?{passed:false,error:'Intentional assertion failure'}:expected;
  res.setHeader('Content-Type','text/html; charset=utf-8');
  res.end(`<!doctype html><title>Isolated runner fixture</title><script>
    window.testProgress=['Fixture loaded'];
    ${route==='/hang'?'':`setTimeout(()=>{
      const node=document.createElement('pre');node.id='result';
      node.textContent=JSON.stringify(${JSON.stringify(result)});document.body.append(node);
    },${route==='/delayed'?13000:30});`}
  </script><body>Waiting for the fixture</body>`);
});
server.listen(0,'127.0.0.1',async()=>{
  const url=`http://127.0.0.1:${server.address().port}`;
  const profile=()=>fs.mkdtempSync(path.join(work,'browser-'));
  try{
    // Longer than the former 12-second virtual-time snapshot: completion, not
    // time passing, determines when the page is ready to be collected.
    assert.deepEqual(JSON.parse(await runBrowser({edge,profile:profile(),url:url+'/delayed',resultId:'result'})),expected);
    console.log('PASS delayed completion is collected with exact text, including HTML-sensitive characters');
    assert.deepEqual(JSON.parse(await runBrowser({edge,profile:profile(),url:url+'/failure',resultId:'result'})),
      {passed:false,error:'Intentional assertion failure'});
    console.log('PASS assertion failure is returned without being turned into a successful result');
    const hung=profile();
    await assert.rejects(runBrowser({edge,profile:hung,url:url+'/hang',resultId:'result',timeoutMs:10000}),error=>{
      assert.match(error.message,/timed out.*waiting for #result/);
      assert.match(error.message,/Last completed check: Fixture loaded/);
      assert.match(fs.readFileSync(path.join(hung,'test-page.html'),'utf8'),/Waiting for the fixture/);
      assert.ok(fs.existsSync(path.join(hung,'browser-stderr.log')));
      return true;
    });
    console.log('PASS unfinished tests fail within a deadline with page, browser log and progress diagnostics');
    await assert.rejects(runBrowser({edge:path.join(work,'missing-browser.exe'),profile:profile(),url:url+'/failure',resultId:'result'}),/ENOENT/);
    console.log('PASS browser startup failures reject and clean up');
  }catch(error){console.error(error);process.exitCode=1;}
  finally{server.close();}
});
