"""Set keys in an ini file, adding them to the right section when missing.

Used by the bring-up loop: hand-editing config.ini between runs kept silently
doing nothing when a key was not already present.
"""
import re
import sys


def set_key(path, section, key, value):
    with open(path, encoding='utf-8') as f:
        text = f.read()

    line = '%s = %s' % (key, value)
    pat = re.compile(r'^%s\s*=.*$' % re.escape(key), re.M)
    if pat.search(text):
        text = pat.sub(line, text, count=1)
    else:
        sec = re.compile(r'^\[%s\][^\n]*\n' % re.escape(section), re.M)
        m = sec.search(text)
        if not m:
            text += '\n[%s]\n%s\n' % (section, line)
        else:
            text = text[:m.end()] + line + '\n' + text[m.end():]

    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


if __name__ == '__main__':
    # setcfg.py <ini> section:key=value [section:key=value ...]
    for spec in sys.argv[2:]:
        sec, rest = spec.split(':', 1)
        k, v = rest.split('=', 1)
        set_key(sys.argv[1], sec, k, v)
        print('%s [%s] = %s' % (k, sec, v))
