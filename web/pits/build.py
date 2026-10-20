#!/usr/bin/env python3
"""把 template.html + data/pits_text.json + data/*.js 拼成单文件 ../../fcs-pits.html（无外部请求，沙箱 iframe 可直接看）。
用法：python3 web/pits/build.py   （在仓库根目录跑）
"""
import json, os, glob, re, sys
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..'))
tpl = open(os.path.join(here, 'template.html'), encoding='utf-8').read()
text = json.load(open(os.path.join(here, 'data', 'pits_text.json'), encoding='utf-8'))
data_files = sorted(glob.glob(os.path.join(here, 'data', 'batch*.js')))
prelude = 'window.PITS=[];function PIT(o){window.PITS.push(o);}\n'
data = prelude + '\n'.join(open(f, encoding='utf-8').read() for f in data_files)
# </script> 不能出现在内联脚本里
data = data.replace('</script>', '<\\/script>')
tj = json.dumps(text, ensure_ascii=False).replace('</script>', '<\\/script>')
out = tpl.replace('/*__TEXT__*/', 'window.TEXT=' + tj + ';').replace('/*__DATA__*/', data)
dst = os.path.join(root, 'fcs-pits.html')
open(dst, 'w', encoding='utf-8').write(out)
ids = re.findall(r"PIT\(\{\s*id:\s*('?[\w-]+'?)", data)
print('wrote', dst, len(out.encode('utf-8')) // 1024, 'KB;', 'text entries', len(text), '; storyboards', len(ids), ids)
