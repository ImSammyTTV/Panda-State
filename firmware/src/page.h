/* The bar's own page. One file, no external requests: it has to work from the
   setup network, where there is no internet. Dark glass cards, sized for a phone
   as much as a desktop. */
#pragma once
#include <Arduino.h>

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Panda Status</title>
<style>
:root{--bg:#07090e;--card:#0f131b;--line:rgba(255,255,255,.08);--text:#eef1f6;--t2:#aab2c0;--t3:#6f7888;--c:#009dff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:860px;margin:0 auto;padding:22px 16px 60px;display:flex;flex-direction:column;gap:16px}
.card{position:relative;overflow:hidden;border-radius:28px;padding:22px 24px;border:1px solid color-mix(in srgb,var(--c) 18%,var(--line));
background:linear-gradient(160deg,color-mix(in srgb,var(--c) 18%,#0c1018),#0a0d14 70%)}
.head{display:flex;align-items:center;gap:12px}.head b{font-size:16px;font-weight:600}
.chip{padding:4px 11px;border-radius:999px;font-size:12px;font-weight:600;background:color-mix(in srgb,var(--c) 28%,transparent)}
.more{margin-left:auto;font-size:13px;color:var(--t3)}
.big{font-size:54px;font-weight:250;letter-spacing:-.04em;margin:18px 0 2px;text-transform:capitalize}
.sub{font-size:13px;color:var(--t3)}
.bar{display:grid;grid-template-columns:repeat(25,1fr);gap:4px;margin-top:20px;padding:12px;border-radius:20px;background:rgba(255,255,255,.05);border:1px solid var(--line)}
.bar i{height:22px;border-radius:6px;background:#111}
h2{font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--t3);margin:0 0 12px;font-weight:600}
.row{display:grid;grid-template-columns:88px 44px 44px minmax(0,1fr) minmax(0,1fr) auto;gap:10px;align-items:center;padding:8px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0}.row span{text-transform:capitalize}
input[type=color]{width:44px;height:32px;padding:0;border:0;border-radius:10px;background:none}
select,input[type=text],input[type=password],input[type=file]{width:100%;height:34px;border-radius:12px;border:1px solid var(--line);background:rgba(255,255,255,.06);color:var(--text);padding:0 10px;font:inherit;font-size:13.5px}
/* The open list of a select is drawn by the browser, not by the rule above: without these it came up as white text on a white list. */
select,input[type=time]{color-scheme:dark}
input[type=time]{height:34px;border-radius:12px;border:1px solid var(--line);background:rgba(255,255,255,.06);color:var(--text);padding:0 10px;font:inherit;font-size:13.5px}
code{font:12px ui-monospace,Consolas,monospace;color:var(--t2);word-break:break-all}a{color:var(--text)}
select option{background:#141922;color:#eef1f6}
input[type=file]{padding:6px 10px;height:auto}
input[type=range]{width:100%;accent-color:var(--c)}
button{height:34px;padding:0 14px;border-radius:999px;border:0;background:rgba(255,255,255,.09);color:var(--text);font:inherit;font-size:13px;cursor:pointer;white-space:nowrap}
button:hover{background:rgba(255,255,255,.17)}button.go{background:var(--text);color:#0a0d14}
.line{display:flex;flex-wrap:wrap;align-items:center;gap:10px;margin-top:12px}.line label{font-size:13px;color:var(--t2)}
.grow{flex:1;min-width:140px}.msg{font-size:13px;color:var(--t2);min-height:18px;margin-top:10px}
@media(max-width:560px){.row{grid-template-columns:76px 40px 40px 1fr auto}.row .spd{display:none}.big{font-size:40px}}
</style></head><body><main>

<section class="card" id="now">
  <div class="head"><b>Panda Status</b><span class="chip" id="fw">…</span><span class="more" id="net"></span></div>
  <div class="big" id="state">…</div>
  <div class="sub" id="said"></div>
  <div class="bar" id="bar"></div>
  <div class="line"><label for="bri">Brightness</label><input class="grow" id="bri" type="range" min="0" max="100"><span class="sub" id="briv"></span></div>
  <div class="line"><label>Mode</label><button id="mstatus">Follow the printer</button><button id="mmusic">Music</button>
    <label for="sens">Music sensitivity</label><select id="sens" style="width:110px"><option>low</option><option>medium</option><option>high</option><option>auto</option></select>
    <span class="sub" id="mic"></span></div>
  <div class="sub" style="margin-top:10px">Button on the bar: press once for the next Music sensitivity, twice to switch mode, hold five seconds for the setup Wi-Fi network.</div>
</section>

<section class="card">
  <h2>What each state looks like</h2>
  <div id="rows"></div>
  <div class="line">
    <label for="order">Colour order</label><select id="order" style="width:90px"></select>
    <label><input type="checkbox" id="rev"> Run the other way along the bar</label>
  </div>
  <div class="msg" id="cfgmsg"></div>
</section>

<section class="card">
  <h2>Schedule</h2>
  <div class="line"><button id="schon"></button><label for="schfrom">Dark from</label><input id="schfrom" type="time" style="width:120px"><label for="schto">back on at</label><input id="schto" type="time" style="width:120px"><label for="schlvl">at brightness</label><input id="schlvl" type="text" inputmode="numeric" style="width:56px"><label>%</label><button id="schsave">Save</button></div>
  <div class="line"><label for="zone">Time zone</label><select id="zone" style="width:230px"></select><span class="sub" id="clock"></span></div>
  <div class="msg" id="schmsg"></div>
</section>

<section class="card">
  <h2>Add-ons</h2>
  <div class="sub">Other programs on your network can use the bar, for example a meter of how much of something has been used. They post to the bar; you choose here whether one is shown.</div>
  <div class="line"><label for="adshow">Show</label><select id="adshow" style="width:200px"></select><label for="adwhen">when</label><select id="adwhen" style="width:270px"><option value="idle">the printer is not busy with a print</option><option value="always">always, even while printing</option></select></div>
  <div class="sub" id="adlist" style="margin-top:10px"></div>
  <div class="sub" style="margin-top:10px">To write one: post JSON to <code>/api/addon</code> and keep posting. The full list of calls is at <a href="/api">/api</a>. Smallest example, a meter at 63%:<br><code>curl -X POST http://<span id="myip"></span>/api/addon -H "content-type: application/json" -d '{"id":"demo","name":"Demo","meters":[{"value":63,"colour":"#D97757"}]}'</code></div>
</section>

<section class="card">
  <h2>Printer</h2>
  <div class="sub" id="prstate"></div>
  <div class="line"><input class="grow" id="prip" type="text" placeholder="Printer address, e.g. 192.168.1.50" autocomplete="off"><input class="grow" id="prsn" type="text" placeholder="Serial number" autocomplete="off"><input class="grow" id="prcode" type="password" placeholder="Access code" autocomplete="off"></div>
  <div class="line"><button class="go" id="pair">Pair</button><button id="unpair">Unpair</button><span class="sub">On the printer: Settings, then LAN Only or Network, shows the address and the access code.</span></div>
  <div class="line"><label for="cmin">After a print, cooling for</label><input id="cmin" type="text" inputmode="numeric" style="width:64px"><label for="cbed">minutes and until the bed is at</label><input id="cbed" type="text" inputmode="numeric" style="width:64px"><label>&deg;C</label><button id="coolsave">Save</button></div>
  <div class="msg" id="prmsg"></div>
</section>

<section class="card">
  <h2>Wi-Fi</h2>
  <div class="line"><input class="grow" id="ssid" type="text" placeholder="Network name" autocomplete="off"><input class="grow" id="pass" type="password" placeholder="Password" autocomplete="off"><button id="wifi">Save and join</button></div>
  <div class="msg" id="wifimsg"></div>
</section>

<section class="card">
  <h2>Firmware</h2>
  <div class="sub" id="slots"></div>
  <div class="line"><input class="grow" id="bin" type="file" accept=".bin"><button class="go" id="up">Install</button></div>
  <div class="line"><button id="other">Start the firmware in the other slot</button><button id="reboot">Restart</button><button id="factory">Reset settings</button></div>
  <div class="msg" id="fwmsg"></div>
</section>

</main><script>
const $=(id)=>document.getElementById(id);
const post=(path,body)=>fetch(path,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body||{})}).then(r=>r.json()).catch(()=>({ok:false,message:'The bar did not answer'}));
const STATES=['idle','printing','paused','finished','cooling','ready','error','offline'];
let built=false, busy=0;
const bar=$('bar'); for(let i=0;i<25;i++) bar.appendChild(document.createElement('i'));

function build(s){
  built=true;
  $('rows').innerHTML='';
  for(const name of STATES){
    const look=s.config.states[name]; const row=document.createElement('div'); row.className='row';
    row.innerHTML='<span>'+name+'</span><input type="color" title="Colour"><input type="color" title="Second colour, for the two-colour effects"><select></select><input class="spd" type="range" min="1" max="100" title="Speed"><button>Try</button>';
    const [,colour,colour2,effect,speed,tryIt]=row.children;
    colour.value=look.colour.toLowerCase(); colour2.value=look.colour2.toLowerCase(); speed.value=look.speed;
    for(const fx of s.effects){const o=document.createElement('option');o.value=o.textContent=fx;effect.appendChild(o);} effect.value=look.effect;
    /* The second colour is only sent once it has been touched, so an untouched one keeps following the first. */
    let chose2=!look.colour2Auto;
    const save=async()=>{busy=Date.now();const one={colour:colour.value,effect:effect.value,speed:+speed.value};if(chose2)one.colour2=colour2.value;const r=await post('/api/config',{states:{[name]:one}});$('cfgmsg').textContent=r.ok?'Saved':(r.message||'Not saved');};
    colour.onchange=save; effect.onchange=save; speed.onchange=save; colour2.onchange=()=>{chose2=true;save();};
    tryIt.onclick=()=>post('/api/preview',{state:name,progress:60,seconds:8});
    $('rows').appendChild(row);
  }
  for(const o of ['GRB','RGB','BRG','BGR','RBG','GBR']){const e=document.createElement('option');e.value=e.textContent=o;$('order').appendChild(e);}
  $('order').value=s.config.order; $('rev').checked=s.config.reversed; $('bri').value=s.config.brightness;
  $('prip').value=s.printer.ip||''; $('prsn').value=s.printer.serial||''; $('cmin').value=s.config.cool.minutes; $('cbed').value=s.config.cool.target;
  const sc=s.config.schedule; schedOn=sc.enabled; paintSched(); $('schfrom').value=sc.from; $('schto').value=sc.to; $('schlvl').value=sc.level;
  /* The zone list: the saved one first if it is not a listed zone, then the browser's own, then the table. */
  const add=(label,tz,zone,pick)=>{const o=document.createElement('option');o.textContent=label;o.value=tz;o.dataset.zone=zone;$('zone').appendChild(o);if(pick)o.selected=true;};
  const saved=s.config.time; let picked=false;
  for(const [zone,tz] of Object.entries(ZONES)){const pick=!picked&&zone===saved.zone&&tz===saved.tz;add(zone.replace(/_/g,' '),tz,zone,pick);picked=picked||pick;}
  if(!ZONES[myZone]){const [label,tz]=fixedZone();const pick=!picked&&tz===saved.tz;add(label,tz,label,pick);picked=picked||pick;}
  if(!picked) add(saved.zone+' (saved)',saved.tz,saved.zone,true);
  /* A bar that has never been told its zone is offered the browser's, but nothing is saved until Save or a change. */
  if(saved.tz==='UTC0'&&ZONES[myZone]) [...$('zone').options].forEach(o=>{if(o.dataset.zone===myZone)o.selected=true;});
  $('myip').textContent=s.ip;
}
/* Time zones as the rule strings the bar's clock understands (they carry the daylight-saving dates). The browser's own zone is picked if it is in the list; otherwise its current offset is offered, which will not follow daylight saving. */
const ZONES={'UTC':'UTC0','Australia/Melbourne':'AEST-10AEDT,M10.1.0,M4.1.0/3','Australia/Sydney':'AEST-10AEDT,M10.1.0,M4.1.0/3','Australia/Hobart':'AEST-10AEDT,M10.1.0,M4.1.0/3','Australia/Brisbane':'AEST-10','Australia/Adelaide':'ACST-9:30ACDT,M10.1.0,M4.1.0/3','Australia/Darwin':'ACST-9:30','Australia/Perth':'AWST-8','Pacific/Auckland':'NZST-12NZDT,M9.5.0,M4.1.0/3','Asia/Tokyo':'JST-9','Asia/Seoul':'KST-9','Asia/Shanghai':'CST-8','Asia/Hong_Kong':'HKT-8','Asia/Singapore':'<+08>-8','Asia/Kolkata':'IST-5:30','Asia/Dubai':'<+04>-4','Europe/London':'GMT0BST,M3.5.0/1,M10.5.0','Europe/Dublin':'GMT0IST,M3.5.0/1,M10.5.0','Europe/Lisbon':'WET0WEST,M3.5.0/1,M10.5.0','Europe/Paris':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Berlin':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Amsterdam':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Madrid':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Rome':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Stockholm':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Warsaw':'CET-1CEST,M3.5.0,M10.5.0/3','Europe/Helsinki':'EET-2EEST,M3.5.0/3,M10.5.0/4','Europe/Athens':'EET-2EEST,M3.5.0/3,M10.5.0/4','America/New_York':'EST5EDT,M3.2.0,M11.1.0','America/Toronto':'EST5EDT,M3.2.0,M11.1.0','America/Chicago':'CST6CDT,M3.2.0,M11.1.0','America/Denver':'MST7MDT,M3.2.0,M11.1.0','America/Phoenix':'MST7','America/Los_Angeles':'PST8PDT,M3.2.0,M11.1.0','America/Vancouver':'PST8PDT,M3.2.0,M11.1.0','America/Anchorage':'AKST9AKDT,M3.2.0,M11.1.0','Pacific/Honolulu':'HST10','America/Sao_Paulo':'<-03>3'};
const myZone=(()=>{try{return Intl.DateTimeFormat().resolvedOptions().timeZone;}catch(e){return '';}})();
function fixedZone(){const m=-new Date().getTimezoneOffset(),h=Math.trunc(Math.abs(m)/60),mm=Math.abs(m)%60;const label='UTC'+(m<0?'-':'+')+h+(mm?':'+String(mm).padStart(2,'0'):'');return [label+' (no daylight saving)','<'+(m<0?'-':'+')+String(h).padStart(2,'0')+(mm?String(mm).padStart(2,'0'):'')+'>'+(m<0?'':'-')+h+(mm?':'+String(mm).padStart(2,'0'):'')];}
let schedOn=false;
function paintSched(){ $('schon').textContent=schedOn?'On':'Off'; $('schon').className=schedOn?'go':''; }
$('schon').onclick=()=>{schedOn=!schedOn;paintSched();saveSched();};
async function saveSched(){busy=Date.now();const o=$('zone').selectedOptions[0];const r=await post('/api/config',{schedule:{enabled:schedOn,from:$('schfrom').value,to:$('schto').value,level:+$('schlvl').value},time:{tz:o.value,zone:o.dataset.zone}});$('schmsg').textContent=r.ok?'Saved':(r.message||'Not saved');}
$('schsave').onclick=saveSched; $('zone').onchange=saveSched;
$('adshow').onchange=()=>{busy=Date.now();post('/api/config',{addon:{show:$('adshow').value}});};
$('adwhen').onchange=()=>{busy=Date.now();post('/api/config',{addon:{when:$('adwhen').value}});};
$('mstatus').onclick=()=>{busy=Date.now();post('/api/config',{mode:'status'});};
$('mmusic').onclick=()=>{busy=Date.now();post('/api/config',{mode:'music'});};
$('sens').onchange=()=>{busy=Date.now();post('/api/config',{music:{sensitivity:$('sens').value}});};
$('factory').onclick=async()=>{if(!confirm('Clear every setting this firmware has saved (looks, mode, pairing, Wi-Fi) and restart?'))return;const r=await post('/api/factory');$('fwmsg').textContent=r.message||'';};
$('pair').onclick=async()=>{const r=await post('/api/printer',{ip:$('prip').value,serial:$('prsn').value,code:$('prcode').value});$('prmsg').textContent=r.message||(r.ok?'Saved':'Not saved');if(r.ok)$('prcode').value='';};
$('unpair').onclick=async()=>{if(!confirm('Unpair the bar from the printer?'))return;const r=await post('/api/printer',{forget:true});$('prmsg').textContent=r.message||'';if(r.ok){$('prip').value='';$('prsn').value='';}};
$('coolsave').onclick=async()=>{const r=await post('/api/config',{cool:{minutes:+$('cmin').value,target:+$('cbed').value}});$('prmsg').textContent=r.ok?'Saved':(r.message||'Not saved');};
$('order').onchange=()=>post('/api/config',{order:$('order').value});
$('rev').onchange=()=>post('/api/config',{reversed:$('rev').checked});
$('bri').oninput=()=>{$('briv').textContent=$('bri').value+'%';};
$('bri').onchange=()=>{busy=Date.now();post('/api/config',{brightness:+$('bri').value});};
$('wifi').onclick=async()=>{const r=await post('/api/wifi',{ssid:$('ssid').value,password:$('pass').value});$('wifimsg').textContent=r.message||(r.ok?'Saved':'Not saved');};
$('reboot').onclick=async()=>{const r=await post('/api/reboot');$('fwmsg').textContent=r.message||'';};
$('other').onclick=async()=>{if(!confirm('Restart the bar into the firmware in the other slot?'))return;const r=await post('/api/slot');$('fwmsg').textContent=r.message||'';};
$('up').onclick=()=>{
  const file=$('bin').files[0]; if(!file){$('fwmsg').textContent='Choose a .bin file first';return;}
  const form=new FormData(); form.append('firmware',file,file.name);
  const xhr=new XMLHttpRequest(); xhr.open('POST','/update');
  xhr.upload.onprogress=(e)=>{$('fwmsg').textContent='Sending '+Math.round(e.loaded*100/e.total)+'%';};
  xhr.onload=()=>{let r={};try{r=JSON.parse(xhr.responseText);}catch(e){} $('fwmsg').textContent=r.message||(xhr.status===200?'Installed':'The bar refused the file');};
  xhr.onerror=()=>{$('fwmsg').textContent='The upload did not finish';};
  xhr.send(form);
};

async function tick(){
  let s; try{s=await fetch('/api/status').then(r=>r.json());}catch(e){$('net').textContent='not answering';return;}
  if(!built) build(s);
  $('fw').textContent=s.firmware+' '+s.version;
  $('net').textContent=(s.setupNetwork?'setup network up · ':'')+s.ip+(s.rssi?' · '+s.rssi+' dBm':'');
  $('state').textContent=s.state;
  const look=s.config.states[s.state]; document.documentElement.style.setProperty('--c',look?look.colour:'#64748b');
  const pct=(s.state==='printing'||s.state==='cooling'||s.state==='paused')?' · '+Math.round(s.progress)+'%':'';
  const music=s.mode==='music';
  $('mstatus').className=music?'':'go'; $('mmusic').className=music?'go':'';
  if(Date.now()-busy>3000) $('sens').value=s.config.music.sensitivity;
  $('mic').textContent=s.mic.blocked?'microphone left off: its last start on this build did not finish':!s.mic.running?'microphone starting':!s.mic.found?'the microphone chip did not answer':'microphone '+Math.round(s.mic.db)+' dB · meter '+Math.round(s.mic.level*100)+'%';
  if(music&&s.source!=='preview') $('state').textContent='music';
  $('said').textContent=music&&s.source!=='preview'?'Music mode: the bar follows the sound in the room'
    :s.source==='preview'?'Trying a look for a few seconds'
    :s.source==='controller'?'Told by a controller '+s.linkAge+' s ago'+pct
    :s.source==='printer'?'From the printer itself'+pct
    :s.printer.paired?'Not hearing from the printer'+(s.printer.error?': '+s.printer.error:'')
    :'Not paired with a printer, and no controller is speaking';
  $('clock').textContent=s.clock.known?'The bar makes it '+s.clock.now+(s.clock.dark?' · dark spell on now':''):'The bar has not got the time from the internet yet, so the schedule is waiting';
  /* Add-ons: the picker lists whatever has posted, plus the saved pick even if it is not posting just now. */
  const want=s.config.addon.show, ids=s.addons.map(a=>a.id), opts=['',...ids,...(want&&!ids.includes(want)?[want]:[])];
  /* Rebuilt only when the set of names changes. Compared against what was last built, not against the list itself: an empty list and a list holding only "nothing" both read as the same empty string (0.4.0 showed a blank picker). */
  const adKey=opts.join('|')+'#'+s.addons.map(a=>a.name).join('|');
  if(Date.now()-busy>3000&&adKey!==window.adBuilt){
    window.adBuilt=adKey; $('adshow').innerHTML=''; for(const id of opts){const o=document.createElement('option');o.value=id;const a=s.addons.find(x=>x.id===id);o.textContent=!id?'nothing':a?a.name:id+' (not posting)';$('adshow').appendChild(o);}
  }
  if(Date.now()-busy>3000){$('adshow').value=want;$('adwhen').value=s.config.addon.when;}
  $('adlist').textContent=s.addons.length?s.addons.map(a=>a.name+' ('+a.id+', '+a.kind+', '+(a.fresh?'posted '+a.age+' s ago':'stopped posting')+(s.addonShowing===a.id?', on the bar now':'')+')').join(' · '):'No add-on has posted since the bar started.';
  if(s.addonShowing){const a=s.addons.find(x=>x.id===s.addonShowing);$('state').textContent=a?a.name:s.addonShowing;$('said').textContent='An add-on has the bar';}
  const p=s.printer;
  $('prstate').textContent=!p.paired?'Not paired.'
    :p.connected?'Linked to '+p.ip+(p.gcode?' · printer says '+p.gcode+(p.percent!==undefined?' '+p.percent+'%':'')+(p.bed!==undefined?' · bed '+Math.round(p.bed)+'°':''):' · waiting for its first message')
    :'Paired with '+p.ip+', not linked'+(p.error?': '+p.error:'')+'.';
  [...bar.children].forEach((el,i)=>{el.style.background='#'+s.frame.substr(i*6,6);});
  if(Date.now()-busy>3000){$('bri').value=s.config.brightness;} $('briv').textContent=$('bri').value+'%';
  const o=s.otherSlot;
  $('slots').textContent='Running from '+s.slot+' ('+s.build+')'+(s.onTrial?' · on trial until it has been on Wi-Fi for 20 s':'')+'. Other slot ('+o.label+'): '+(o.project?o.project+' '+o.version+', built '+o.built:'empty')+'.';
  $('other').disabled=!o.project;
}
tick(); setInterval(tick,1000);
</script></body></html>)HTML";
