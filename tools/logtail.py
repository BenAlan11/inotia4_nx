import os, sys
p = os.path.expandvars(r"%APPDATA%\eden\sdmc\switch\inotia4\debug.log")
if len(sys.argv) > 1:
    p = sys.argv[1]
b = open(p, 'rb').read()
s = b.decode('utf-8', 'replace')
lines = s.splitlines()
n = int(sys.argv[2]) if len(sys.argv) > 2 else 120
print("total", len(lines))
for l in lines[-n:]:
    print(l)
