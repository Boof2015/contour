// Compare the display-only fast path with coefficients produced by the actual
// C++ DSP compiler, across every filter type and several real device rates.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path'),assert=require('node:assert/strict');
const {execFileSync}=require('node:child_process');
const context=vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(process.argv[2],'editor.js'),'utf8'),context);
const evaluate=s=>vm.runInContext(s,context);
const fixtures=JSON.parse(execFileSync(process.argv[3],['--curve-fixtures'],{maxBuffer:8*1024*1024,encoding:'utf8'}));
let checked=0;
for(const {rate,filter} of fixtures){
  if(filter.coefficients===null)continue; // Native validation rejects this extreme combination.
  context.filter=filter;context.rate=rate;
  const actual=JSON.parse(JSON.stringify(evaluate('designCurve(filter,rate)')));
  assert.equal(actual.length,filter.coefficients.length);
  actual.forEach((section,i)=>section.forEach((v,j)=>assert.ok(Math.abs(v-filter.coefficients[i][j])<5e-13*Math.max(1,Math.abs(v)),JSON.stringify({rate,filter,i,j,v}))));
  checked++;
}
assert.ok(checked>3000);
evaluate(`
  const APP={eqEnabled:true},profile={work:{layers:[]}},P=()=>profile;
  const f=F('PK',1000,6,2),left=Ly('Left',false,1,-10,true,[f]),right=Ly('Right',false,2,-20,true,[F('PK',1000,-3,2)]);
  const sample={n:1,f:[1000],c1:[Math.cos(2*Math.PI*1000/FS)],s1:[Math.sin(2*Math.PI*1000/FS)],c2:[Math.cos(4*Math.PI*1000/FS)],s2:[Math.sin(4*Math.PI*1000/FS)]};
  f.coefficients=designCurve(f,FS);rememberCurves({layers:[left]},FS);
`);
const near=(actual,expected)=>assert.ok(Math.abs(actual-expected)<1e-8,`${actual} != ${expected}`);
near(evaluate('resp(left.filters,sample)[0]'),6);
near(evaluate('output([left,right],0,sample)[0]'),6);
near(evaluate('output([left,right],1,sample)[0]'),-3);
evaluate('f.gain=9;'); // No host reply: all curves must already reflect the edit.
near(evaluate('one(f,sample)[0]'),9);
near(evaluate('resp(left.filters,sample)[0]'),9);
near(evaluate('output([left,right],0,sample)[0]'),9);
evaluate('left.gain=-80;');near(evaluate('output([left,right],0,sample)[0]'),9);
evaluate('left.on=false;');near(evaluate('output([left,right],0,sample)[0]'),0);
evaluate('APP.eqEnabled=false;');near(evaluate('output([left,right],1,sample)[0]'),-3);
// Library thumbnails can show saved profiles at different rates without
// changing the open editor's frequency grid or using its cached coefficients.
evaluate(`
  left.on=true;
  const editorRate=FS,editorSpan=LSPAN;
  const thumb=profileCurvePreview({layers:[left,right]},2,96000);
  const same=profileCurvePreview({layers:[Ly('All',true,0,-80,true,[f])]},8,96000);
  const quiet=profileCurvePreview({layers:[Ly('Off',true,0,0,false,[f])]},2,44100);
`);
assert.equal(evaluate('FS===editorRate&&LSPAN===editorSpan'),true);
assert.equal(evaluate('(thumb.match(/class="profile-curve"/g)||[]).length'),2);
assert.equal(evaluate('(same.match(/class="profile-curve"/g)||[]).length'),1);
assert.equal(evaluate('(quiet.match(/class="profile-curve"/g)||[]).length'),1);
assert.equal(evaluate('quiet.includes("NaN")||thumb.includes("NaN")'),false);
evaluate(`const otherGrid=grid(192,96000),otherResponse=resp(right.filters,otherGrid);`);
const thumbResponse=Array.from(evaluate('otherResponse'));
evaluate('FS=96000;LSPAN=Math.log10(20000)-LMIN;');
const referenceResponse=Array.from(evaluate('resp(right.filters,grid(192))'));
thumbResponse.forEach((value,i)=>near(value,referenceResponse[i]));
evaluate('FS=editorRate;LSPAN=editorSpan;');
assert.equal(evaluate('profileCurvePreview({layers:[left,right]},2,96000)===thumb'),true);
console.log(`PASS ${checked} native/JS coefficient cases; synchronous edited curves; gain-independent channel sums, layer bypass and visible curves while global EQ is off`);
console.log('PASS multichannel profile thumbnails, identical-response merging, disabled layers, and independent sample rates');
