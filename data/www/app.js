'use strict';
const el=id=>document.getElementById(id);
const size=n=>`${(Number(n)/1048576).toLocaleString('de-DE',{maximumFractionDigits:1})} MiB`;
let busy=false;
async function copyPath(path,button){
  try{await navigator.clipboard.writeText(path);const old=button.textContent;button.textContent='Kopiert ✓';setTimeout(()=>{button.textContent=old;},1500);}
  catch(_){window.prompt('Freigabepfad kopieren:',path);}
}
async function refresh(){
  if(busy)return;busy=true;
  const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),7000);
  try{
    const response=await fetch('/api/status',{cache:'no-store',signal:controller.signal});
    if(!response.ok)throw new Error(`HTTP ${response.status}`);
    const s=await response.json();el('led').className='led ok';
    el('connection').textContent=s.smbReady?'Verbunden · SMB bereit · Gastzugriff':'Verbunden · SMB wird gestartet';
    el('ip').textContent=s.ip;el('rssi').textContent=`${s.rssi} dBm`;
    const seconds=Number(s.uptimeSeconds);el('uptime').textContent=`${Math.floor(seconds/3600)} h ${Math.floor(seconds%3600/60)} min`;
    el('heap').textContent=`${Math.round(s.heapFree/1024)} KiB`;el('psram').textContent=`${size(s.psramFree)} / ${size(s.psramSize)}`;
    el('clients').textContent=`${s.smbClients} · ${s.smbHandles} offene Dateien`;
    s.volumes.forEach((v,index)=>{
      const id=index?'sd':'flash';el(`${id}-used`).textContent=v.ready?`${size(v.used)} belegt`:'Nicht verfügbar';
      el(`${id}-progress`).value=v.total?Math.round(v.used/v.total*100):0;
      el(`${id}-capacity`).textContent=v.ready?`${size(v.total-v.used)} frei von ${size(v.total)}`:(index?s.sdStatus:'LittleFS-Image hochladen');
      const path=String.fromCharCode(92,92)+s.ip+String.fromCharCode(92)+v.name;
      const button=el(`${id}-share`);button.textContent=path;button.disabled=!v.ready;button.onclick=()=>copyPath(path,button);
    });
    el('updated').textContent=`Aktualisiert ${new Date().toLocaleTimeString('de-DE')}`;
  }catch(_){el('led').className='led';el('connection').textContent='Gerät nicht erreichbar · erneuter Versuch folgt';}
  finally{clearTimeout(timeout);busy=false;}
}
el('refresh').addEventListener('click',refresh);refresh();setInterval(refresh,10000);
