#!/usr/bin/env python3
"""Пересоздаёт test/schema.sql из schemaDdl() в src/database.cpp (запускать из корня репозитория)."""
import re, sys
s = open('src/database.cpp', encoding='utf-8').read()
a = s.index('schemaDdl()'); a = s.index('{', a); b = s.index('return ddl;', a)
body = re.sub(r'(?m)^\s*//.*$', '', s[a:b])
out = []
for m in re.finditer(r'R"\((.*?)\)"|((?:"(?:[^"\\]|\\.)*"\s*)+)', body, re.S):
    if m.group(1) is not None:
        out.append(m.group(1).strip())
    else:
        out.append(''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))).replace('\\"', '"').strip())
out = [o for o in out if o]
header = '-- Копия schemaDdl() из src/database.cpp (для тестов сайта). Обновляется: python3 web/test/extract-schema.py\n'
open('web/test/schema.sql', 'w', encoding='utf-8').write(header + ';\n'.join(out) + ';\n')
