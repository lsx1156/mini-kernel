import re

p = 'main.c'
s = open(p, encoding='utf-8').read()
NL = chr(10)
BS = chr(92)

pat = re.compile(r'printf\("(\[pb\] start \w+)' + NL + r'"\); stdio_flush\(\);')
s2, n = pat.subn(lambda m: 'printf("' + m.group(1) + BS + 'n"); stdio_flush();', s)

open(p, 'w', encoding='utf-8').write(s2)
print('fixed', n)
