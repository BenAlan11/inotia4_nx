import os, sys
p = os.path.expandvars(r"%APPDATA%\eden\sdmc\switch\inotia4\debug.log")
if len(sys.argv) > 1:
    p = sys.argv[1]
b = open(p, 'rb').read()
s = b.decode('utf-8', 'replace')
lines = s.splitlines()
pat = sys.argv[2] if len(sys.argv) > 2 else 'csir'
before = int(sys.argv[3]) if len(sys.argv) > 3 else 8
after = int(sys.argv[4]) if len(sys.argv) > 4 else 8
for i, l in enumerate(lines):
    if pat in l:
        print('---- line', i + 1)
        for x in lines[max(0, i - before): i + after + 1]:
            print(x)
