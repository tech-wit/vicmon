// Web UI assets (HTML / CSS / JS) lifted out of main.cpp. Pure string literals,
// no logic — included once by the web layer. Kept as C++ raw-strings for now; a
// later step may serve these from LittleFS instead.
#pragma once

static const char kStyle[] = R"CSS(
:root{--bg:#0f1720;--card:#172230;--fg:#e6edf3;--muted:#7d8da1;--line:#243140;
--accent:#22d3ee;--green:#34d399;--red:#f87171;--amber:#fbbf24}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
header{display:flex;gap:1em;align-items:center;padding:.7em 1em;background:#0b1118;
border-bottom:1px solid var(--line);position:sticky;top:0}
header h1{font-size:1em;margin:0;color:var(--accent);letter-spacing:.12em}
nav a{color:var(--muted);text-decoration:none;margin-right:1em;font-size:.95em}
nav a.active,nav a:hover{color:var(--fg)}
main{padding:1em;max-width:760px;margin:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1em;margin-bottom:1em}
h3{margin:.2em 0 .8em}
table{width:100%;border-collapse:collapse}
td,th{padding:.5em;border-bottom:1px solid var(--line);text-align:left;font-size:.92em}
button{background:var(--accent);color:#06121a;border:0;border-radius:8px;padding:.45em .8em;font-weight:600;cursor:pointer}
button.danger{background:var(--red);color:#1a0606}
button.ghost{background:transparent;color:var(--muted);border:1px solid #2c3a4a}
.winbtn.active{background:var(--accent);color:#06121a;border-color:var(--accent)}
input,select{background:#0d1620;color:var(--fg);border:1px solid #2c3a4a;border-radius:8px;padding:.45em;font-size:.92em}
label{font-size:.8em;color:var(--muted);display:block;margin-bottom:.2em}
form.inline{display:flex;gap:.6em;flex-wrap:wrap;align-items:end;margin:0}
.banner{text-align:center;font-weight:700;letter-spacing:.18em;padding:.5em;
border:1px solid #2c3a4a;border-radius:10px;margin-top:.6em;color:var(--muted)}
.legend{font-size:.8em;display:flex;gap:1em;flex-wrap:wrap;margin-top:.5em}
line,polyline{stroke-width:4;stroke-linecap:round;stroke-linejoin:round;fill:none}
.flow{stroke-dasharray:7 7;animation:dash 1s linear infinite}
.flowrev{stroke-dasharray:7 7;animation:dashrev 1s linear infinite}
@keyframes dash{to{stroke-dashoffset:-14}}
@keyframes dashrev{to{stroke-dashoffset:14}}
svg text{fill:#e6edf3;font-family:system-ui,sans-serif}
svg text.muted{fill:var(--muted)}
.muted{color:var(--muted)}
)CSS";

static const char kMimicPage[] = R"HTML(
<div id="alerts"></div>
<div class=card>
<svg viewBox="0 0 360 350" id="mimic" style="width:100%;max-width:460px;display:block;margin:auto">
  <polyline id="lineSolar"   points="62,86 62,152 150,152" stroke="#2c3a4a" />
  <line     id="lineCharger" x1="180" y1="78" x2="180" y2="150" stroke="#2c3a4a" />
  <polyline id="lineDcdc"    points="298,86 298,152 210,152" stroke="#2c3a4a" />
  <line     id="lineLoad"    x1="180" y1="244" x2="180" y2="298" stroke="#2c3a4a" />
  <rect x="150" y="150" width="60" height="92" rx="9" fill="#0d1620" stroke="#2c3a4a" stroke-width="3" />
  <rect id="fill" x="153" y="242" width="54" height="0" fill="#34d399" opacity="0.85" />
  <rect id="batt" x="150" y="150" width="60" height="92" rx="9" fill="none" stroke="#7d8da1" stroke-width="3" />
  <rect x="167" y="145" width="26" height="7" rx="2" fill="#7d8da1" />
  <text id="soc" x="180" y="202" text-anchor="middle" font-size="20" font-weight="700">--</text>
  <text x="50" y="40" text-anchor="middle" font-size="22">&#9728;&#65039;</text>
  <text x="50" y="57" text-anchor="middle" font-size="10" class="muted">Solar</text>
  <text id="solarTxt" x="50" y="76" text-anchor="middle" font-size="13">--</text>
  <text id="solarSub" x="50" y="90" text-anchor="middle" font-size="10" class="muted"></text>
  <text x="180" y="32" text-anchor="middle" font-size="22">&#128268;</text>
  <text x="180" y="49" text-anchor="middle" font-size="10" class="muted">Charger</text>
  <text id="chargerTxt" x="180" y="68" text-anchor="middle" font-size="13">--</text>
  <text x="310" y="40" text-anchor="middle" font-size="22">&#9889;</text>
  <text x="310" y="57" text-anchor="middle" font-size="10" class="muted">DC-DC</text>
  <text id="dcdcTxt" x="310" y="76" text-anchor="middle" font-size="13">--</text>
  <text id="dcdcSub" x="310" y="90" text-anchor="middle" font-size="10" class="muted"></text>
  <text x="152" y="318" text-anchor="middle" font-size="22">&#128161;</text>
  <text id="loadTxt" x="172" y="314" text-anchor="start" font-size="13">Load --</text>
  <text id="dV" x="222" y="170" text-anchor="start" font-size="13">--</text>
  <text id="dA" x="222" y="188" text-anchor="start" font-size="13">--</text>
  <text id="dAh" x="222" y="206" text-anchor="start" font-size="13" class="muted">--</text>
  <text id="dStarter" x="222" y="224" text-anchor="start" font-size="13" class="muted">--</text>
  <text id="dTTG" x="222" y="242" text-anchor="start" font-size="13" class="muted">--</text>
</svg>
<div id="modeBanner" class="banner">--</div>
</div>
<div class=card>
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:.5em">
    <h3 style="margin:0">Trend</h3>
    <div id="winbtns">
      <button class="winbtn ghost" data-m="1">1m</button>
      <button class="winbtn ghost" data-m="10">10m</button>
      <button class="winbtn ghost" data-m="60">1h</button>
      <button class="winbtn ghost" data-m="720">12h</button>
      <button class="winbtn ghost" data-m="1440">24h</button>
    </div>
  </div>
  <canvas id="chart" width="700" height="160" style="width:100%;height:160px;margin-top:.5em"></canvas>
  <div id="legend" class="legend"></div>
</div>
<script>
function set(id,t){document.getElementById(id).textContent=t;}
function setNode(id,valid,a){set(id,valid?a.toFixed(1)+'A':'--');}
function setLine(id,mode,col){var e=document.getElementById(id);e.classList.remove('flow','flowrev');
 if(!mode){e.setAttribute('stroke','#2c3a4a');return;}e.setAttribute('stroke',col);
 e.classList.add(mode==1?'flow':'flowrev');}
function ttgStr(m){
 if(m>=1440){var d=Math.floor(m/1440),h=Math.round((m%1440)/60);return d+'d'+(h?' '+h+'h':'');}
 if(m>=60){var hh=Math.floor(m/60),mm=Math.round(m%60);return hh+'h'+(mm?' '+mm+'m':'');}
 return Math.round(m)+'m';}
async function tick(){
 let p; try{p=await(await fetch('/api/panel')).json();}catch(e){return;}
 var av=document.getElementById('alerts');
 if(p.alerts&&p.alerts.length){av.innerHTML=p.alerts.map(function(a){
  var c=a.sev=='crit'?'#f87171':'#fbbf24';
  return '<div class=card style="border-color:'+c+';color:'+c+';padding:.6em 1em;margin-bottom:.6em;font-weight:600">&#9888; '+a.msg+'</div>';}).join('');}
 else av.innerHTML='';
 var b=p.battery,soc=b.valid?b.soc:0;
 set('soc',b.valid?Math.round(soc)+'%':'--');
 set('dV',b.valid?b.v.toFixed(2)+' V':'--');
 set('dA',b.valid?(b.a>=0?'+':'')+b.a.toFixed(1)+' A':'--');
 if(b.capacity>0&&b.valid){var rem=b.capacity*(b.soc/100);
  set('dAh',rem.toFixed(0)+' / '+b.capacity.toFixed(0)+' Ah');}
 else if(b.consumed_valid){set('dAh',Math.abs(b.consumed).toFixed(1)+' Ah used');}
 else set('dAh','-- Ah');
 set('dStarter',b.starter_valid?'Starter '+b.starter_v.toFixed(2)+' V':'Starter --');
 // Estimate from instantaneous current so it settles in seconds, instead of the
 // BMV's heavily-filtered (multi-minute) time-to-go. Needs a known capacity.
 if(b.capacity>0&&b.valid&&Math.abs(b.a)>0.05){
  if(b.a>0){var mf=(b.capacity*(1-b.soc/100))/b.a*60;set('dTTG','Full '+ttgStr(mf));}
  else{var me=(b.capacity*(b.soc/100))/Math.abs(b.a)*60;set('dTTG','TTG '+ttgStr(me));}}
 else if(b.ttg_valid)set('dTTG','TTG '+ttgStr(b.ttg));
 else set('dTTG','TTG ∞');
 var h=Math.max(0,Math.min(1,soc/100))*88,f=document.getElementById('fill');
 f.setAttribute('y',242-h);f.setAttribute('height',h);
 var col=p.mode=='charging'?'#34d399':p.mode=='discharging'?'#f87171':'#7d8da1';
 document.getElementById('batt').setAttribute('stroke',col);f.setAttribute('fill',col);
 var mb=document.getElementById('modeBanner');mb.textContent=p.mode.toUpperCase();
 mb.style.color=col;mb.style.borderColor=col;
 setLine('lineSolar',(p.solar.valid&&p.solar.a>0.05)?1:0,'#34d399');
 setNode('solarTxt',p.solar.valid,p.solar.a);
 set('solarSub',p.solar.valid?(p.solar.w.toFixed(0)+'W'+(p.solar.v_valid?' · '+p.solar.v.toFixed(1)+'V':'')):'');
 setLine('lineCharger',(p.charger.valid&&p.charger.a>0.05)?1:0,'#34d399');
 setNode('chargerTxt',p.charger.valid,p.charger.a);
 setLine('lineDcdc',(p.dcdc.valid&&p.dcdc.out_a>0.05)?1:0,'#34d399');
 setNode('dcdcTxt',p.dcdc.valid,p.dcdc.out_a);
 set('dcdcSub',p.dcdc.in_v_valid?('in '+p.dcdc.in_v.toFixed(1)+'V'):'');
 // Load line is driven by the load signal itself: it flows DOWN to the load
 // (amber) whenever there is load, independent of battery charge/discharge.
 var ld=p.load.valid?p.load.a:0;
 setLine('lineLoad',(p.load.valid&&ld>0.05)?1:0,'#fbbf24');
 set('loadTxt','Load '+(p.load.valid?ld.toFixed(1)+'A':'--'));
}
var SERIES=[
 {k:'battery',label:'Battery',color:'#22d3ee'},
 {k:'solar',label:'Solar',color:'#facc15'},
 {k:'charger',label:'Charger',color:'#60a5fa'},
 {k:'dcdc',label:'DC-DC',color:'#a78bfa'},
 {k:'load',label:'Load',color:'#f87171'},
 {k:'soc',label:'SoC %',color:'#f1f5f9',right:true,dash:true}
];
var hidden={};
var chartWin=10,chartData=null;
function setWin(m){chartWin=m;
 var bs=document.querySelectorAll('.winbtn');for(var i=0;i<bs.length;i++)
  bs[i].classList.toggle('active',+bs[i].dataset.m===m);
 loadChart();}
async function loadChart(){
 try{chartData=await(await fetch('/api/history?mins='+chartWin)).json();}catch(e){return;}
 drawChart();}
function drawChart(){
 var c=document.getElementById('chart');if(!c||!c.getContext||!chartData)return;
 var ctx=c.getContext('2d'),W=c.width,H=c.height,padL=40,padR=32,padT=8,padB=18;
 ctx.clearRect(0,0,W,H);
 var s=chartData.series,N=0;
 SERIES.forEach(function(se){if(s[se.k]&&s[se.k].length>N)N=s[se.k].length;});
 var mn=0,mx=0;
 SERIES.forEach(function(se){if(se.right||hidden[se.k])return;(s[se.k]||[]).forEach(function(v){
  if(v!=null){if(v<mn)mn=v;if(v>mx)mx=v;}});});
 if(mx-mn<2){mx=mn+2;}
 // Right-align by real time over the full window, so 30/60m zoom out even
 // before the buffer has that much history (data sits at the right edge).
 var interval=chartData.interval||5;
 var totalSlots=Math.max(2,Math.round(chartData.mins*60/interval));
 function Y(v){return padT+(H-padT-padB)*(1-(v-mn)/(mx-mn));}
 function Yr(v){return padT+(H-padT-padB)*(1-v/100);}
 function X(i){var frac=1-((N-1-i)/(totalSlots-1));if(frac<0)frac=0;
  return padL+(W-padL-padR)*frac;}
 // left grid + A labels: 10 gradations across the auto-scaled current range
 var divs=10,step=(mx-mn)/divs,dec=step<1?1:0;
 ctx.fillStyle='#7d8da1';ctx.font='9px system-ui';ctx.textAlign='right';
 for(var gi=0;gi<=divs;gi++){var val=mn+step*gi,y=Y(val);
  ctx.strokeStyle='#1f2c3a';ctx.lineWidth=1;
  ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();
  ctx.fillText(val.toFixed(dec)+'A',padL-4,y+3);}
 if(mn<0&&mx>0){var y0=Y(0);ctx.strokeStyle='#3a4a5c';ctx.lineWidth=1;
  ctx.beginPath();ctx.moveTo(padL,y0);ctx.lineTo(W-padR,y0);ctx.stroke();}
 // right axis labels for the SoC overlay, matching at 10% gradations
 ctx.textAlign='left';ctx.fillStyle='#9aa7b5';
 for(var pi=0;pi<=10;pi++){ctx.fillText(pi*10+'%',W-padR+4,Yr(pi*10)+3);}
 // Vertical scale marks: 1/min @10m, 1/10min @30m & 1h, 1/3h @24h.
 var stepMin=chartWin<=10?1:(chartWin<=60?10:180);
 var plot=W-padL-padR;
 ctx.font='9px system-ui';
 for(var t=stepMin;t<chartWin-0.001;t+=stepMin){
  var xx=padL+plot*(1-t/chartWin);
  ctx.strokeStyle='#243140';ctx.lineWidth=1;
  ctx.beginPath();ctx.moveTo(xx,padT);ctx.lineTo(xx,H-padB);ctx.stroke();
  if(plot*(stepMin/chartWin)>=38){ctx.fillStyle='#5b6b7d';ctx.textAlign='center';
   ctx.fillText(t<60?t+'m':(t/60)+'h',xx,H-5);}
 }
 // X end labels (oldest .. now)
 ctx.font='10px system-ui';ctx.fillStyle='#7d8da1';
 ctx.textAlign='left';ctx.fillText('-'+(chartWin>=60?chartWin/60+'h':chartWin+'m'),padL,H-5);
 ctx.textAlign='right';ctx.fillText('now',W-padR,H-5);
 // series lines (SoC uses the right 0-100% axis + a dashed stroke)
 SERIES.forEach(function(se){if(hidden[se.k])return;var a=s[se.k]||[];var yf=se.right?Yr:Y;
  ctx.strokeStyle=se.color;ctx.lineWidth=2;ctx.setLineDash(se.dash?[5,3]:[]);
  ctx.beginPath();var started=false;
  for(var i=0;i<a.length;i++){var v=a[i];if(v==null){started=false;continue;}
   var x=X(i),y=yf(v);if(started)ctx.lineTo(x,y);else{ctx.moveTo(x,y);started=true;}}
  ctx.stroke();});
 ctx.setLineDash([]);
}
function renderLegend(){
 document.getElementById('legend').innerHTML=SERIES.map(function(se){var off=hidden[se.k];
  return '<span class=legitem data-k="'+se.k+'" style="cursor:pointer;user-select:none;color:'+
   (off?'#54606e':se.color)+';'+(off?'text-decoration:line-through':'')+'">&#9632; '+se.label+
   '</span>';}).join('');
 var it=document.querySelectorAll('.legitem');
 for(var i=0;i<it.length;i++)it[i].addEventListener('click',function(){
  hidden[this.dataset.k]=!hidden[this.dataset.k];renderLegend();drawChart();});
}
renderLegend();
var wb=document.querySelectorAll('.winbtn');
for(var i=0;i<wb.length;i++)wb[i].addEventListener('click',function(){setWin(+this.dataset.m);});
setWin(10);
setInterval(tick,1000);tick();
setInterval(loadChart,5000);
</script>
)HTML";

static const char kStatsPage[] = R"HTML(
<div class=card>
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:.5em">
    <div id="scopebtns">
      <button class="winbtn ghost" data-s="today">Today</button>
      <button class="winbtn ghost" data-s="trip">Trip</button>
      <button class="winbtn ghost" data-s="total">Total</button>
    </div>
    <form id="resetForm" method=post action=/stats/reset style="margin:0">
      <input type=hidden name=scope id=resetScope value=trip>
      <button class=ghost id=resetBtn>reset</button>
    </form>
  </div>
  <div id="since" class="muted" style="font-size:.82em;margin-top:.5em">--</div>
</div>
<div class=card>
  <h3>Energy in</h3>
  <table>
    <tr><th>Source</th><th style="text-align:right">Ah</th><th style="text-align:right">Wh</th></tr>
    <tr><td>&#9728;&#65039; Solar</td><td id="s_solar_ah" style="text-align:right">--</td><td id="s_solar_wh" style="text-align:right">--</td></tr>
    <tr><td>&#9889; DC-DC</td><td id="s_dcdc_ah" style="text-align:right">--</td><td id="s_dcdc_wh" style="text-align:right">--</td></tr>
    <tr><td>&#128268; Charger</td><td id="s_charger_ah" style="text-align:right">--</td><td id="s_charger_wh" style="text-align:right">--</td></tr>
    <tr><td><b>Into battery</b></td><td id="s_charged_ah" style="text-align:right"><b>--</b></td><td id="s_charged_wh" style="text-align:right"><b>--</b></td></tr>
  </table>
</div>
<div class=card>
  <h3>Energy out</h3>
  <table>
    <tr><th>&nbsp;</th><th style="text-align:right">Ah</th><th style="text-align:right">Wh</th></tr>
    <tr><td>&#128161; Load</td><td id="s_load_ah" style="text-align:right">--</td><td id="s_load_wh" style="text-align:right">--</td></tr>
    <tr><td><b>From battery</b></td><td id="s_discharged_ah" style="text-align:right"><b>--</b></td><td id="s_discharged_wh" style="text-align:right"><b>--</b></td></tr>
  </table>
  <div id="net" class="muted" style="font-size:.82em;margin-top:.5em">--</div>
</div>
<div class=card>
  <h3>Extremes</h3>
  <table>
    <tr><td>State of charge</td><td id="s_soc" style="text-align:right">--</td></tr>
    <tr><td>Voltage</td><td id="s_v" style="text-align:right">--</td></tr>
    <tr><td>Peak solar</td><td id="s_psolar" style="text-align:right">--</td></tr>
    <tr><td>Peak load</td><td id="s_pload" style="text-align:right">--</td></tr>
    <tr><td>Peak charge / discharge</td><td id="s_pcur" style="text-align:right">--</td></tr>
    <tr><td>Time charging / discharging</td><td id="s_time" style="text-align:right">--</td></tr>
  </table>
</div>
<div class=card>
  <h3>Last 7 days</h3>
  <canvas id="dayChart" width="700" height="170" style="width:100%;height:170px"></canvas>
  <div class="legend" style="margin-top:.4em">
    <span style="color:#facc15">&#9632; Solar</span>
    <span style="color:#a78bfa">&#9632; DC-DC</span>
    <span style="color:#60a5fa">&#9632; Charger</span>
    <span style="color:#f87171">&#9632; Load</span>
    <span class=muted>Wh in (stacked) vs out</span>
  </div>
  <div id="dayEmpty" class="muted" style="font-size:.82em"></div>
</div>
<script>
function ss(id,t){document.getElementById(id).textContent=t;}
function durStr(s){s=Math.round(s);var d=Math.floor(s/86400);s-=d*86400;
 var h=Math.floor(s/3600);s-=h*3600;var m=Math.floor(s/60);
 if(d>0)return d+'d '+h+'h';if(h>0)return h+'h '+m+'m';return m+'m';}
function dt2(e){var d=new Date(e*1000);return d.toISOString().slice(0,16).replace('T',' ');}
function fmtDay(ymd){var s=''+ymd;return s.slice(4,6)+'/'+s.slice(6,8);}
function drawDays(){
 var c=document.getElementById('dayChart');if(!c||!c.getContext)return;
 var ctx=c.getContext('2d'),W=c.width,H=c.height,padL=38,padR=8,padT=8,padB=18;
 ctx.clearRect(0,0,W,H);
 var days=(data&&data.days)?data.days.slice(-7):[];
 var em=document.getElementById('dayEmpty');
 if(!days.length){em.textContent=(data&&data.clock)?
  'No completed days yet — check back after midnight.':
  'Needs an NTP clock (configure WiFi) to track daily history.';return;}
 em.textContent='';
 var mx=1;days.forEach(function(d){var i=d.solar_wh+d.dcdc_wh+d.charger_wh;
  if(i>mx)mx=i;if(d.load_wh>mx)mx=d.load_wh;});
 function Y(v){return padT+(H-padT-padB)*(1-v/mx);}
 ctx.fillStyle='#7d8da1';ctx.font='9px system-ui';ctx.textAlign='right';
 [mx,mx/2,0].forEach(function(v){var y=Y(v);ctx.strokeStyle='#1f2c3a';
  ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();
  ctx.fillText(v.toFixed(0),padL-4,y+3);});
 var n=days.length,slot=(W-padL-padR)/n,bw=slot*0.30,base=Y(0);
 ctx.textAlign='center';
 days.forEach(function(d,i){var cx=padL+slot*(i+0.5),xi=cx-bw-1,xo=cx+1,acc=0;
  [['solar_wh','#facc15'],['dcdc_wh','#a78bfa'],['charger_wh','#60a5fa']].forEach(function(p){
   var v=d[p[0]]||0;if(v<=0)return;var y0=Y(acc),y1=Y(acc+v);
   ctx.fillStyle=p[1];ctx.fillRect(xi,y1,bw,y0-y1);acc+=v;});
  ctx.fillStyle='#f87171';var yo=Y(d.load_wh);ctx.fillRect(xo,yo,bw,base-yo);
  ctx.fillStyle='#7d8da1';ctx.fillText(fmtDay(d.date),cx,H-5);});
}
var scope='trip',data=null;
function setScope(s){scope=s;document.getElementById('resetScope').value=s;
 var bs=document.querySelectorAll('.winbtn');for(var i=0;i<bs.length;i++)
  bs[i].classList.toggle('active',bs[i].dataset.s===s);render();}
async function load(){try{data=await(await fetch('/api/stats')).json();}catch(e){return;}render();drawDays();}
function render(){
 if(!data)return;var b=data[scope];if(!b)return;
 ss('s_solar_ah',b.solar_ah.toFixed(1));ss('s_solar_wh',b.solar_wh.toFixed(0));
 ss('s_dcdc_ah',b.dcdc_ah.toFixed(1));ss('s_dcdc_wh',b.dcdc_wh.toFixed(0));
 ss('s_charger_ah',b.charger_ah.toFixed(1));ss('s_charger_wh',b.charger_wh.toFixed(0));
 ss('s_charged_ah',b.charged_ah.toFixed(1));ss('s_charged_wh',b.charged_wh.toFixed(0));
 ss('s_load_ah',b.load_ah.toFixed(1));ss('s_load_wh',b.load_wh.toFixed(0));
 ss('s_discharged_ah',b.discharged_ah.toFixed(1));ss('s_discharged_wh',b.discharged_wh.toFixed(0));
 var net=b.charged_ah-b.discharged_ah;
 ss('net','Net battery balance: '+(net>=0?'+':'')+net.toFixed(1)+' Ah ('+(net>=0?'+':'')+(b.charged_wh-b.discharged_wh).toFixed(0)+' Wh)');
 ss('s_soc',b.soc_min==null?'--':b.soc_min.toFixed(0)+'% .. '+b.soc_max.toFixed(0)+'%');
 ss('s_v',b.v_min==null?'--':b.v_min.toFixed(2)+' .. '+b.v_max.toFixed(2)+' V');
 ss('s_psolar',b.peak_solar_w.toFixed(0)+' W');
 ss('s_pload',b.peak_load_w.toFixed(0)+' W');
 ss('s_pcur',b.peak_charge_a.toFixed(1)+' / '+b.peak_discharge_a.toFixed(1)+' A');
 ss('s_time',durStr(b.charge_secs)+' / '+durStr(b.discharge_secs));
 var since='Accumulated over '+durStr(b.duration_secs);
 if(data.clock&&b.start_epoch>0)since+=' &middot; since '+dt2(b.start_epoch);
 else if(scope=='today')since+=' (no clock yet — “since boot”; set WiFi for NTP)';
 document.getElementById('since').innerHTML=since;
 document.getElementById('resetBtn').textContent=scope=='total'?'reset lifetime':'reset '+scope;
}
document.getElementById('resetForm').addEventListener('submit',function(e){
 if(scope=='total'&&!confirm('Reset lifetime totals? This cannot be undone.'))e.preventDefault();});
var sb=document.querySelectorAll('.winbtn');
for(var i=0;i<sb.length;i++)sb[i].addEventListener('click',function(){setScope(this.dataset.s);});
setScope('trip');load();setInterval(load,5000);
</script>
)HTML";

static const char kDiagPage[] = R"HTML(
<div class=card><h3>Diagnostics</h3>
<p class=muted>Live decoded values plus the raw decrypted advertisement bytes for
each configured device &mdash; use this to confirm a parser against
VictronConnect.</p>
<div id=diag>Loading&hellip;</div></div>
<script>
function esc(s){return (s+'').replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
async function load(){let d;try{d=await(await fetch('/api/diag')).json();}catch(e){return;}
 var el=document.getElementById('diag');
 if(!d.length){el.innerHTML='<p class=muted>No devices configured.</p>';return;}
 el.innerHTML=d.map(function(dev){
  var rows=dev.fields.map(function(f){return '<tr><td class=muted>'+esc(f[0])+
   '</td><td style="text-align:right">'+esc(f[1])+'</td></tr>';}).join('');
  var st=dev.seen?(dev.stale?'<span style="color:#f87171">stale '+dev.age+'s</span>':
   '<span style="color:#34d399">live, '+dev.age+'s ago</span>'):'<span class=muted>never seen</span>';
  return '<div style="margin-bottom:1em;border-bottom:1px solid var(--line);padding-bottom:.7em">'+
   '<div style="display:flex;justify-content:space-between"><b>'+esc(dev.name)+
   '</b><span class=muted>'+esc(dev.type)+' &middot; '+esc(dev.model)+'</span></div>'+
   '<div class=muted style="font-size:.78em">'+esc(dev.mac)+' &middot; '+st+'</div>'+
   '<table>'+rows+'</table>'+
   '<div class=muted style="font-size:.74em;word-break:break-all;margin-top:.3em">raw: '+
   (dev.raw||'(none)')+'</div></div>';
 }).join('');}
load();setInterval(load,2000);
</script>
)HTML";
