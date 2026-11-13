/* The bar's own page. One file, and nothing it needs comes from elsewhere: it
   has to work from the setup network, where there is no internet. The add-on
   store is the one exception, and it is optional: it reads a folder on GitHub
   only when opened. Dark glass cards, sized for a phone as much as a desktop. */
#pragma once
#include <Arduino.h>

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Panda State</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='16' fill='%230b0e14'/%3E%3Crect x='6' y='23' width='52' height='18' rx='9' fill='%23171c26' stroke='%23ffffff' stroke-opacity='.14' stroke-width='1.5'/%3E%3Crect x='9' y='28' width='7.6' height='8' rx='2.5' fill='%23ff6a00'/%3E%3Crect x='18.6' y='28' width='7.6' height='8' rx='2.5' fill='%23ffd60a'/%3E%3Crect x='28.2' y='28' width='7.6' height='8' rx='2.5' fill='%2334c759'/%3E%3Crect x='37.8' y='28' width='7.6' height='8' rx='2.5' fill='%2300a8ff'/%3E%3Crect x='47.4' y='28' width='7.6' height='8' rx='2.5' fill='%23bf5af2'/%3E%3C/svg%3E">
<style>
/* Two themes from one set of tokens. --ink is the colour every translucent fill is made from (white on dark, black on light), so a fill keeps its weight in both. Auto follows the device; the switch in the header pins one, per browser. */
:root{--bg:#07090e;--card:#0f131b;--line:rgba(var(--ink),.08);--text:#eef1f6;--t2:#aab2c0;--t3:#6f7888;--c:#009dff;--ink:255,255,255;--cardA:#0c1018;--cardB:#0a0d14;--tint:18%;--off:#111;--panel:rgba(0,0,0,.18);--scheme:dark}
@media(prefers-color-scheme:light){:root:not([data-theme=dark]){--bg:#eef1f5;--card:#fff;--line:rgba(0,0,0,.1);--text:#12161d;--t2:#475062;--t3:#6b7382;--ink:0,0,0;--cardA:#fff;--cardB:#f6f8fb;--tint:10%;--off:#d5dae2;--panel:rgba(0,0,0,.035);--scheme:light}}
:root[data-theme=light]{--bg:#eef1f5;--card:#fff;--line:rgba(0,0,0,.1);--text:#12161d;--t2:#475062;--t3:#6b7382;--ink:0,0,0;--cardA:#fff;--cardB:#f6f8fb;--tint:10%;--off:#d5dae2;--panel:rgba(0,0,0,.035);--scheme:light}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:860px;margin:0 auto;padding:22px 16px 60px;display:flex;flex-direction:column;gap:16px}
.card{position:relative;overflow:hidden;border-radius:28px;padding:22px 24px;border:1px solid color-mix(in srgb,var(--c) 18%,var(--line));
background:linear-gradient(160deg,color-mix(in srgb,var(--c) var(--tint),var(--cardA)),var(--cardB) 70%)}
.head{display:flex;align-items:center;gap:12px}.head b{font-size:16px;font-weight:600}
.chip{padding:4px 11px;border-radius:999px;font-size:12px;font-weight:600;background:color-mix(in srgb,var(--c) 28%,transparent)}
.more{margin-left:auto;font-size:13px;color:var(--t3)}
.big{font-size:54px;font-weight:250;letter-spacing:-.04em;margin:18px 0 2px;text-transform:capitalize}
.sub{font-size:13px;color:var(--t3)}
.bar{display:grid;grid-template-columns:repeat(25,1fr);gap:4px;margin-top:20px;padding:12px;border-radius:20px;background:rgba(var(--ink),.05);border:1px solid var(--line)}
.bar i{height:22px;border-radius:6px;background:var(--off);transition:background-color .2s linear}
h2{font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--t3);margin:0 0 12px;font-weight:600}
.row{display:grid;grid-template-columns:88px 44px 44px minmax(0,1fr) minmax(0,1fr) auto;gap:10px;align-items:center;padding:8px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0}.row span{text-transform:capitalize}
input[type=color]{width:44px;height:32px;padding:0;border:0;border-radius:10px;background:none}
select,input[type=text],input[type=password],input[type=file]{width:100%;height:34px;border-radius:12px;border:1px solid var(--line);background:rgba(var(--ink),.06);color:var(--text);padding:0 10px;font:inherit;font-size:13.5px}
/* The open list of a select is drawn by the browser, not by the rule above: without these it came up as white text on a white list. */
select,input[type=time]{color-scheme:var(--scheme)}
input[type=time]{height:34px;border-radius:12px;border:1px solid var(--line);background:rgba(var(--ink),.06);color:var(--text);padding:0 10px;font:inherit;font-size:13.5px}
code{font:12px ui-monospace,Consolas,monospace;color:var(--t2);word-break:break-all}a{color:var(--text)}
select option{background:var(--card);color:var(--text)}
input[type=file]{padding:6px 10px;height:auto}
input[type=range]{width:100%;accent-color:var(--c)}
button{height:34px;padding:0 14px;border-radius:999px;border:0;background:rgba(var(--ink),.09);color:var(--text);font:inherit;font-size:13px;cursor:pointer;white-space:nowrap}
button:hover{background:rgba(var(--ink),.17)}button.go{background:var(--text);color:var(--bg)}
.line{display:flex;flex-wrap:wrap;align-items:center;gap:10px;margin-top:12px}.line label{font-size:13px;color:var(--t2)}
.grow{flex:1;min-width:140px}.msg{font-size:13px;color:var(--t2);min-height:18px;margin-top:10px}
.head2{display:flex;align-items:center;gap:8px;margin-bottom:12px}.head2 h2{margin:0}.head2 .sp{margin-left:auto;display:flex;gap:8px}
.tiles,.store{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:10px;margin-top:14px}
.tile{display:flex;flex-direction:column;gap:6px;padding:14px 16px;border-radius:20px;background:rgba(var(--ink),.045);border:1px solid var(--line);min-width:0}
.tile[data-on]{border-color:color-mix(in srgb,var(--c) 55%,var(--line));background:color-mix(in srgb,var(--c) 12%,rgba(var(--ink),.04))}
.tile .t{display:flex;align-items:center;gap:8px;font-weight:600;min-width:0}.tile .t span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tile .d{font-size:12.5px;color:var(--t2);display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}
.tile .acts{display:flex;gap:6px;margin-top:auto;padding-top:4px}.tile button{height:30px;font-size:12.5px}
.tile .body{display:flex;flex-direction:column;gap:6px;min-width:0}
/* A lone add-on gets the whole row: in a grid cell of its own it left most of the section empty. Text on the left, buttons on the right. */
.tiles .tile.wide{grid-column:1/-1;flex-direction:row;align-items:center;gap:18px;padding:16px 18px}
.tile.wide .body{flex:1}.tile.wide .d{-webkit-line-clamp:3}.tile.wide .acts{margin:0;padding:0;flex:none}
.dot{flex:none;width:8px;height:8px;border-radius:50%;background:#64748b}.dot[data-live]{background:#22c55e;box-shadow:0 0 0 3px rgba(34,197,94,.18)}.dot[data-bad]{background:#f59e0b}
.tag{margin-left:auto;flex:none;padding:2px 8px;border-radius:999px;font-size:11px;font-weight:600;color:var(--t2);background:rgba(var(--ink),.07)}
.empty{grid-column:1/-1;padding:16px;border-radius:18px;border:1px dashed var(--line);color:var(--t3);font-size:13px}
.sig{display:inline-flex;align-items:flex-end;gap:2px;height:13px;margin-left:8px;vertical-align:-1px}.sig i{width:4px;border-radius:1px;background:rgba(var(--ink),.14)}.sig i:nth-child(1){height:4px}.sig i:nth-child(2){height:7px}.sig i:nth-child(3){height:10px}.sig i:nth-child(4){height:13px}
.sw{display:grid;grid-template-columns:repeat(25,1fr);gap:2px;height:10px;margin:2px 0}.sw i{border-radius:2px;background:var(--off)}
.panel{margin-top:14px;padding:14px;border-radius:22px;background:var(--panel);border:1px solid var(--line)}
details.byo{margin-top:14px;border-top:1px solid var(--line);padding-top:12px}details.byo summary{cursor:pointer;color:var(--t2);font-size:13px}
/* Sections that fold: the header is the whole click target and carries a one-line summary, so a closed section still says how things are. */
details.card>summary{list-style:none;display:flex;align-items:center;gap:12px;cursor:pointer;margin:-4px 0;padding:4px 0}
details.card>summary::-webkit-details-marker{display:none}
details.card>summary h2{margin:0;flex:none}
details.card>summary .sum{flex:1;min-width:0;font-size:13px;color:var(--t2);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
details.card>summary .chev{flex:none;width:28px;height:28px;border-radius:50%;display:grid;place-items:center;background:rgba(var(--ink),.07);transition:transform .2s}
details.card>summary .chev::before{content:'';width:7px;height:7px;border-right:2px solid var(--t2);border-bottom:2px solid var(--t2);transform:translateY(-2px) rotate(45deg)}
details.card[open]>summary .chev{transform:rotate(180deg)}
details.card>summary:hover .chev{background:rgba(var(--ink),.14)}
details.card[open]>summary{margin-bottom:12px}
.upd{display:flex;align-items:center;gap:14px;flex-wrap:wrap;padding:14px 16px;border-radius:20px;background:rgba(var(--ink),.045);border:1px solid var(--line)}
.upd b{font-size:15px}
.prog{height:6px;border-radius:99px;background:rgba(var(--ink),.08);overflow:hidden;margin-top:10px}.prog i{display:block;height:100%;width:0;background:var(--c);transition:width .4s}
.thm{flex:none;height:26px;padding:0 10px;font-size:12px;color:var(--t2)}
.tog{display:inline-flex;align-items:center;gap:8px;font-size:13px;color:var(--t2)}
[hidden]{display:none!important}
/* On a phone the board name, address and signal no longer fit beside the title and the theme switch: they take their own line underneath. */
@media(max-width:560px){.tiles .tile.wide{flex-direction:column;align-items:stretch;gap:8px}}
@media(max-width:560px){.head{flex-wrap:wrap;row-gap:6px}.head b{white-space:nowrap}.thm{margin-left:auto}.head .more{order:3;flex-basis:100%;margin-left:0}}
@media(max-width:560px){.row{grid-template-columns:76px 40px 40px 1fr auto}.row .spd{display:none}.big{font-size:40px}}
</style>
<script>/* Set before the body draws, so a pinned theme never flashes the other one first. */try{const t=localStorage.getItem('theme');if(t==='light'||t==='dark')document.documentElement.dataset.theme=t;}catch(e){}</script>
</head><body><main>

<section class="card" id="now">
  <div class="head"><b>Panda State</b><span class="chip" id="fw">…</span><span class="more"><span id="net"></span><span class="sig" id="sig" title="Wi-Fi signal"><i></i><i></i><i></i><i></i></span></span><button class="thm" id="thm"></button></div>
  <div class="big" id="state">…</div>
  <div class="sub" id="said"></div>
  <div class="bar" id="bar"></div>
  <div class="line"><label for="bri">Brightness</label><input class="grow" id="bri" type="range" min="0" max="100"><span class="sub" id="briv"></span></div>
  <div class="line" id="moderow"><label>Mode</label><button id="mstatus">Follow the printer</button><button id="mmusic">Music</button>
    <label for="sens">Music sensitivity</label><select id="sens" style="width:110px"><option>low</option><option>medium</option><option>high</option><option>auto</option></select>
    <span class="sub" id="mic"></span></div>
  <div class="sub" id="btnhelp" style="margin-top:10px">Button on the bar: press once for the next Music sensitivity, twice to switch mode, hold five seconds for the setup Wi-Fi network.</div>
</section>

<details class="card" id="fold-looks">
  <summary><h2>What each state looks like</h2><span class="sum" id="lookssum"></span><i class="chev"></i></summary>
  <div id="rows"></div>
  <div class="line">
    <label for="order">Colour order</label><select id="order" style="width:90px"></select>
    <label><input type="checkbox" id="rev"> Run the other way along the bar</label>
  </div>
  <div class="line" id="striprow" hidden><label for="striplen">LEDs on the strip</label><input id="striplen" type="text" inputmode="numeric" style="width:70px"><button id="stripsave">Save</button><span class="sub">The 25-LED picture is stretched over however many there are.</span></div>
  <div class="line" id="splitrow" hidden><label class="tog"><input type="checkbox" id="split"> Split the bar: channel 1 shows its first half, channel 2 its second (otherwise both show all of it)</label></div>
  <div class="msg" id="cfgmsg"></div>
</details>

<details class="card" id="fold-addons" open>
  <summary><h2>Add-ons</h2><span class="sum" id="adsum"></span><i class="chev"></i></summary>
  <div class="sub">Show something else on the bar when the printer has nothing to say.</div>
  <div class="tiles" id="adtiles"></div>
  <div class="line"><label for="adwhen">Show it</label><select id="adwhen" style="width:auto;max-width:100%"><option value="idle">while the printer is not busy with a print</option><option value="always">always, even while printing</option></select><button id="adnone" hidden>Show nothing</button></div>
  <div class="msg" id="admsg"></div>
  <div class="panel" id="storepanel">
    <div class="head2" style="margin:0"><h2>Store</h2><span class="sp"><span class="sub" id="storenote" style="align-self:center"></span><button id="adup">Upload a file</button><input id="adfile" type="file" accept=".json,application/json" hidden></span></div>
    <div class="store" id="storelist"></div>
  </div>
  <details class="byo"><summary>Build your own</summary>
    <div class="sub" style="margin-top:10px">An add-on <b>file</b> is a few lines of JSON the bar runs itself: a look, meters, an animation, or a live meter read from a web address. Upload one here, or add yours to the store's folder on GitHub (<a href="https://github.com/ImSammyTTV/Panda-State/tree/main/addons/store" target="_blank" rel="noopener">format and examples</a>).</div>
    <div class="sub" style="margin-top:10px">An add-on <b>program</b> runs anywhere on your network and posts to <code>/api/addon</code>, again and again. Every call is listed at <a href="/api">/api</a>. A meter at 63%:<br><code>curl -X POST http://<span id="myip"></span>/api/addon -H "content-type: application/json" -d '{"id":"demo","name":"Demo","meters":[{"value":63,"colour":"#D97757"}]}'</code></div>
  </details>
</details>

<details class="card" id="fold-sched">
  <summary><h2>Schedule</h2><span class="sum" id="schedsum"></span><i class="chev"></i></summary>
  <div class="line"><button id="schon"></button><label for="schfrom">Dark from</label><input id="schfrom" type="time" style="width:120px"><label for="schto">back on at</label><input id="schto" type="time" style="width:120px"><label for="schlvl">at brightness</label><input id="schlvl" type="text" inputmode="numeric" style="width:56px"><label>%</label><button id="schsave">Save</button></div>
  <div class="line"><label for="zone">Time zone</label><select id="zone" style="width:230px"></select><span class="sub" id="clock"></span></div>
  <div class="msg" id="schmsg"></div>
</details>

<details class="card" id="fold-printer">
  <summary><h2>Printer</h2><span class="sum" id="prsum"></span><i class="chev"></i></summary>
  <div class="sub" id="prstate"></div>
  <div class="line"><label for="prtype">Printer</label><select id="prtype" style="width:220px"><option value="bambu">Bambu Lab</option><option value="klipper">Klipper (Moonraker)</option></select></div>
  <div class="line" id="klrow" hidden><input class="grow" id="klhost" type="text" placeholder="Klipper address, e.g. 192.168.1.60 or mainsail.local" autocomplete="off"><input id="klport" type="text" inputmode="numeric" placeholder="7125" style="width:80px"><input class="grow" id="klkey" type="password" placeholder="Moonraker API key, if it asks for one" autocomplete="off"></div>
  <div class="line" id="bbrow"><input class="grow" id="prip" type="text" placeholder="Printer address, e.g. 192.168.1.50" autocomplete="off"><input class="grow" id="prsn" type="text" placeholder="Serial number" autocomplete="off"><input class="grow" id="prcode" type="password" placeholder="Access code" autocomplete="off"></div>
  <div class="line"><button class="go" id="pair">Pair</button><button id="unpair">Unpair</button><span class="sub" id="prhelp">On the printer: Settings, then LAN Only or Network, shows the address and the access code.</span></div>
  <div class="line"><label for="cmin">After a print, cooling for</label><input id="cmin" type="text" inputmode="numeric" style="width:64px"><label for="cbed">minutes and until the bed is at</label><input id="cbed" type="text" inputmode="numeric" style="width:64px"><label>&deg;C</label><button id="coolsave">Save</button></div>
  <div class="msg" id="prmsg"></div>
</details>

<details class="card" id="fold-ha">
  <summary><h2>Home Assistant</h2><span class="sum" id="hasum"></span><i class="chev"></i></summary>
  <div class="upd"><div class="grow"><b id="hatitle">…</b><div class="sub" id="hasub"></div></div><button id="haon"></button></div>
  <div class="line"><input class="grow" id="hahost" type="text" placeholder="MQTT broker address, e.g. 192.168.1.10" autocomplete="off"><input id="haport" type="text" inputmode="numeric" placeholder="1883" style="width:80px"></div>
  <div class="line"><input class="grow" id="hauser" type="text" placeholder="User name (if the broker has one)" autocomplete="off"><input class="grow" id="hapass" type="password" placeholder="Password (leave empty to keep)" autocomplete="new-password"><input id="haprefix" type="text" placeholder="homeassistant" style="width:150px" title="Discovery prefix"><button class="go" id="hasave">Save</button></div>
  <div class="sub" style="margin-top:10px">With MQTT discovery the bar appears in Home Assistant by itself: a light (on, off, brightness, colour, effect; pick &ldquo;Follow the printer&rdquo; to hand it back), what it is showing, the print&rsquo;s progress and its Wi-Fi signal. An automation can also send <code>{"state":"printing","progress":42}</code> to <code id="hatopic"></code><code>/state/set</code>.</div>
  <div class="msg" id="hamsg"></div>
</details>

<details class="card" id="fold-wifi">
  <summary><h2>Wi-Fi</h2><span class="sum" id="wifisum"></span><i class="chev"></i></summary>
  <div class="upd"><div class="grow"><b id="wifititle">…</b><div class="sub" id="wifisub"></div></div><button id="scan">Scan</button></div>
  <div class="line"><input class="grow" id="ssid" type="text" list="nets" placeholder="Network name" autocomplete="off"><datalist id="nets"></datalist><input class="grow" id="pass" type="password" placeholder="Password" autocomplete="off"><button class="go" id="wifi">Save and join</button></div>
  <div class="msg" id="wifimsg"></div>
  <h2 style="margin-top:18px">The bar's own hotspot</h2>
  <div class="upd"><div class="grow"><b id="aptitle">…</b><div class="sub" id="apsub"></div></div></div>
  <div class="line"><label for="apmode">Hotspot</label><select id="apmode" style="width:300px"><option value="fallback">On only when Wi-Fi does not connect</option><option value="always">Always on</option></select></div>
  <div class="line"><input class="grow" id="apssid" type="text" placeholder="Hotspot name" autocomplete="off"><input class="grow" id="appass" type="password" placeholder="New password (8+ characters), or leave empty" autocomplete="new-password"><button id="apsave">Save</button></div>
  <div class="msg" id="apmsg"></div>
</details>

<details class="card" id="fold-fw">
  <summary><h2>Firmware</h2><span class="sum" id="fwsum"></span><i class="chev"></i></summary>
  <div class="upd">
    <div class="grow"><b id="updtitle">…</b><div class="sub" id="updsub"></div></div>
    <button id="updcheck">Check for updates</button><button class="go" id="updinstall" hidden>Install</button>
  </div>
  <div class="prog" id="updprog" hidden><i></i></div>
  <div class="line"><label class="tog"><input type="checkbox" id="updauto"> Look for a new release once a day (it is never installed without you)</label></div>
  <div class="sub" id="slots" style="margin-top:12px"></div>
  <div class="line"><input class="grow" id="bin" type="file" accept=".bin"><button id="up">Install file</button></div>
  <div class="line"><button id="other">Start the firmware in the other slot</button><button id="backup" hidden title="A copy of the stock firmware from the other slot. Keep it: an update later writes over that slot, and Install file puts this back.">Download the stock firmware</button><button id="reboot">Restart</button><button id="factory">Reset settings</button></div>
  <div class="msg" id="fwmsg"></div>
</details>

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
  $('prtype').value=s.printer.type||'bambu'; showPrType();
  if(s.printer.type==='klipper'){$('klhost').value=s.printer.ip||'';$('klport').value=s.printer.port||7125;}else{$('prip').value=s.printer.ip||''; $('prsn').value=s.printer.serial||'';}
  const h=s.homeAssistant; haOn=h.on; paintHa(); $('hahost').value=h.host; $('haport').value=h.port; $('hauser').value=h.user; $('haprefix').value=h.prefix; $('hatopic').textContent=h.topic;

  const p2=(s.board||'p2')==='p2'; $('moderow').hidden=!p2; $('btnhelp').textContent=p2?$('btnhelp').textContent:'Button on the board: hold five seconds for the setup Wi-Fi network.';
  if(s.board==='status'||s.board==='lux'){$('striprow').hidden=false;$('striplen').value=s.config.stripLength;}
  if(s.board==='rgb'){$('splitrow').hidden=false;$('split').checked=s.config.split;} $('cmin').value=s.config.cool.minutes; $('cbed').value=s.config.cool.target;
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
const STORE='https://raw.githubusercontent.com/ImSammyTTV/Panda-State/main/addons/store/';
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
  /* Picked but nothing is running it: replaced by another add-on file, or a program that stopped. Said plainly, with a way out. */
  if(want&&!tiles.some(t=>t.id===want))tiles.push({id:want,name:want,desc:'Picked, but nothing is running it now: it was replaced, or the program that posted it has stopped. Use it again from the store, or clear it.',tag:'not running',bad:true,missing:true});
  const html=tiles.length?tiles.map(t=>'<div class="tile'+(tiles.length===1?' wide':'')+'"'+(t.id&&t.id===want?' data-on':'')+'><div class="body"><div class="t"><i class="dot"'+(t.live?' data-live':'')+(t.bad?' data-bad':'')+'></i><span>'+esc(t.name)+'</span><em class="tag">'+t.tag+'</em></div>'
    +(t.desc?'<div class="d">'+esc(t.desc)+'</div>':'')+'<div class="sub">'+esc(t.sub||'')+(s.addonShowing&&s.addonShowing===t.id?' · on the bar now':'')+'</div></div>'
    +'<div class="acts">'+(t.missing?'<button data-pick="">Clear</button>':t.id?(t.id===want?'<button class="go" disabled>Picked</button>':'<button data-pick="'+esc(t.id)+'">Show this</button>'):'')+(t.file?'<button data-remove="1">Remove</button>':'')+'</div></div>').join('')
    :'<div class="empty">Nothing yet. Pick one from the store below, or upload a file of your own.</div>';
  if(html!==window.adHtml){window.adHtml=html;$('adtiles').innerHTML=html;}
  $('adnone').hidden=!want;
  if(window.storeIdx&&s.addonFile.url!==window.storeFor){last=s;drawStore();}
  const on=tiles.find(t=>t.id&&t.id===want);
  $('adsum').textContent=on?on.name+(s.addonShowing===on.id?' · on the bar now':''):'None picked';
}
$('adtiles').onclick=async(e)=>{const b=e.target.closest('button');if(!b)return;if(b.dataset.pick!==undefined)pick(b.dataset.pick);
  if(b.dataset.remove){if(!confirm('Remove this add-on from the bar?'))return;const r=await post('/api/addonfile',{remove:true});$('admsg').textContent=r.message||'';}};
$('adnone').onclick=()=>pick('');
$('backup').onclick=()=>{location.href='/api/otherslot.bin';};
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
  window.storeIdx=idx; loadSims(idx); drawStore();
}
/* Drawn again whenever the bar's add-on changes, so the card it came from says In use even when the store was read before the bar first answered. */
function drawStore(){
  const idx=window.storeIdx;if(!idx)return;
  window.storeFor=last?last.addonFile.url:null;
  $('storelist').innerHTML=idx.addons.map(a=>{const url=STORE+a.file,using=last&&last.addonFile.url===url,cols=(a.preview||'').match(/[0-9a-fA-F]{6}/g)||[];
    return '<div class="tile"'+(using?' data-on':'')+'><div class="t"><span>'+esc(a.name)+'</span><em class="tag">'+esc(a.kind)+'</em></div>'
      +(cols.length===25?'<div class="sw" data-f="'+esc(a.file)+'">'+cols.map(c=>'<i style="background:#'+c+'"></i>').join('')+'</div>':'')
      +'<div class="d">'+esc(a.description)+'</div><div class="sub">'+(a.author?'by '+esc(a.author):'')+'</div>'
      +'<div class="acts">'+(using?'<button class="go" disabled>In use</button>':'<button data-url="'+esc(url)+'"'+(a.needs?' data-needs="'+esc(a.needs)+'"':'')+'>'+(a.needs==='place'?'Use for my town':'Use this')+'</button>')+'</div></div>';}).join('');
}
/* Store cards that move the way the bar would. Looks run through a copy of the bar's own effects (the same sums as render() and paintEffect() in the firmware), animations play their frames at their own speed, and live meters fill up to the level in their picture. */
const SIMS={};
const hex3=(c)=>{c=(c||'#000000').replace('#','');return [parseInt(c.substr(0,2),16)||0,parseInt(c.substr(2,2),16)||0,parseInt(c.substr(4,2),16)||0];};
const hexRow=(h)=>((h||'').match(/[0-9a-fA-F]{6}/g)||[]).map(hex3);
function partner(c){const [r,g,b]=c.map(x=>x/255),hi=Math.max(r,g,b),lo=Math.min(r,g,b),sp=hi-lo;if(sp<0.08)return [0x40,0x60,0xff];
  let h=hi===r?((g-b)/sp)%6:hi===g?(b-r)/sp+2:(r-g)/sp+4;h=((h/6+1+1/6)%1)*6;
  const s=sp/hi,v=hi,f=h-Math.floor(h),p=v*(1-s),q=v*(1-s*f),u=v*(1-s*(1-f));
  return [[v,u,p],[q,v,p],[p,v,u],[p,q,v],[u,p,v],[v,p,q]][Math.floor(h)%6].map(x=>Math.floor(x*255));}
function hue(h){const x=h*6,f=x-Math.floor(x),q=1-f;return [[1,f,0],[q,1,0],[0,1,f],[0,q,1],[f,0,1],[1,0,q]][Math.floor(x)%6].map(v=>v*255);}
function lookSim(look){
  const N=25,c1=hex3(look.colour),c2=look.colour2&&look.colour2!=='auto'?hex3(look.colour2):partner(c1),fx=look.effect||'solid',speed=look.speed||50;
  const pal=[c1,c2],mix=(a,b,k)=>[0,1,2].map(i=>a[i]+(b[i]-a[i])*k),along=(x)=>{const f=(((x%1)+1)%1)*2,i=Math.floor(f);return mix(pal[i%2],pal[(i+1)%2],f-i);};
  const heat=new Array(N).fill(0),glow=new Array(N).fill(0),tint=[...Array(N)].map(()=>[0,0,0]);let drops=[];
  return (t,dt)=>{
    const rate=0.1+speed/100*1.9,s=0.5+speed/100*7.5;let lamp=null;
    if(fx==='fire'){const ticks=Math.max(1,Math.round(dt*30*Math.max(0.5,s/4)));
      for(let n=0;n<ticks;n++){for(let i=0;i<N;i++)heat[i]=Math.max(0,heat[i]-Math.random()*((55*10/N+2)/255));for(let i=N-1;i>=2;i--)heat[i]=(heat[i-1]+heat[i-2]+heat[i-2])/3;if(Math.random()<0.65){const a=Math.floor(Math.random()*4);heat[a]=Math.min(1,heat[a]+0.6+Math.random()*0.4);}}
      lamp=heat.map(h=>{const x=Math.min(1,Math.max(0,h))*3;return [[x<1?x*255:255,x<1?0:x<2?(x-1)*255:255,x<2?0:(x-2)*255],1];});}
    else if(fx==='twinkle'){lamp=[];for(let i=0;i<N;i++){glow[i]=Math.max(0,glow[i]-dt*0.9*s/4);if(Math.random()<dt*s*0.5){glow[i]=1;tint[i]=along(Math.random());}lamp.push([tint[i],glow[i]]);}}
    else if(fx==='sparkle'){lamp=[];for(let i=0;i<N;i++){glow[i]=Math.max(0,glow[i]-dt*6);if(Math.random()<dt*s*0.8)glow[i]=1;lamp.push([mix(c1,[255,255,255],glow[i]),0.55+0.45*glow[i]]);}}
    else if(fx==='rain'){drops=drops.map(d=>d-dt*s*5).filter(d=>d>=-4);if(drops.length<8&&Math.random()<dt*s*0.9)drops.push(N-1);
      const lv=new Array(N).fill(0);for(const d of drops)for(let j=0;j<4;j++){const i=Math.round(d+j);if(i>=0&&i<N)lv[i]=Math.max(lv[i],1-j/4);}lamp=lv.map(l=>[c1,l]);}
    const out=[];
    for(let i=0;i<N;i++){let col=c1,v=1;
      switch(fx){
        case 'breathe':v=0.10+0.90*(0.5-0.5*Math.cos(2*Math.PI*t*rate*0.5));break;
        case 'flash':v=((t*rate*2)%1)<0.5?1:0.15;break;
        case 'chase':{const head=(t*rate*N*0.5)%N,behind=(head-i+N)%N;v=Math.max(0.2,behind<6?1-behind/6:0.2);break;}
        case 'rainbow':col=hue((i/N+t*rate*0.2)%1);break;
        case 'gradient':col=mix(c1,c2,i/(N-1));break;
        case 'waves':col=along(i/N+t*0.05*s+0.12*Math.sin(t*0.6*s+i/3));v=0.55+0.45*Math.sin(i/2.5-t*0.9*s);break;
        case 'scroll':col=along(i/N+t*0.07*s);break;
        case 'theatre':{const on=(i+Math.floor(t*s*3))%3===0;col=on?c1:c2;v=on?1:0.25;break;}
        case 'scanner':{const pos=(N-1)*(0.5-0.5*Math.cos(t*s*1.5)),m=Math.min(1,Math.max(0,1-Math.abs(i-pos)/3));col=mix(c2,c1,m);v=0.25+0.75*m;break;}
        case 'meteor':{const pos=(t*s*2.6)%(N+14),b=pos-i,g=b<0?Math.exp(-(b*b)/2.2):Math.exp(-b/4.5);col=along(1-i/N*0.6);v=Math.sqrt(Math.min(1,Math.max(0,g)));break;}
        case 'aurora':{const w=Math.min(1,Math.max(0,0.5+0.5*Math.sin(i*0.35+t*s*0.35)*Math.sin(i*0.17-t*s*0.25+1.7)+0.15));col=along(0.5*w+i/N*0.3+t*0.02*s);v=0.5+0.5*w;break;}
        default:if(lamp){col=lamp[i][0];v=lamp[i][1];}
      }
      const k=v*v;out.push([col[0]*k,col[1]*k,col[2]*k]);}
    return out;};
}
function storeSim(file,entry){
  if(file.look)return lookSim(file.look);
  /* The filament add-on shows whatever is loaded, so the card steps through a few common filament colours. */
  if(file.filament){const cols=['F72323','FEC600','00AE42','0086D6','FFFFFF'].map(hex3);return (t)=>Array(25).fill(cols[Math.floor(t/1.6)%cols.length]);}
  if(file.frames&&file.frames.list&&file.frames.list.length){const rows=file.frames.list.map(hexRow),fps=file.frames.fps||4;return (t)=>rows[Math.floor(t*fps)%rows.length];}
  const pic=hexRow(entry.preview);if(pic.length!==25)return null;
  if(!file.source)return ()=>pic;
  /* A live meter: rise to the level in the picture, hold, start again. Lit is anything brighter than the picture's unlit grey. */
  const lit=pic.filter(c=>c[0]+c[1]+c[2]>90).length,dark=[17,20,24];
  return (t)=>{const p=Math.min(1,(t%4)/1.4),n=lit*(1-Math.pow(1-p,3));return pic.map((c,i)=>i<Math.floor(n)?c:i<n?c.map((x,j)=>dark[j]+(x-dark[j])*(n-i)):dark);};
}
function loadSims(idx){for(const a of idx.addons){if(SIMS[a.file]!==undefined)continue;SIMS[a.file]=null;
  fetch(STORE+a.file,{cache:'no-store'}).then(r=>r.json()).then(j=>{SIMS[a.file]=storeSim(j,a);}).catch(()=>{});}}
let simLast=0;
function simTick(now){
  requestAnimationFrame(simTick);
  if(document.hidden||!$('fold-addons').open||now-simLast<33)return;
  const dt=simLast?Math.min(0.1,(now-simLast)/1000):0.033;simLast=now;
  document.querySelectorAll('#storelist .sw[data-f]').forEach(sw=>{const sim=SIMS[sw.dataset.f];if(!sim)return;const px=sim(now/1000,dt);if(!px)return;
    [...sw.children].forEach((el,i)=>{const c=px[i];el.style.background='rgb('+Math.round(c[0])+','+Math.round(c[1])+','+Math.round(c[2])+')';});});
}
requestAnimationFrame(simTick);
openStore();
$('storelist').onclick=async(e)=>{const b=e.target.closest('button');if(!b||!b.dataset.url)return;
  /* An add-on for a place (weather, air): this page asks for the town, finds it with Open-Meteo's free place search, and hands the bar a copy with the place filled in. Only the coordinates go to the bar; nothing is looked up from the bar's own address. */
  if(b.dataset.needs==='place'){
    const town=prompt('Which town or suburb is the bar in?');if(!town)return;
    b.disabled=true;b.textContent='Finding it…';
    try{
      const g=await fetch('https://geocoding-api.open-meteo.com/v1/search?count=1&name='+encodeURIComponent(town.trim())).then(r=>r.json());
      const at=g.results&&g.results[0];if(!at)throw new Error('Could not find "'+town+'"');
      let text=await fetch(b.dataset.url,{cache:'no-store'}).then(r=>r.text());
      text=text.split('{lat}').join(at.latitude.toFixed(3)).split('{lon}').join(at.longitude.toFixed(3)).split('{place}').join(at.name.replace(/"/g,''));
      const r=await post('/api/addonfile',{file:JSON.parse(text)});
      $('admsg').textContent=r.ok?'Added for '+at.name+(at.admin1?', '+at.admin1:'')+(at.country?', '+at.country:'')+'.':(r.message||'Not accepted');
    }catch(err){$('admsg').textContent=err.message||'That did not work';}
    setTimeout(openStore,2000);return;
  }
  b.disabled=true;b.textContent='Adding…';
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
function showPrType(){const k=$('prtype').value==='klipper';$('klrow').hidden=!k;$('bbrow').hidden=k;$('prhelp').textContent=k?'The address of the machine running Moonraker (Mainsail or Fluidd use the same one). Port 7125 unless you changed it.':'On the printer: Settings, then LAN Only or Network, shows the address and the access code.';}
$('prtype').onchange=showPrType;
let haOn=false;
function paintHa(){$('haon').textContent=haOn?'On':'Off';$('haon').className=haOn?'go':'';}
async function saveHa(){busy=Date.now();const body={on:haOn,host:$('hahost').value.trim(),port:+($('haport').value||1883),user:$('hauser').value.trim(),prefix:$('haprefix').value.trim()||'homeassistant'};if($('hapass').value)body.password=$('hapass').value;
  const r=await post('/api/ha',body);$('hamsg').textContent=r.message||(r.ok?'Saved':'Not saved');if(r.ok)$('hapass').value='';else{haOn=last&&last.homeAssistant?last.homeAssistant.on:false;paintHa();}}
$('haon').onclick=()=>{haOn=!haOn;paintHa();saveHa();};
$('hasave').onclick=saveHa;
$('stripsave').onclick=async()=>{busy=Date.now();const r=await post('/api/config',{stripLength:+$('striplen').value});$('cfgmsg').textContent=r.ok?'Saved':(r.message||'Not saved');};
$('split').onchange=()=>{busy=Date.now();post('/api/config',{split:$('split').checked});};
$('pair').onclick=async()=>{const k=$('prtype').value==='klipper';const r=await post('/api/printer',k?{type:'klipper',host:$('klhost').value,port:+($('klport').value||7125),key:$('klkey').value}:{ip:$('prip').value,serial:$('prsn').value,code:$('prcode').value});if(r.ok)$('klkey').value='';$('prmsg').textContent=r.message||(r.ok?'Saved':'Not saved');if(r.ok)$('prcode').value='';};
$('unpair').onclick=async()=>{if(!confirm('Unpair the bar from the printer?'))return;const r=await post('/api/printer',{forget:true});$('prmsg').textContent=r.message||'';if(r.ok){$('prip').value='';$('prsn').value='';$('klhost').value='';}};
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
  $('fw').textContent='v'+s.version;
  /* The board's own name sits with the address: the heading is the firmware's name, the same on every board. */
  $('net').textContent=(s.boardName?s.boardName+' · ':'')+(s.setupNetwork?'setup network up · ':'')+s.ip+(s.rssi?' · '+s.rssi+' dBm':'');
  /* Four bars like a phone's, coloured by how usable the link is: -60 dBm and better is solid, below -80 the page and updates start to struggle. */
  const r=s.rssi||-200,bars=r>=-60?4:r>=-70?3:r>=-80?2:r>-200?1:0,col=bars===4?'#22c55e':bars===3?'#eab308':bars===2?'#f97316':'#ef4444';
  [...$('sig').children].forEach((el,i)=>{el.style.background=i<bars?col:'';}); $('sig').hidden=!s.rssi;
  $('state').textContent=s.state;
  const look=s.config.states[s.state]; document.documentElement.style.setProperty('--c',look?look.colour:'#64748b');
  const pct=(s.state==='printing'||s.state==='cooling'||s.state==='paused')?' · '+Math.round(s.progress)+'%':'';
  const music=s.mode==='music';
  $('mstatus').className=music?'':'go'; $('mmusic').className=music?'go':'';
  if(Date.now()-busy>3000) $('sens').value=s.config.music.sensitivity;
  $('mic').textContent=s.mic.blocked?'microphone left off: its last start on this build did not finish':!s.mic.running?'microphone starting':!s.mic.found?'the microphone chip did not answer':'microphone '+Math.round(s.mic.db)+' dB · meter '+Math.round(s.mic.level*100)+'%';
  if(music&&s.source!=='preview') $('state').textContent='music';
  $('said').textContent=music&&s.source!=='preview'?'Music mode: the bar follows the sound in the room'
    :s.lightOff?'Switched off from Home Assistant'
    :s.source==='homeassistant'?'Set from Home Assistant; pick &ldquo;Follow the printer&rdquo; there to hand it back'.replace(/&ldquo;|&rdquo;/g,'"')
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
  const pk=p.type==='klipper'?'Klipper at ':'';
  $('prstate').textContent=!p.paired?'Not paired.'
    :p.connected?'Linked to '+pk+p.ip+(p.gcode?' · printer says '+p.gcode+(p.percent!==undefined?' '+p.percent+'%':'')+(p.bed!==undefined?' · bed '+Math.round(p.bed)+'°':'')+(p.filament?' · '+(p.filament.loaded?'':'last used ')+(p.filament.type||'filament')+' '+p.filament.colour:''):' · waiting for its first message')
    :'Paired with '+p.ip+', not linked'+(p.error?': '+p.error:'')+'.';
  if(Date.now()-busy>3000){$('bri').value=s.config.brightness;} $('briv').textContent=$('bri').value+'%';
  const o=s.otherSlot;
  $('slots').textContent='Running from '+s.slot+' ('+s.build+')'+(s.onTrial?' · on trial until it has been on Wi-Fi for 20 s':'')+'. Other slot ('+o.label+'): '+(o.project?o.project+' '+o.version+', built '+o.built:'empty')+'.';
  $('other').disabled=!o.project; $('backup').hidden=!o.project||o.ours;
  /* What a closed section says about itself. */
  $('lookssum').textContent=['idle','printing','error'].map(n=>{const l=s.config.states[n];return n.charAt(0).toUpperCase()+n.slice(1)+' '+(l?l.effect:'');}).join(' · ')+' · and five more';
  const sc=s.config.schedule;
  $('schedsum').textContent=sc.enabled?'Dark '+sc.from+' to '+sc.to+(s.clock.dark?' · dark now':''):'Off';
  $('prsum').textContent=$('prstate').textContent;
  $('wifisum').textContent=$('wifititle').textContent+(s.ap.up?' · hotspot on':'');
  $('fwsum').textContent=$('updtitle').textContent;
  const h=s.homeAssistant;
  $('hatitle').textContent=!h.on?'Off':h.connected?'Connected to '+h.host:'Not connected'+(h.error?': '+h.error:'');
  $('hasub').textContent=h.on?(h.connected?'Discovery prefix '+h.prefix+' · device topics under '+h.topic+(h.manual?' · Home Assistant has the light':''):'Broker '+h.host+':'+h.port):'Turn on, give the broker address (and user and password if it has them), and the bar appears under MQTT devices.';
  $('hasum').textContent=$('hatitle').textContent;
  if(Date.now()-busy>3000&&haOn!==h.on){haOn=h.on;paintHa();}
}
/* Which sections are open, remembered by this browser. */
document.querySelectorAll('details.card').forEach(d=>{
  try{const was=localStorage.getItem(d.id);if(was!==null)d.open=was==='1';}catch(e){}
  d.addEventListener('toggle',()=>{try{localStorage.setItem(d.id,d.open?'1':'0');}catch(e){}});
});
/* The page theme: Auto follows the device, Light and Dark pin it for this browser only, since a phone and a desktop can want different things. */
const THEMES=['auto','light','dark'],THEME_NAMES={auto:'Auto',light:'Light',dark:'Dark'};
function paintTheme(){const t=document.documentElement.dataset.theme||'auto';$('thm').textContent=THEME_NAMES[t];$('thm').title='Page theme: '+(t==='auto'?'following this device':THEME_NAMES[t].toLowerCase())+'. Click to change.';}
$('thm').onclick=()=>{const now=document.documentElement.dataset.theme||'auto',next=THEMES[(THEMES.indexOf(now)+1)%3];
  if(next==='auto')delete document.documentElement.dataset.theme;else document.documentElement.dataset.theme=next;
  try{if(next==='auto')localStorage.removeItem('theme');else localStorage.setItem('theme',next);}catch(e){}paintTheme();};
paintTheme();
tick(); setInterval(tick,1000);
/* The live picture: its own small request five times a second, each LED fading to the next reading (CSS transition) so effects move smoothly instead of jumping once a second. One request at a time, so a slow bar is never queued up. */
let framing=false;
async function frameTick(){
  if(framing||document.hidden)return; framing=true;
  try{const hex=await fetch('/api/frame').then(r=>r.text());if(hex.length===150)[...bar.children].forEach((el,i)=>{el.style.backgroundColor='#'+hex.substr(i*6,6);});}catch(e){}
  framing=false;
}
setInterval(frameTick,200);
</script></body></html>)HTML";
