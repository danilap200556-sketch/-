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
# Оператор без закрывающей `))",` (типичный итог слияния веток, где обе дописали в конец списка) сливается со следующим
# в одну строку: C++ это компилирует, а база отвечает синтаксической ошибкой. Ловим по следам склейки.
for o in out:
    if 'R"(' in o or re.search(r'(?m)^\s*//', o):
        sys.exit('schemaDdl(): оператор склеился со следующим (нет закрывающей `))",`):\n' + o[:300])
header = '-- Копия schemaDdl() из src/database.cpp (для тестов сайта). Обновляется: python3 web/test/extract-schema.py\n'
open('web/test/schema.sql', 'w', encoding='utf-8').write(header + ';\n'.join(out) + ';\n')
