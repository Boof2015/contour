// Device management does not change the output/profile open in the editor.
const setupReports=new Map();
const setupCache=new Map();
let scanQueue=[],scanStarting=false,setupOperation=null,pendingSetup=null,inventoryRefreshing=false;
const setupDevice=()=>APP.devices.find(d=>d.id===S.settingsDevice);
const setupDeviceStamp=d=>JSON.stringify([d.available,d.state,d.channels,d.rate,d.mask]);
const inspectingDevice=id=>(SETUP.busy||scanStarting)&&SETUP.endpoint===id;
function setupCheckFresh(d,now=Date.now()){
  const cached=setupCache.get(d.id),report=setupReports.get(d.id);
  return !!cached&&!!report&&!report.busy&&cached.stamp===setupDeviceStamp(d)&&now-cached.time<60000;
}
function prioritizeCheck(endpoint){
  if(inspectingDevice(endpoint))return;
  scanQueue=[endpoint,...scanQueue.filter(id=>id!==endpoint)];scanNext();
}
const installation=s=>s?.installation||(s?.state==='attached'?'installed':s?.state==='recovery'?'incomplete':s?.state==='available'?'not_installed':'unknown');
function installationLabel(s){
  if(s?.busy)return s.state==='installing'?'Installing…':s.state==='removing'?'Uninstalling…':'Checking…';
  return {installed:'Installed',not_installed:'Not installed',incomplete:'Needs attention',unknown:'Not checked'}[installation(s)];
}
function compatibilityLabel(d,s){
  if(!d.available)return d.state===2?'Disabled':'Disconnected';
  if(d.channels>32||(d.rate&& (d.rate<8000||d.rate>384000)))return 'Unsupported format';
  if(installation(s)==='installed'){
    if(d.connection?.checking)return 'Checking';
    if(d.connection?.detail)return 'Check connection';
    if(d.connection?.confirmed||d.running||(d.id===APP.device&&APP.status.endpoint==='ready'))return 'Confirmed';
  }
  return s?.canInstall?'Ready to test':'Not checked';
}
function settingsView(){
  return `<div class="body settings-body"><aside class="side settings-nav"><button class="settings-back" data-act="back-eq">${I('back')}Back to EQ</button><nav aria-label="Settings"><button data-settings-tab="outputs" class="${S.settingsTab==='outputs'?'sel':''}">${I('speaker')}Outputs</button><button data-settings-tab="general" class="${S.settingsTab==='general'?'sel':''}">${I('settings')}General</button></nav></aside><main class="settings-main" id="settingsMain">${S.settingsTab==='general'?generalSettings():outputSettings()}</main></div>`;
}
function generalSettings(){
  return `<div class="settings-head"><h1>General</h1></div><section class="preference-row"><div><h2>Appearance</h2><p>Choose how Contour looks.</p></div><div class="theme-options" role="group" aria-label="Appearance">${['dark','light'].map(theme=>`<button data-theme-choice="${theme}" aria-pressed="${S.theme===theme}">${theme==='dark'?'Dark':'Light'}</button>`).join('')}</div></section>`;
}
function outputSettings(){
  const connected=APP.devices.filter(d=>d.available),disconnected=APP.devices.filter(d=>!d.available);
  const rows=devices=>devices.map(d=>`<section class="device-entry"><div class="device-row" data-device-row="${esc(d.id)}" tabindex="0" role="button" aria-expanded="${S.settingsDevice===d.id}" aria-label="${esc(d.name)}"><span class="device-title">${I('speaker')}<span><strong>${esc(d.name)}</strong><small>${d.channels?`${d.channels} channels · ${d.rate/1000} kHz`:'Device unavailable'}${d.id===APP.device?' · Open in editor':''}</small></span></span><span data-install-state="${esc(d.id)}"></span><span class="device-compat" data-compat="${esc(d.id)}"></span><button class="device-manage" data-manage-device="${esc(d.id)}">Manage</button></div><div class="device-detail" data-device-detail="${esc(d.id)}" ${S.settingsDevice===d.id?'':'hidden'}></div></section>`).join('');
  return `<div class="settings-head"><div><h1>Outputs</h1><p>Choose where Contour processes audio.</p></div><button class="settings-button" data-act="refresh-devices">${I('refresh')}Refresh</button></div>${APP.masterIssues?.length?'<div class="settings-message"><strong>EQ change pending</strong><p>Some devices could not be updated. Check their connection and try again.</p><button class="settings-button" data-act="retry-master">Retry EQ change</button></div>':''}<div class="device-columns" aria-hidden="true"><span>Device</span><span>Contour</span><span>Compatibility</span><span></span></div><div class="device-list">${rows(connected)}</div>${disconnected.length?`<details class="disconnected-devices" ${S.showDisconnected?'open':''}><summary>Disconnected or disabled <span>${disconnected.length}</span></summary><div class="device-list">${rows(disconnected)}</div></details>`:''}`;
}
function deviceMessage(d,s){
  if(pendingSetup?.endpoint===d.id)return 'Finishing the current device check before '+(pendingSetup.action==='Install'?'installation.':'removal.');
  if(s?.state==='installing')return 'Installing Contour on this device. Audio will return when installation finishes.';
  if(s?.state==='removing')return 'Uninstalling Contour from this device. Your profiles will be kept.';
  if(s?.busy)return 'Checking this device…';
  if(installation(s)==='incomplete')return 'The installation needs attention. Uninstall it before trying again.';
  if(!d.available)return d.state===2?'Enable this device in Windows to use it with Contour.':'Reconnect this device to use it with Contour.';
  if(installation(s)==='installed'){
    if(d.connection?.checking)return 'Checking the connection silently. You can keep editing.';
    if(d.connection?.detail)return 'Contour could not connect to this device. Check again or review the troubleshooting details.';
    return d.connection?.confirmed||d.running||(d.id===APP.device&&APP.status.endpoint==='ready')?'Contour is ready on this device.':'Contour is installed. The connection is checked automatically; no playback is needed.';
  }
  if(s?.reason==='another_device')return `This preview supports one installed device at a time. Uninstall Contour from ${s.otherDevice||'the other device'} before installing it here.`;
  if(s?.reason==='preview_requirements')return 'Installation is not available on this computer with this preview.';
  if(s?.canInstall)return 'Contour can be installed on this device.';
  if(s?.state==='blocked')return 'Contour could not confirm that installation is available. Check again or review the troubleshooting details.';
  return 'Check this device to see whether Contour can be installed.';
}
function deviceDetails(d,s){
  const confirm=S.setupConfirm?.endpoint===d.id?S.setupConfirm.action:null;
  const busy=!!setupOperation||!!s?.busy,installed=installation(s)==='installed',canInstall=s?.canInstall&&d.available;
  const actions=confirm?`<div class="install-review"><h2>Uninstall Contour from this device?</h2><p>Your profiles will be kept. Windows will ask for administrator approval. Audio will briefly stop during removal.</p><div class="settings-actions"><button class="settings-button" data-setup-act="cancel">Cancel</button><button class="settings-button primary" data-setup-act="confirm" ${busy?'disabled':''}>Uninstall</button></div></div>`:`${canInstall?'<p class="settings-notice">Windows will ask for administrator approval. Audio will briefly stop during installation.</p>':''}<div class="settings-actions">${canInstall?`<button class="settings-button primary" data-setup-act="install" ${busy?'disabled':''}>Install</button>`:''}${installed&&d.available?'<button class="settings-button primary" data-setup-act="open-eq">Open EQ</button>':''}${s?.canRemove?'<button class="settings-button" data-setup-act="uninstall">Uninstall</button>':''}<button class="settings-button" data-setup-act="check" ${busy?'disabled':''}>Check again</button></div>`;
  const errors=(APP.masterIssues||[]).filter(x=>x.device===d.id).map(x=>x.detail);
  const runtime=d.id===APP.device?APP.status:null;
  const diagnostics=[s?.detail,s?.notice,d.connection?.detail,...errors,runtime?.detail,runtime?.previewDetail,runtime?.copyDetail].filter(Boolean);
  const controls=pendingSetup?.endpoint===d.id?'<div class="settings-actions"><button class="settings-button" data-setup-act="cancel-pending">Cancel</button></div>':setupOperation?.endpoint===d.id?'':!s?.busy||s.state==='checking'?actions:'';
  return `<p class="device-explanation" role="status">${esc(deviceMessage(d,s))}</p>${s?.notice?`<p class="settings-notice">${esc(s.notice)}</p>`:''}${controls}<details class="troubleshooting"><summary>Troubleshooting details</summary><p>${esc(d.name)}</p><p class="technical-id">${esc(d.id)}</p>${runtime?`<p>Saved EQ: ${esc(runtime.saved||'Not checked')}<br>Running EQ: ${esc(runtime.running||'Not checked')}</p>`:''}${diagnostics.map(x=>`<p>${esc(x)}</p>`).join('')}</details>`;
}
function updateSettingsStatus(){
  const toggle=$('[data-act="eq"]');if(toggle)toggle.disabled=!!setupOperation;
  if(S.view!=='settings'||S.settingsTab!=='outputs')return;
  const refresh=$('[data-act="refresh-devices"]');if(refresh)refresh.disabled=!!setupOperation||inventoryRefreshing;
  APP.devices.forEach(d=>{
    const s=setupReports.get(d.id);
    const row=$$('[data-device-row]').find(el=>el.dataset.deviceRow===d.id);if(!row)return;
    row.setAttribute('aria-expanded',String(S.settingsDevice===d.id));
    row.querySelector('[data-install-state]').textContent=installationLabel(s);
    row.querySelector('[data-compat]').textContent=compatibilityLabel(d,s);
    const detail=row.nextElementSibling;detail.hidden=S.settingsDevice!==d.id;
    if(!detail.hidden){
      const html=deviceDetails(d,s);
      if(detail.dataset.markup!==html){
        const expanded=detail.querySelector('.troubleshooting')?.open,focused=detail.contains(document.activeElement)?document.activeElement?.dataset.setupAct:null;
        detail.innerHTML=html;detail.dataset.markup=html;
        if(expanded)detail.querySelector('.troubleshooting').open=true;
        if(focused)detail.querySelector(`[data-setup-act="${focused}"]`)?.focus({preventScroll:true});
      }
    }
  });
}
async function openSettings(tab='outputs'){
  document.activeElement?.blur();endGesture();await flush();
  S.pop=null;S.view='settings';S.settingsTab=tab;S.settingsDevice=null;S.setupConfirm=null;render();
  if(tab==='outputs')refreshDevices();
}
async function refreshDevices(force=false){
  if(setupOperation||inventoryRefreshing)return;
  inventoryRefreshing=true;updateSettingsStatus();
  try{
    // Inventory is read-only. A delayed response must not replace a draft or
    // steal focus from the editor if the user has already returned to it.
    const data=await rpc('devices');APP.devices=data.devices;
    const eligible=APP.devices.filter(d=>(d.available||S.showDisconnected||d.id===S.settingsDevice)&&d.id.startsWith('{0.0.0.00000000}.'));
    if(force)eligible.forEach(d=>setupCache.delete(d.id));
    const rank=id=>id===S.settingsDevice?0:id===APP.device?1:2;
    scanQueue=eligible.filter(d=>!inspectingDevice(d.id)&&!setupCheckFresh(d)).map(d=>d.id).sort((a,b)=>rank(a)-rank(b));
    if(S.view==='settings'&&S.settingsTab==='outputs')render();
  }catch(error){toast(error.message);}
  finally{inventoryRefreshing=false;updateSettingsStatus();scanNext();}
}
function scanNext(){
  if(scanStarting||SETUP.busy)return;
  // An explicit action takes the next slot; it never waits for the remaining
  // device list, and is never run concurrently with another setup helper.
  if(pendingSetup){const next=pendingSetup;pendingSetup=null;startSetup(next.action,next.endpoint);return;}
  if(setupOperation||S.setupConfirm||S.view!=='settings'||S.settingsTab!=='outputs')return;
  const endpoint=scanQueue.shift();if(endpoint)startSetup('Inspect',endpoint);
}
function acceptSetup(data){
  // Verification is still part of the operation the user started. Do not
  // briefly restore the device actions while the helper checks its result.
  if(!pendingSetup&&data.busy&&data.state==='checking'&&setupOperation?.endpoint===data.endpoint){
    data={...data,state:setupOperation.action==='Install'?'installing':'removing'};
  }
  SETUP=data;if(data.endpoint)setupReports.set(data.endpoint,data);
  if(!data.busy){
    const device=APP.devices.find(d=>d.id===data.endpoint);
    if(device&&!data.notice&&(data.state!=='blocked'||['another_device','preview_requirements'].includes(data.reason)))setupCache.set(device.id,{time:Date.now(),stamp:setupDeviceStamp(device)});
    else setupCache.delete(data.endpoint);
  }
  if(!pendingSetup&&!data.busy&&setupOperation?.endpoint===data.endpoint){
    const installed=setupOperation.action==='Install'&&installation(data)==='installed'&&!data.notice;
    setupOperation=null;S.setupConfirm=null;
    // Installation changes availability for every row in the current preview.
    scanQueue=[];
    for(const id of setupReports.keys())if(id!==data.endpoint){setupReports.delete(id);setupCache.delete(id);}
    setTimeout(async()=>{
      // Apply the app-wide preference and saved assignment to the new install.
      // This does not Save a draft or change the library's profiles.
      if(installed)await command('eq',{enabled:APP.eqEnabled!==false},true);
      if(S.view==='settings')refreshDevices();
    },0);
  }
  updateSettingsStatus();statusbar();
  if(!data.busy)setTimeout(scanNext,0);
}
function setupRequest(action,endpoint=S.settingsDevice){
  if(!endpoint||setupOperation)return;
  if(action==='Inspect'){prioritizeCheck(endpoint);return;}
  if(SETUP.busy||scanStarting){
    pendingSetup={endpoint,action};setupOperation={endpoint,action};S.setupConfirm=null;updateSettingsStatus();return;
  }
  startSetup(action,endpoint);
}
async function startSetup(action,endpoint){
  scanStarting=true;
  if(action!=='Inspect'){setupOperation={endpoint,action};S.setupConfirm=null;}
  try{
    // Background inspection must leave text editing, pointer gestures and
    // unsaved input alone. Only an explicit install/remove flushes edits.
    if(action!=='Inspect'){document.activeElement?.blur();endGesture();await flush();}
    SETUP={...setupReports.get(endpoint),endpoint,busy:true,state:action==='Inspect'?'checking':action==='Install'?'installing':'removing'};
    setupReports.set(endpoint,SETUP);updateSettingsStatus();
    const result=await rpc('setup',{action,endpoint});scanStarting=false;acceptSetup(result);
  }catch(error){scanStarting=false;if(action!=='Inspect')setupOperation=null;acceptSetup({...SETUP,busy:false,state:'blocked',notice:error.message,canInstall:false,canRemove:false});}
}
function openSetup(){return openSettings('outputs');}
function setupAct(action){
  const d=setupDevice(),s=d&&setupReports.get(d.id);if(!d)return;
  if(action==='cancel-pending'&&pendingSetup?.endpoint===d.id){pendingSetup=null;setupOperation=null;updateSettingsStatus();scanNext();}
  else if(action==='cancel'){S.setupConfirm=null;updateSettingsStatus();scanNext();}
  else if(action==='install'&&!s?.busy&&s?.canInstall&&d.available){setupRequest('Install',d.id);}
  else if(action==='uninstall'&&!setupOperation&&!s?.busy&&s?.canRemove){S.setupConfirm={endpoint:d.id,action:'Remove'};updateSettingsStatus();}
  else if(action==='confirm'&&S.setupConfirm){setupRequest(S.setupConfirm.action,d.id);}
  else if(action==='check'){setupRequest('Inspect',d.id);}
  else if(action==='open-eq'){S.openEditorDevice=d.id;command('device',{id:d.id},true);}
}
document.addEventListener('click',e=>{
  const tab=e.target.closest('[data-settings-tab]');if(tab){S.settingsTab=tab.dataset.settingsTab;render();if(S.settingsTab==='outputs')scanNext();return;}
  const theme=e.target.closest('[data-theme-choice]');if(theme){S.theme=theme.dataset.themeChoice;localStorage.setItem('theme',S.theme);render();syncWindowTheme();return;}
  const action=e.target.closest('[data-setup-act]');if(action){setupAct(action.dataset.setupAct);return;}
  const row=e.target.closest('[data-device-row]');if(row){
    S.settingsDevice=S.settingsDevice===row.dataset.deviceRow?null:row.dataset.deviceRow;S.setupConfirm=null;
    const device=setupDevice();if(device&&!setupCheckFresh(device))prioritizeCheck(device.id);
    updateSettingsStatus();scanNext();
  }
});
document.addEventListener('keydown',e=>{if(e.target.matches?.('[data-device-row]')&&(e.key==='Enter'||e.key===' ')){e.preventDefault();e.target.click();}});
document.addEventListener('toggle',e=>{
  if(!e.target.matches?.('.disconnected-devices'))return;
  S.showDisconnected=e.target.open;
  if(S.showDisconnected){
    for(const d of APP.devices)if(!d.available&&!setupCheckFresh(d)&&!inspectingDevice(d.id)&&!scanQueue.includes(d.id)&&d.id.startsWith('{0.0.0.00000000}.'))scanQueue.push(d.id);
    scanNext();
  }else scanQueue=scanQueue.filter(id=>APP.devices.find(d=>d.id===id)?.available||id===S.settingsDevice);
},true);
