"""Patch the android32 runtime's JNI core to say what the engine asked for.

The port can only see what jni_core logs, and a FindClass miss is silent: the
engine gets NULL, stores it, and later calls a Java method through a NULL method
ID ("[jni] call through a bad method ID 0x0"). This adds two log lines so the
next debug.log names the class and the method.

Run by .github/workflows/build.yml after the runtime is downloaded, so the
repository keeps no patched runtime copy. Nothing here changes behaviour: both
additions are debugPrintf calls next to the existing returns.
"""
import io
import sys

PATH = "runtime/source/jni_core.c"


def patch(text):
    # 1. j_FindClass: say which class was not found.
    old = """  if (!jni_class_exists(buf)) {
    jni_throw("java/lang/NoClassDefFoundError", buf);
    return NULL;
  }"""
    new = """  if (!jni_class_exists(buf)) {
    debugPrintf("[jni] FindClass MISS %s\\n", buf);
    jni_throw("java/lang/NoClassDefFoundError", buf);
    return NULL;
  }"""
    if old not in text:
        raise SystemExit("FindClass body not found: the runtime changed")
    text = text.replace(old, new)

    # 2. method_id: say what the NULL was about (which class pointer, what name).
    old = """  if (!c || !name || !sig)
    return NULL;"""
    new = """  if (!c || !name || !sig) {
    debugPrintf("[jni] methodID NULL: class %p name %s sig %s\\n", (void *)c,
                name ? name : "(null)", sig ? sig : "(null)");
    return NULL;
  }"""
    if old not in text:
        raise SystemExit("method_id guard not found: the runtime changed")
    text = text.replace(old, new)
    return text


def main():
    p = sys.argv[1] if len(sys.argv) > 1 else PATH
    with io.open(p, encoding="utf-8") as f:
        t = f.read()
    if "[jni] FindClass MISS" in t:
        print("already patched")
        return
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(patch(t))
    print("patched", p)


if __name__ == "__main__":
    main()
