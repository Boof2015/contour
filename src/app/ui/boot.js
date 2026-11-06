'use strict';
// Keep startup failures visible even if another script fails before connecting.
window.addEventListener('error',event=>{
  if(!document.querySelector('.tb')){
    const root=document.getElementById('win');
    root.classList.add('fatal');root.textContent='The editor could not load. Restart Contour. '+(event.message||'');
  }
});
