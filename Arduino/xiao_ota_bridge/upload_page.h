#pragma once
static const char UPLOAD_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>MAZDASPEED Display</title>
<style>
:root{--bg:#0a0b0d;--card:#15171b;--line:#282c33;--fg:#e6e8ea;--dim:#8b929c;--acc:#ff6a00}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
 font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;align-items:center;padding:1rem 1.25rem;border-bottom:1px solid var(--line)}
header h1{font-size:1rem;letter-spacing:.14em;margin:0;font-weight:600}
header .ver{margin-left:auto;color:var(--dim);font:12px ui-monospace,monospace}
main{max-width:900px;margin:0 auto;padding:1.25rem;display:grid;gap:1.25rem;
 grid-template-columns:1fr 1fr}
@media(max-width:640px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1.1rem}
.card h2{margin:.1rem 0 .9rem;font-size:.72rem;letter-spacing:.16em;color:var(--dim);font-weight:700}
.row{display:flex;align-items:center;gap:.6rem;flex-wrap:wrap;margin-top:.6rem}
button{font:inherit;color:#111;background:var(--acc);border:0;border-radius:8px;
 padding:.5rem .9rem;cursor:pointer;font-weight:600}
button:disabled{opacity:.5;cursor:default}
input[type=file]{color:var(--dim);font-size:.85rem;max-width:100%}
.bar{height:8px;background:#000;border-radius:6px;overflow:hidden;margin:.7rem 0 0}
.bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
.status{margin-top:.6rem;font:12px ui-monospace,monospace;color:var(--dim);white-space:pre-wrap}
.crumb{display:flex;gap:.1rem;align-items:center;font:13px ui-monospace,monospace;color:var(--dim);margin-bottom:.5rem}
.crumb span,.crumb b{cursor:pointer}
.crumb b{color:var(--fg)}
.space{margin-left:auto;color:var(--dim)}
ul.files{list-style:none;margin:0 0 .8rem;padding:0;border:1px solid var(--line);border-radius:8px;overflow:hidden}
ul.files li{display:flex;align-items:center;gap:.6rem;padding:.5rem .7rem;border-top:1px solid var(--line)}
ul.files li:first-child{border-top:0}
ul.files li .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:default}
ul.files li.dir .nm{cursor:pointer;color:var(--acc)}
ul.files li .sz{color:var(--dim);font:12px ui-monospace,monospace}
.del{background:transparent;border:1px solid var(--line);color:var(--dim);border-radius:6px;
 padding:.2rem .55rem;cursor:pointer;font-weight:500;font-size:.8rem}
