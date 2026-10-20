#!/usr/bin/env python3
from pathlib import Path
p = Path("/mnt/e/FCS/embdb/x/web/pits/template.html")
t = p.read_text(encoding="utf-8")
start = t.find("function thicken(steps,p,isNew){")
end = t.find("function ensureAnalysis(p){")
assert start > 0 and end > start, (start, end)
new_fn = r"""function thicken(steps,p,isNew){
  const d=seed(p);
  let src=(steps&&steps.length)?steps.slice():[{t:'初态：示例键 '+d.k0+'/'+d.k1+'/'+d.k2+'（'+d.pgs+' 页）。',
    rows:[{n:'run R',items:['p0:'+d.k0+':data','p1:'+d.k1+':data','p2:'+d.k2+':data']}],kv:{'示例键':d.k0+','+d.k1+','+d.k2}}];
  const pads=[
    {t:'触发：'+(p.found||'问题路径')+' — '+(isNew?'走新修法':'走老缺陷路径')+'。',kv:{'路径':isNew?'新':'老'},_enr:true},
    {t:'中间态：'+(p.cause?String(p.cause).slice(0,140):'口径分叉')+(isNew?' → 按不变量收束。':' → 账/可见性偏离。'),kv:{'口径':isNew?'统一':'分叉 ✗'},_enr:true},
    {t:isNew?('修法：'+(p.fix||'').slice(0,140)):('放大：'+(p.sym||'').slice(0,140)),kv:{'状态':isNew?'收敛':'恶化 ✗'},ok:isNew?undefined:false,_enr:true},
    {t:isNew?('验证：'+(p.verify||'回归绿').slice(0,140)):('残余：'+(p.cost||'继续污染').slice(0,140)),kv:{'判定':isNew?'PASS ✓':'FAIL ✗'},ok:isNew?true:false,_enr:true},
    {t:isNew?('教训：'+(p.lesson||'').slice(0,140)):('代价：'+(p.cost||'').slice(0,140)),ok:isNew?true:false,_enr:true},
  ];
  let i=0;
  while(src.length<6&&i<pads.length){ src.splice(Math.min(src.length-1,1+i),0,pads[i]); i++; }
  while(src.length<6) src.push({t:isNew?'收尾：新码保持不变量。':'收尾：老码缺陷仍在。',ok:!!isNew,_enr:true});
  if(!isNew) src[src.length-1]=Object.assign({},src[src.length-1],{ok:false});
  else src[src.length-1]=Object.assign({},src[src.length-1],{ok:true});
  return src.map((s,idx)=>Object.assign({},s,{tbl:s.tbl||tblFrom(s,p,isNew,Math.min(idx,5))}));
}
"""
p.write_text(t[:start] + new_fn + t[end:], encoding="utf-8")
print("OK thicken replaced")
