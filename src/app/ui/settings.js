// Device management does not change the output/profile open in the editor.
const setupReports=new Map();
const setupCache=new Map();
let scanQueue=[],scanStarting=false,setupOperation=null,pendingSetup=null,inventoryRefreshing=false,engineSetup=null;
let STARTUP=null,startupBusy=false;
function startupPreference(){
  return `<div><h2>Start with Windows</h2><p>${esc(STARTUP?.note||'Checking Windows startup settings…')}</p></div><button class="sw${STARTUP?.enabled?' on':''}" data-start-with-windows aria-label="Start with Windows" aria-pressed="${!!STARTUP?.enabled}" ${startupBusy||!STARTUP?.available?'disabled':''}></button>`;
}
async function startupSettings(enabled){
  if(startupBusy)return;
  startupBusy=true;const update=()=>{const row=$('#startupPreference');if(row)row.innerHTML=startupPreference();};update();
  try{STARTUP=await rpc('startup',enabled===undefined?{action:'state'}:{action:'set',enabled});}
  catch(error){toast(error.message);if(!STARTUP)STARTUP={available:false,note:'Couldn’t read Windows startup settings. Reopen General to try again.'};}
  finally{startupBusy=false;update();}
}
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
const setupProgress=action=>({Inspect:'checking',Install:'installing',Remove:'removing',Update:'updating',RecoverUpdate:'recovering'}[action]);
// Installation is independent of profiles and of whether audio is playing.
// A retained settings context/old runtime report is not proof of attachment.
function editorGate(){
  if(!APP)return 'checking';
  if(['Update','RecoverUpdate'].includes(setupOperation?.action))return setupProgress(setupOperation.action);
  const d=D(),s=setupReports.get(d.id),operation=setupOperation?.endpoint===d.id?setupOperation.action:null;
  if(operation)return operation==='Install'?'installing':'removing';
  if(installation(s)==='installed')return '';
  if(installation(s)==='incomplete')return 'attention';
  if(installation(s)==='not_installed')return 'not_installed';
  if(!d.available)return 'disconnected';
  if(s&&!s.busy)return 'unknown';
  if(APP.status.endpoint==='ready'||d.connection?.confirmed)return '';
  if(APP.status.endpoint==='not_attached')return 'not_installed';
  return 'checking';
}
const editorChecks=new Map();
function ensureEditorCheck(){
  if(!APP||S.view!=='editor'||setupOperation)return;
  const d=D(),key=setupDeviceStamp(d),attempt=editorChecks.get(d.id);
  if(!d.available||setupCheckFresh(d)||inspectingDevice(d.id))return;
  if(setupReports.has(d.id)&&attempt?.stamp===key&&Date.now()-attempt.time<60000)return;
  editorChecks.set(d.id,{stamp:key,time:Date.now()});prioritizeCheck(d.id);
}
function installationView(gate){
  const messages={
    not_installed:['Contour isn’t installed on this device','Install Contour to apply EQ to '+D().name+'.','Install Contour'],
    checking:['Checking this device…','Checking whether Contour is installed.',''],
    installing:['Installing Contour…','Audio will return when installation finishes.',''],
    removing:['Uninstalling Contour…','Your profiles will be kept.',''],
    updating:['Updating the audio engine…','Your EQ and installed devices will be kept.',''],
    recovering:['Recovering the audio engine…','Restoring the previous engine for your installed devices.',''],
    attention:['Installation needs attention','Review this device’s installation in Settings.','Manage device'],
    unknown:['Couldn’t check this device','Review its installation status in Settings before editing EQ.','Check device'],
    disconnected:['This device is disconnected','Reconnect '+D().name+' to check its installation.','Manage device']
  };
  const [title,detail,action]=messages[gate],s=setupReports.get(APP.device);
  const unsupported=gate==='not_installed'&&s&&!s.busy&&!s.canInstall;
  return `<main class="welcome installation-view" aria-labelledby="installationTitle"><h2 id="installationTitle">${esc(title)}</h2><p role="status">${esc(unsupported?deviceMessage(D(),s):detail)}</p>${action?`<button class="prim" data-act="device-setup">${unsupported?'Manage device':action}</button>`:''}</main>`;
}
function installationLabel(s){
  if(s?.busy)return {installing:'Installing…',removing:'Uninstalling…',updating:'Updating…',recovering:'Recovering…'}[s.state]||'Checking…';
  return {installed:'Installed',not_installed:'Not installed',incomplete:'Needs attention',unknown:'Not checked'}[installation(s)];
}
function compatibilityLabel(d,s){
  if(!d.available)return d.state===2?'Disabled':'Disconnected';
  if(s?.reason==='existing_effects')return 'Not supported yet';
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
  return `<div class="settings-head"><h1>General</h1></div>
    <section class="preference-row"><div><h2>Appearance</h2><p>Choose how Contour looks.</p></div><div class="theme-options" role="group" aria-label="Appearance">${['dark','light'].map(theme=>`<button data-theme-choice="${theme}" aria-pressed="${S.theme===theme}">${theme==='dark'?'Dark':'Light'}</button>`).join('')}</div></section>
    <section class="preference-row" id="startupPreference">${startupPreference()}</section>
    <section class="preference-row"><div><h2>Start in tray</h2><p>Open in the notification area when Contour starts. Launch Contour again to open its window.</p></div><button class="sw${APP.startInTray?' on':''}" data-start-in-tray aria-label="Start in tray" aria-pressed="${!!APP.startInTray}"></button></section>
    <section class="preference-row"><div><h2>Close window to tray</h2><p>Keep Contour and your current edits available in the notification area. Right-click its icon and choose Quit to exit.</p></div><button class="sw${APP.closeToTray===false?'':' on'}" data-close-to-tray aria-label="Close window to tray" aria-pressed="${APP.closeToTray!==false}"></button></section>
    <p class="settings-notice">Saved EQ continues to work when Contour is closed. After a restart or sign-out, unsaved edits are available to recover in Profiles.</p>${APP.recoveryWarning?`<p class="settings-notice warn" role="status">Couldn’t keep a recovery copy: ${esc(APP.recoveryWarning)}</p>`:''}`;
}
function engineUpdateView(){
  const operation=['Update','RecoverUpdate'].includes(setupOperation?.action)?setupOperation:null;
  const s=engineSetup,recovery=operation?operation.action==='RecoverUpdate':s?.canRecoverUpdate;
  if(!operation&&!s?.canRecoverUpdate&&!s?.canUpdate)return '';
  const queued=operation&&!!pendingSetup;
  const title=operation?(queued?'Waiting for the current device check…':recovery?'Recovering the audio engine…':'Updating the audio engine…'):recovery?'Recover the audio engine':'Audio engine update available';
  const detail=operation?(recovery?'Restoring the previous engine':'Updating the audio engine'):(recovery?'Restore the previous engine':'Update Contour’s audio processing');
  const controls=queued?'<button class="settings-button" data-engine-act="cancel">Cancel</button>':operation?'':`<button class="settings-button primary" data-engine-act="${recovery?'recover':'update'}" ${setupOperation?'disabled':''}>${recovery?'Recover engine':'Update engine'}</button>`;
  return `<section class="engine-update" aria-labelledby="engineUpdateTitle"><h2 id="engineUpdateTitle">${title}</h2><p ${operation?'role="status"':''}>${detail} for all installed devices. Your EQ and device installations will be kept.</p>${s?.engineDevices?.length?`<p>${s.engineDevices.map(esc).join(' · ')}</p>`:''}${!operation?'<p>Windows will ask for administrator approval. Audio will briefly stop. Disconnected devices will use the engine when they reconnect.</p>':''}${s?.notice?`<p class="settings-notice">${esc(s.notice)}</p>`:''}${controls}</section>`;
}
function outputSettings(){
  const connected=APP.devices.filter(d=>d.available),disconnected=APP.devices.filter(d=>!d.available);
  const rows=devices=>devices.map(d=>`<section class="device-entry"><div class="device-row" data-device-row="${esc(d.id)}" tabindex="0" role="button" aria-expanded="${S.settingsDevice===d.id}" aria-label="${esc(d.name)}"><span class="device-title">${I('speaker')}<span><strong>${esc(d.name)}</strong><small>${d.channels?`${d.channels} channels · ${d.rate/1000} kHz`:'Device unavailable'}${d.id===APP.device?' · Open in editor':''}</small></span></span><span data-install-state="${esc(d.id)}"></span><span class="device-compat" data-compat="${esc(d.id)}"></span><button class="device-manage" data-manage-device="${esc(d.id)}">Manage</button></div><div class="device-detail" data-device-detail="${esc(d.id)}" ${S.settingsDevice===d.id?'':'hidden'}></div></section>`).join('');
  return `<div class="settings-head"><div><h1>Outputs</h1><p>Choose where Contour processes audio.</p></div><button class="settings-button" data-act="refresh-devices">${I('refresh')}Refresh</button></div><div id="engineUpdate"></div>${APP.masterIssues?.length?'<div class="settings-message"><strong>EQ change pending</strong><p>Some devices could not be updated. Check their connection and try again.</p><button class="settings-button" data-act="retry-master">Retry EQ change</button></div>':''}<div class="device-columns" aria-hidden="true"><span>Device</span><span>Contour</span><span>Compatibility</span><span></span></div><div class="device-list">${rows(connected)}</div>${disconnected.length?`<details class="disconnected-devices" ${S.showDisconnected?'open':''}><summary>Disconnected or disabled <span>${disconnected.length}</span></summary><div class="device-list">${rows(disconnected)}</div></details>`:''}`;
}
function deviceMessage(d,s){
  if(pendingSetup?.endpoint===d.id)return 'Finishing the current device check before '+({Install:'installation.',Remove:'removal.',Update:'the engine update.',RecoverUpdate:'engine recovery.'}[pendingSetup.action]);
  if(s?.state==='updating')return 'Updating the audio engine for all installed devices. Your EQ will be kept.';
  if(s?.state==='recovering')return 'Restoring the previous audio engine. Your EQ will be kept.';
  if(s?.engineState==='recovery')return 'An engine update needs recovery. Restore the previous engine before making further changes.';
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
  if(s?.reason==='existing_effects')return 'Contour does not yet support installation alongside this device’s existing audio effects.';
  if(s?.canInstall)return 'Contour can be installed on this device.';
  if(s?.state==='blocked')return 'Contour could not confirm that installation is available. Check again or review the troubleshooting details.';
  return 'Check this device to see whether Contour can be installed.';
}
function deviceDetails(d,s){
  const confirm=S.setupConfirm?.endpoint===d.id?S.setupConfirm.action:null;
  const busy=!!setupOperation||!!s?.busy,installed=installation(s)==='installed',canInstall=s?.canInstall&&d.available;
  const compatibility=canInstall&&s?.enablesUnsignedAudio?'<p class="settings-notice">Installation will allow unsigned audio effects in Windows. This affects all audio devices and may affect protected media playback. The setting is kept after removal so other audio software can continue using it.</p>':'';
  const actions=confirm?`<div class="install-review"><h2>Uninstall Contour from this device?</h2><p>Your profiles will be kept. Windows will ask for administrator approval. Audio will briefly stop during removal.</p><div class="settings-actions"><button class="settings-button" data-setup-act="cancel">Cancel</button><button class="settings-button primary" data-setup-act="confirm" ${busy?'disabled':''}>Uninstall</button></div></div>`:`${canInstall?'<p class="settings-notice">Windows will ask for administrator approval. Audio will briefly stop during installation.</p>':''}${compatibility}<div class="settings-actions">${canInstall?`<button class="settings-button primary" data-setup-act="install" ${busy?'disabled':''}>Install</button>`:''}${installed&&d.available?'<button class="settings-button primary" data-setup-act="open-eq">Open EQ</button>':''}${s?.canRemove?'<button class="settings-button" data-setup-act="uninstall">Uninstall</button>':''}<button class="settings-button" data-setup-act="check" ${busy?'disabled':''}>Check again</button></div>`;
  const errors=(APP.masterIssues||[]).filter(x=>x.device===d.id).map(x=>x.detail);
  const runtime=d.id===APP.device?APP.status:null;
  const diagnostics=[s?.detail,s?.notice,s?.engineDetail,s?.installedEngine?`Installed engine: ${s.installedEngine}`:'',d.connection?.detail,...errors,runtime?.detail,runtime?.previewDetail,runtime?.copyDetail].filter(Boolean);
  const controls=pendingSetup?.endpoint===d.id?'<div class="settings-actions"><button class="settings-button" data-setup-act="cancel-pending">Cancel</button></div>':setupOperation?.endpoint===d.id?'':!s?.busy||s.state==='checking'?actions:'';
  return `<p class="device-explanation" role="status">${esc(deviceMessage(d,s))}</p>${s?.notice?`<p class="settings-notice">${esc(s.notice)}</p>`:''}${controls}<details class="troubleshooting"><summary>Troubleshooting details</summary><p>${esc(d.name)}</p><p class="technical-id">${esc(d.id)}</p>${runtime?`<p>Saved EQ: ${esc(runtime.saved||'Not checked')}<br>Running EQ: ${esc(runtime.running||'Not checked')}</p>`:''}${diagnostics.map(x=>`<p>${esc(x)}</p>`).join('')}</details>`;
}
function updateSettingsStatus(){
  const toggle=$('[data-act="eq"]');if(toggle)toggle.disabled=!!setupOperation;
  if(S.view!=='settings'||S.settingsTab!=='outputs')return;
  const refresh=$('[data-act="refresh-devices"]');if(refresh)refresh.disabled=!!setupOperation||inventoryRefreshing;
  const engine=$('#engineUpdate'),engineHtml=engineUpdateView();
  if(engine&&engine.dataset.markup!==engineHtml){
    const focused=engine.contains(document.activeElement)?document.activeElement?.dataset.engineAct:null;
    engine.innerHTML=engineHtml;engine.dataset.markup=engineHtml;
    if(focused)engine.querySelector(`[data-engine-act="${focused}"]`)?.focus({preventScroll:true});
  }
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
async function openSettings(tab='outputs',endpoint=null){
  document.activeElement?.blur();endGesture();await flush();
  S.pop=null;S.view='settings';S.settingsTab=tab;S.settingsDevice=endpoint;S.setupConfirm=null;render();
  if(tab==='outputs')refreshDevices();
  else startupSettings();
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
  if(setupOperation||S.setupConfirm)return;
  if(S.tray){const endpoint=scanQueue.shift();if(endpoint)startSetup('Inspect',endpoint);return;}
  if(S.view==='editor'){
    const index=scanQueue.indexOf(APP.device);if(index<0)return;
    scanQueue.splice(index,1);startSetup('Inspect',APP.device);return;
  }
  if(S.view!=='settings'||S.settingsTab!=='outputs')return;
  const endpoint=scanQueue.shift();if(endpoint)startSetup('Inspect',endpoint);
}
function acceptSetup(data){
  // Verification is still part of the operation the user started. Do not
  // briefly restore the device actions while the helper checks its result.
  if(!pendingSetup&&data.busy&&data.state==='checking'&&setupOperation?.endpoint===data.endpoint){
    data={...data,state:setupProgress(setupOperation.action)};
  }
  // A background check is not a new installation result. Keep the last
  // confirmed state until its completed report replaces it, so tray options
  // and the editor do not disappear whenever the inspection cache expires.
  if(data.busy&&data.state==='checking'&&installation(data)==='unknown'){
    const known=installation(setupReports.get(data.endpoint));
    if(known!=='unknown')data={...data,installation:known};
  }
  SETUP=data;if(data.endpoint)setupReports.set(data.endpoint,data);
  if(!data.busy){
    // Share the latest engine state, retaining an authorized endpoint for that
    // same build if the latest device cannot manage it. A newer engine state
    // must never resurrect an older per-device offer.
    if(data.engineState){
      const eligible=s=>s.engineState===data.engineState&&s.installedEngine===data.installedEngine&&s.availableEngine===data.availableEngine&&(s.canUpdate||s.canRecoverUpdate);
      if(eligible(data))engineSetup=data;
      else if(!engineSetup||!eligible(engineSetup)||engineSetup.endpoint===data.endpoint)engineSetup=[...setupReports.values()].find(s=>!s.busy&&eligible(s))||data;
    }else if(data.endpoint===engineSetup?.endpoint||setupOperation?.endpoint===data.endpoint)engineSetup=null;
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
  updateEditorAvailability();updateSettingsStatus();statusbar();
  publishTray();
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
    const previous=setupReports.get(endpoint);
    SETUP={...previous,installation:installation(previous),endpoint,busy:true,state:setupProgress(action)};
    setupReports.set(endpoint,SETUP);updateEditorAvailability();updateSettingsStatus();
    const result=await rpc('setup',{action,endpoint});scanStarting=false;acceptSetup(result);
  }catch(error){scanStarting=false;if(action!=='Inspect')setupOperation=null;acceptSetup({...SETUP,busy:false,state:'blocked',notice:error.message,canInstall:false,canRemove:false});}
}
function openSetup(){return openSettings('outputs');}
function engineAct(action){
  if(action==='cancel'&&['Update','RecoverUpdate'].includes(pendingSetup?.action)){
    pendingSetup=null;setupOperation=null;updateSettingsStatus();scanNext();return;
  }
  if(setupOperation||!engineSetup)return;
  if(action==='update'&&engineSetup.canUpdate)setupRequest('Update',engineSetup.endpoint);
  else if(action==='recover'&&engineSetup.canRecoverUpdate)setupRequest('RecoverUpdate',engineSetup.endpoint);
}
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
  if(e.target.closest('[data-start-with-windows]')){if(STARTUP?.available)startupSettings(!STARTUP.enabled);return;}
  if(e.target.closest('[data-start-in-tray]')){command('preferences',{startInTray:!APP.startInTray},true);return;}
  if(e.target.closest('[data-close-to-tray]')){command('preferences',{closeToTray:APP.closeToTray===false},true);return;}
  const tab=e.target.closest('[data-settings-tab]');if(tab){S.settingsTab=tab.dataset.settingsTab;render();if(S.settingsTab==='outputs')scanNext();else startupSettings();return;}
  const theme=e.target.closest('[data-theme-choice]');if(theme){S.theme=theme.dataset.themeChoice;localStorage.setItem('theme',S.theme);render();syncWindowTheme();return;}
  const engine=e.target.closest('[data-engine-act]');if(engine){engineAct(engine.dataset.engineAct);return;}
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