.del:hover{color:#ff5a5a;border-color:#ff5a5a}
</style></head><body>
<header><h1>MAZDASPEED DISPLAY</h1><span class=ver id=ver>…</span></header>
<main>
  <section class=card>
    <h2>FIRMWARE</h2>
    <input type=file id=fw accept=".bin">
    <div class=row><button id=fwbtn onclick=flashFw()>Flash</button></div>
    <div class=bar><i id=fwbar></i></div>
    <div class=status id=fwlog></div>
  </section>
  <section class=card>
    <h2>FILES</h2>
    <div class=crumb id=crumb></div>
    <ul class=files id=list></ul>
    <input type=file id=up>
    <div class=row><button id=upbtn onclick=upload()>Upload here</button></div>
    <div class=bar><i id=upbar></i></div>
    <div class=status id=uplog></div>
  </section>
</main>
<script>
let cwd="/";
const $=id=>document.getElementById(id);
fetch('/ping').then(r=>r.text()).then(t=>{$('ver').textContent=t;})
  .catch(()=>{$('ver').textContent='(no reply)';});

function fmt(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(2)+' MB';}

function renderCrumb(){
  const parts=cwd.split('/').filter(Boolean);
  let html='<b onclick="nav(\'/\')">/</b>',acc='';
  parts.forEach((p,i)=>{acc+='/'+p;if(i>0)html+='/';html+='<span onclick="nav(\''+acc+'\')">'+p+'</span>';});
  $('crumb').innerHTML=html;
}
function nav(p){cwd=p;list();}
function list(){
  renderCrumb();
  fetch('/api/list?path='+encodeURIComponent(cwd)).then(r=>r.json()).then(d=>{
    const ul=$('list');ul.innerHTML='';
    if(cwd!=='/'){const up=cwd.replace(/\/[^/]+$/,'')||'/';
      ul.insertAdjacentHTML('beforeend','<li class=dir><span class=nm onclick="nav(\''+up+'\')">..</span></li>');}
    d.entries.sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name));
    d.entries.forEach(e=>{
      const li=document.createElement('li');li.className=e.dir?'dir':'';
      const full=(cwd==='/'?'':cwd)+'/'+e.name;
      if(e.dir){li.innerHTML='<span class=nm>'+e.name+'/</span>';
        li.querySelector('.nm').onclick=()=>nav(full);}
      else{li.innerHTML='<span class=nm>'+e.name+'</span><span class=sz>'+fmt(e.size)+'</span>';
        const x=document.createElement('button');x.className='del';x.textContent='Delete';
        x.onclick=()=>del(full,x);li.appendChild(x);}
      ul.appendChild(li);
    });
    const sp=document.createElement('div');sp.className='space';
    sp.textContent=fmt(d.used)+' / '+fmt(d.total);
    $('crumb').appendChild(sp);
  }).catch(()=>{$('list').innerHTML='<li>display not responding</li>';});
}
function del(path,btn){
  if(btn.dataset.armed){
    fetch('/api/delete?path='+encodeURIComponent(path),{method:'POST'})
      .then(r=>r.text()).then(t=>{$('uplog').textContent=t;list();});
  } else { btn.dataset.armed=1;btn.textContent='Confirm';
    setTimeout(()=>{if(btn.dataset.armed){delete btn.dataset.armed;btn.textContent='Delete';}},2500);}
}
function setBusy(b){$('fwbtn').disabled=b;$('upbtn').disabled=b;}
function xhrUpload(url,file,bar,log,done){
  const x=new XMLHttpRequest();x.open('POST',url);
  x.upload.onprogress=e=>{$(bar).style.width=(80*e.loaded/e.total)+'%';};
  x.onload=()=>{$(bar).style.width='100%';$(log).textContent=x.responseText;done&&done();};
  x.onerror=()=>{$(log).textContent='upload error';setBusy(false);};
  const fd=new FormData();fd.append('f',file);x.send(fd);
}
// After a firmware flash the display reboots; keep the buttons locked and poll /ping
// until it answers again, so a second flash can't fire into the reboot window.
function waitReboot(tries){
  tries=tries||0;
  fetch('/ping').then(r=>r.text()).then(t=>{
    if(t && t.indexOf('not responding')<0 && t.indexOf('no reply')<0){
      $('ver').textContent=t;$('fwlog').textContent+='\nback online: '+t;setBusy(false);return;
    }
    next();
  }).catch(next);
  function next(){ if(tries<25){setTimeout(()=>waitReboot(tries+1),1000);} else {setBusy(false);} }
}
function flashFw(){
  const f=$('fw').files[0];if(!f){$('fwlog').textContent='pick a .bin';return;}
  setBusy(true);$('fwlog').textContent='uploading…';
  xhrUpload('/update',f,'fwbar','fwlog',()=>{$('fwlog').textContent+='\nrebooting — waiting for display…';waitReboot();});
}
function upload(){
  const f=$('up').files[0];if(!f){$('uplog').textContent='pick a file';return;}
  setBusy(true);$('uplog').textContent='uploading '+f.name+'…';
  xhrUpload('/api/upload?path='+encodeURIComponent(cwd),f,'upbar','uplog',()=>{$('upbar').style.width='0';setBusy(false);list();});
}
list();
</script></body></html>
)HTML";
