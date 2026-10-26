"""Build dist/heddle-<ver>-win.7z with HEADER encryption (filenames hidden).

Usage: make_pkg_7z.py <dist_dir> <version>

py7zr header_encryption=True encrypts the 7z header so NO source filename
appears in the raw archive (a scanner can't see an .exe or .msi in it).
Payload = everything in dist, including the .msi (delivery is via Telegram
DM, where Gmail's extension/category blocks do not apply — see
docs/PACKAGING.md for the email-side findings).
Verified: round-trip decrypt sha256-match + plaintext-name leak scan.
"""
import hashlib
import os
import sys

import py7zr

DIST = sys.argv[1] if len(sys.argv) > 1 else r"D:\Hermes\MV\projects\heddle\dist"
VER = sys.argv[2] if len(sys.argv) > 2 else "0.1.8"
OUT = os.path.join(DIST, "heddle-%s-win.7z" % VER)
PW = "iris1234"  # standing user pass (user-rejected a generated strong one)

if os.path.exists(OUT):
    os.remove(OUT)

entries = []
for root, _d, files in os.walk(DIST):
    for f in sorted(files):
        p = os.path.join(root, f)
        if os.path.abspath(p) == os.path.abspath(OUT):
            continue
        if f.endswith(".7z") or f.endswith(".wixpdb"):
            continue
        arc = os.path.relpath(p, DIST).replace(os.sep, "/")
        entries.append((p, arc))
entries.sort(key=lambda x: x[1])
print("[7z] PAYLOAD:")
for p, arc in entries:
    print("   ", arc, os.path.getsize(p))

with py7zr.SevenZipFile(OUT, "w", password=PW, header_encryption=True) as z:
    for p, arc in entries:
        z.write(p, arc)
print("[7z] WROTE", OUT, os.path.getsize(OUT), "bytes")

# verify: round-trip decrypt, sha256-match every file
with py7zr.SevenZipFile(OUT, "r", password=PW, header_encryption=True) as z:
    names = z.getnames()
    tmp = os.path.join(DIST, ".hdrcheck")
    z.extractall(tmp)
ok = True
for p, arc in entries:
    got = os.path.join(tmp, *arc.split("/"))
    if not os.path.exists(got):
        print("[7z] MISSING after decrypt:", arc)
        ok = False
        continue
    h1 = hashlib.sha256(open(p, "rb").read()).hexdigest()
    h2 = hashlib.sha256(open(got, "rb").read()).hexdigest()
    match = h1 == h2
    ok = ok and match
    print("[7z] extracted", arc, os.path.getsize(got), "sha256", "match" if match else "MISMATCH")

# verify: no plaintext filenames in the raw archive
raw = open(OUT, "rb").read()
leaks = [arc for p, arc in entries if os.path.basename(arc).encode() in raw]
print("[7z] PLAINTEXT_NAME_LEAKS:", leaks if leaks else "NONE (names encrypted)")

import shutil
shutil.rmtree(tmp, ignore_errors=True)
if not ok or leaks:
    print("[7z] VERIFY_FAIL")
    sys.exit(1)
print("[7z] VERIFY_OK")
