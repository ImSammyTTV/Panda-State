/* The bar's own page. One file, and nothing it needs comes from elsewhere: it
   has to work from the setup network, where there is no internet. The add-on
   store is the one exception, and it is optional: it reads a folder on GitHub
   only when opened. Dark glass cards, sized for a phone as much as a desktop. */
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
.head2{display:flex;align-items:center;gap:8px;margin-bottom:12px}.head2 h2{margin:0}.head2 .sp{margin-left:auto;display:flex;gap:8px}
.tiles,.store{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:10px;margin-top:14px}
.tile{display:flex;flex-direction:column;gap:6px;padding:14px 16px;border-radius:20px;background:rgba(255,255,255,.045);border:1px solid var(--line);min-width:0}
.tile[data-on]{border-color:color-mix(in srgb,var(--c) 55%,var(--line));background:color-mix(in srgb,var(--c) 12%,rgba(255,255,255,.04))}
.tile .t{display:flex;align-items:center;gap:8px;font-weight:600;min-width:0}.tile .t span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tile .d{font-size:12.5px;color:var(--t2);display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}
.tile .acts{display:flex;gap:6px;margin-top:auto;padding-top:4px}.tile button{height:30px;font-size:12.5px}
.dot{flex:none;width:8px;height:8px;border-radius:50%;background:#64748b}.dot[data-live]{background:#22c55e;box-shadow:0 0 0 3px rgba(34,197,94,.18)}.dot[data-bad]{background:#f59e0b}
.tag{margin-left:auto;flex:none;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:600;color:var(--t2);background:rgba(255,255,255,.07)}
.empty{padding:16px;border-radius:18px;border:1px dashed var(--line);color:var(--t3);font-size:13px}
.sw{display:grid;grid-template-columns:repeat(25,1fr);gap:2px;height:10px;margin:2px 0}.sw i{border-radius:2px;background:#222}
.panel{margin-top:14px;padding:14px;border-radius:22px;background:rgba(0,0,0,.18);border:1px solid var(--line)}
details{margin-top:14px;border-top:1px solid var(--line);padding-top:12px}summary{cursor:pointer;color:var(--t2);font-size:13px}
.upd{display:flex;align-items:center;gap:14px;flex-wrap:wrap;padding:14px 16px;border-radius:20px;background:rgba(255,255,255,.045);border:1px solid var(--line)}
.upd b{font-size:15px}
.prog{height:6px;border-radius:99px;background:rgba(255,255,255,.08);overflow:hidden;margin-top:10px}.prog i{display:block;height:100%;width:0;background:var(--c);transition:width .4s}
.tog{display:inline-flex;align-items:center;gap:8px;font-size:13px;color:var(--t2)}
[hidden]{display:none!important}
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
  <div class="head2"><h2>Add-ons</h2><span class="sp"><button id="adup">Upload</button><input id="adfile" type="file" accept=".json,application/json" hidden></span></div>
  <div class="sub">Show something else on the bar when the printer has nothing to say: an add-on from the store, a file of your own, or any program on your network.</div>
  <div class="tiles" id="adtiles"></div>
  <div class="line"><label for="adwhen">Show the picked one</label><select id="adwhen" style="width:auto;max-width:100%"><option value="idle">while the printer is not busy with a print</option><option value="always">always, even while printing</option></select><button id="adnone">Show nothing</button></div>
  <div class="panel" id="storepanel">
    <div class="head2" style="margin:0"><h2>Store</h2><span class="sp"><span class="sub" id="storenote"></span></span></div>
    <div class="store" id="storelist"></div>
  </div>
  <div class="msg" id="admsg"></div>
  <details><summary>Build your own</summary>
    <div class="sub" style="margin-top:10px">An add-on <b>file</b> is a few lines of JSON the bar runs itself: a look, meters, an animation, or a live meter read from a web address. Upload one here, or add yours to the store's folder on GitHub (<a href="https://github.com/ImSammyTTV/panda-status-p2-firmware/tree/main/addons/store" target="_blank" rel="noopener">format and examples</a>).</div>
    <div class="sub" style="margin-top:10px">An add-on <b>program</b> runs anywhere on your network and posts to <code>/api/addon</code>, again and again. Every call is listed at <a href="/api">/api</a>. A meter at 63%:<br><code>curl -X POST http://<span id="myip"></span>/api/addon -H "content-type: application/json" -d '{"id":"demo","name":"Demo","meters":[{"value":63,"colour":"#D97757"}]}'</code></div>
  </details>
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
  <div class="upd"><div class="grow"><b id="wifititle">…</b><div class="sub" id="wifisub"></div></div><button id="scan">Scan</button></div>
  <div class="line"><input class="grow" id="ssid" type="text" list="nets" placeholder="Network name" autocomplete="off"><datalist id="nets"></datalist><input class="grow" id="pass" type="password" placeholder="Password" autocomplete="off"><button class="go" id="wifi">Save and join</button></div>
  <div class="msg" id="wifimsg"></div>
  <h2 style="margin-top:18px">The bar's own hotspot</h2>
  <div class="upd"><div class="grow"><b id="aptitle">…</b><div class="sub" id="apsub"></div></div></div>
  <div class="line"><label for="apmode">Hotspot</label><select id="apmode" style="width:300px"><option value="fallback">On only when Wi-Fi does not connect</option><option value="always">Always on</option></select></div>
  <div class="line"><input class="grow" id="apssid" type="text" placeholder="Hotspot name" autocomplete="off"><input class="grow" id="appass" type="password" placeholder="New password (8+ characters), or leave empty" autocomplete="new-password"><button id="apsave">Save</button></div>
  <div class="msg" id="apmsg"></div>
</section>

<section class="card">
  <h2>Firmware</h2>
  <div class="upd">
    <div class="grow"><b id="updtitle">…</b><div class="sub" id="updsub"></div></div>
    <button id="updcheck">Check for updates</button><button class="go" id="updinstall" hidden>Install</button>
  </div>
  <div class="prog" id="updprog" hidden><i></i></div>
  <div class="line"><label class="tog"><input type="checkbox" id="updauto"> Look for a new release once a day (it is never installed without you)</label></div>
  <div class="sub" id="slots" style="margin-top:12px"></div>
  <div class="line"><input class="grow" id="bin" type="file" accept=".bin"><button id="up">Install file</button></div>
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
/* The store is a folder on GitHub, read by this page (so it needs internet on the device showing the page, not on the bar). Using an add-on hands the bar its address; the bar fetches it from there each time it starts and keeps nothing else. */
const STORE='https://raw.githubusercontent.com/ImSammyTTV/panda-status-p2-firmware/main/addons/store/';
const esc=(t)=>String(t??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const ago=(n)=>n<60?n+' s':n<3600?Math.round(n/60)+' min':n<172800?Math.round(n/3600)+' h':Math.round(n/86400)+' days';
let last=null;
function pick(id){busy=Date.now();post('/api/config',{addon:{show:id}}).then(r=>{$('admsg').textContent=r.ok?'':(r.message||'Not saved');});}
function renderAddons(s){
  const f=s.addonFile, want=s.config.addon.show, tiles=[];
  if(f.from||f.loading||f.error){
    const posted=s.addons.find(a=>a.id===f.id);
    tiles.push({id:f.id,name:f.name||(f.loading?'Loading…':'Add-on file'),desc:f.error||f.description,tag:f.from==='store'?'store':'file',
      sub:[f.kind!=='none'?f.kind:'',f.value!==undefined?'reads '+(+f.value).toPrecision(4):'',f.author?'by '+f.author:''].filter(Boolean).join(' · '),live:posted&&posted.fresh,bad:!!f.error,file:true});
  }
  for(const a of s.addons){if(f.from&&a.id===f.id)continue;tiles.push({id:a.id,name:a.name,tag:'program',sub:a.kind+' · '+(a.fresh?'posted '+a.age+' s ago':'stopped posting'),live:a.fresh});}
  if(want&&!tiles.some(t=>t.id===want))tiles.push({id:want,name:want,desc:'Picked, but not posting just now',tag:'program',live:false});
  const html=tiles.length?tiles.map(t=>'<div class="tile"'+(t.id&&t.id===want?' data-on':'')+'><div class="t"><i class="dot"'+(t.live?' data-live':'')+(t.bad?' data-bad':'')+'></i><span>'+esc(t.name)+'</span><em class="tag">'+t.tag+'</em></div>'
    +(t.desc?'<div class="d">'+esc(t.desc)+'</div>':'')+'<div class="sub">'+esc(t.sub||'')+(s.addonShowing&&s.addonShowing===t.id?' · on the bar now':'')+'</div>'
    +'<div class="acts">'+(t.id?(t.id===want?'<button class="go" disabled>Picked</button>':'<button data-pick="'+esc(t.id)+'">Show this</button>'):'')+(t.file?'<button data-remove="1">Remove</button>':'')+'</div></div>').join('')
    :'<div class="empty">No add-ons yet. Open the store, upload a file, or have a program post to <code>/api/addon</code>.</div>';
  if(html!==window.adHtml){window.adHtml=html;$('adtiles').innerHTML=html;}
}
$('adtiles').onclick=async(e)=>{const b=e.target.closest('button');if(!b)return;if(b.dataset.pick)pick(b.dataset.pick);
  if(b.dataset.remove){if(!confirm('Remove this add-on from the bar?'))return;const r=await post('/api/addonfile',{remove:true});$('admsg').textContent=r.message||'';}};
$('adnone').onclick=()=>pick('');
$('adup').onclick=()=>$('adfile').click();
$('adfile').onchange=async()=>{const file=$('adfile').files[0];$('adfile').value='';if(!file)return;let obj;
  try{obj=JSON.parse(await file.text());}catch(e){$('admsg').textContent='That file is not valid JSON';return;}
  const r=await post('/api/addonfile',{file:obj});$('admsg').textContent=r.ok?'Uploaded. It shows as soon as it has loaded, in place of any add-on file the bar had.':(r.message||'Not accepted');};
/* Always shown, so there is nothing to find. Read once when the page opens, and again after an add-on is picked so the card it came from says In use. */
async function openStore(){
  if(!$('storelist').children.length)$('storenote').textContent='Loading…';
  let idx;try{idx=await fetch(STORE+'index.json',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;return r.json();});}
  catch(e){$('storenote').textContent='Could not reach the store: it needs internet on this device';$('storelist').innerHTML='';return;}
  $('storenote').textContent=idx.addons.length+' from GitHub · one at a time';
  $('storelist').innerHTML=idx.addons.map(a=>{const url=STORE+a.file,using=last&&last.addonFile.url===url,cols=(a.preview||'').match(/[0-9a-fA-F]{6}/g)||[];
    return '<div class="tile"'+(using?' data-on':'')+'><div class="t"><span>'+esc(a.name)+'</span><em class="tag">'+esc(a.kind)+'</em></div>'
      +(cols.length===25?'<div class="sw">'+cols.map(c=>'<i style="background:#'+c+'"></i>').join('')+'</div>':'')
      +'<div class="d">'+esc(a.description)+'</div><div class="sub">'+(a.author?'by '+esc(a.author):'')+'</div>'
      +'<div class="acts">'+(using?'<button class="go" disabled>In use</button>':'<button data-url="'+esc(url)+'">Use this</button>')+'</div></div>';}).join('');
}
openStore();
$('storelist').onclick=async(e)=>{const b=e.target.closest('button');if(!b||!b.dataset.url)return;b.disabled=true;b.textContent='Adding…';
  const r=await post('/api/addonfile',{url:b.dataset.url});$('admsg').textContent=r.ok?'Added. The bar is fetching it from GitHub.':(r.message||'Not accepted');setTimeout(openStore,3000);};
function renderUpdate(s){
  const u=s.update,v=(t)=>String(t).replace(/^v/,'');
  $('updtitle').textContent=u.phase==='downloading'?'Installing '+v(u.latest)+' · '+u.percent+'%':u.phase==='installed'?u.message
    :u.phase==='available'?'Version '+v(u.latest)+' is available':u.phase==='checking'?'Looking on GitHub…'
    :u.phase==='current'?'Up to date · '+s.version:'Version '+s.version;
  const sub=u.phase==='failed'?esc(u.message)
    :u.phase==='available'?'It goes into the other slot (replacing what is there) and starts on trial. <a href="'+esc(u.page)+'" target="_blank" rel="noopener">What is new</a>'
    :u.phase==='downloading'?'Downloading from GitHub and checking it against the release\'s checksum. The bar restarts when it is done.'
    :u.checkedAgo!==null?'Checked GitHub '+ago(u.checkedAgo)+' ago':'Releases come from github.com/'+esc(u.repo);
  if(sub!==window.updSub){window.updSub=sub;$('updsub').innerHTML=sub;}
  $('updinstall').hidden=u.phase!=='available';$('updinstall').textContent='Install '+v(u.latest);
  $('updcheck').disabled=u.phase==='checking'||u.phase==='downloading';
  $('updprog').hidden=u.phase!=='downloading';$('updprog').firstChild.style.width=u.percent+'%';
  if(Date.now()-busy>3000)$('updauto').checked=u.auto;
}
function renderWifi(s){
  $('wifititle').textContent=s.wifiConnected?'Connected to '+s.wifiName:s.wifiName?'Not connected to '+s.wifiName:'No Wi-Fi network saved';
  $('wifisub').textContent=s.wifiConnected?s.ip+' · signal '+s.rssi+' dBm':'Trying again every 30 seconds. Its own hotspot comes up after a minute without Wi-Fi.';
  const a=s.ap;
  $('aptitle').textContent=a.up?'On: '+a.ssid:'Off until Wi-Fi fails';
  const sub=a.up?'Join "'+a.ssid+'"'+(a.defaultPassword?' (password '+a.defaultPassword+')':'')+', then open http://'+a.ip+' · '+a.clients+' connected'
    :'If the bar cannot reach Wi-Fi for a minute it starts "'+a.ssid+'"'+(a.defaultPassword?', password '+a.defaultPassword:'')+', at http://'+a.ip+'. Holding the button for five seconds starts it too.';
  $('apsub').textContent=sub;
  if(Date.now()-busy>3000&&document.activeElement!==$('apssid')){$('apmode').value=a.always?'always':'fallback';if(!$('apssid').value)$('apssid').value=a.ssid;}
}
async function scanNets(again){
  $('scan').disabled=true;$('wifimsg').textContent='Looking for networks…';
  /* The bar's radio leaves its channel while it scans, so a request now and then goes unanswered: keep asking. */
  for(let i=0;i<20;i++){let r={};try{r=await fetch('/api/wifi/scan'+(again&&i===0?'?again':'')).then(x=>x.json());}catch(e){}
    if(r.networks){$('nets').innerHTML=r.networks.map(n=>'<option value="'+esc(n.ssid)+'">'+n.rssi+' dBm'+(n.open?' · open':'')+'</option>').join('');
      $('wifimsg').textContent=r.networks.length+' networks found: click the name box to pick one';$('scan').disabled=false;return;}
    await new Promise(z=>setTimeout(z,800));}
  $('wifimsg').textContent='The scan did not finish';$('scan').disabled=false;
}
$('scan').onclick=()=>scanNets(true);
$('apsave').onclick=async()=>{busy=Date.now();const body={always:$('apmode').value==='always',ssid:$('apssid').value.trim()};if($('appass').value)body.password=$('appass').value;
  const r=await post('/api/ap',body);$('apmsg').textContent=r.message||(r.ok?'Saved':'Not saved');if(r.ok)$('appass').value='';};
$('apmode').onchange=$('apsave').onclick;
$('updcheck').onclick=()=>post('/api/update',{check:true});
$('updinstall').onclick=async()=>{if(!confirm('Install '+$('updinstall').textContent.replace('Install ','')+' from GitHub? It replaces the firmware in the other slot, and the bar restarts into it.'))return;const r=await post('/api/update',{install:true});if(!r.ok)$('fwmsg').textContent=r.message||'';};
$('updauto').onchange=()=>{busy=Date.now();post('/api/update',{auto:$('updauto').checked});};
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
  /* Capitalised for show only: other programs recognise the firmware by its lower-case name in /api/status. */
  $('fw').textContent=s.firmware.charAt(0).toUpperCase()+s.firmware.slice(1)+' '+s.version;
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
  last=s; renderAddons(s); renderUpdate(s); renderWifi(s);
  if(Date.now()-busy>3000) $('adwhen').value=s.config.addon.when;
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
