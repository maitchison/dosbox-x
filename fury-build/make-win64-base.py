"""make-win64-base.py STOCK_ZIP EXE PATCHES_DIR OUT_ZIP [NOTE]: DOSBox-X's own Win64 portable zip with
mingw-build/mingw-sdl2/dosbox-x.exe replaced by EXE (built by build-win64.sh), and the patches it was
built with in mingw-build/mingw-sdl2/patches/ beside a FURY-BUILD.txt. Every other entry as upstream's."""
import hashlib, os, sys, time, zipfile

stock, exe, patches, out = sys.argv[1:5]
note = sys.argv[5] if len(sys.argv) > 5 else ""
if os.path.exists(out):
    raise SystemExit(f"{out} exists: a base is never overwritten")
D = "mingw-build/mingw-sdl2/"
sha = lambda b: hashlib.sha256(b).hexdigest()
ps = sorted(n for n in os.listdir(patches) if n.endswith(".patch"))
data = open(exe, "rb").read()
lines = [f"DOSBox-X for the Win64 package, made {time.strftime('%Y-%m-%d %H:%M')} by make-win64-base.py",
         f"from: {os.path.basename(stock)} (sha256 {sha(open(stock, 'rb').read())}), every entry kept but {D}dosbox-x.exe",
         f"dosbox-x.exe: sha256 {sha(data)}, {len(data)} bytes", note,
         "patches (in patches/, applied to the source in this order):"]
lines += [f"  {n}  sha256 {sha(open(os.path.join(patches, n), 'rb').read())}" for n in ps]
build = ("\n".join(l for l in lines if l) + "\n").replace("\n", "\r\n").encode()
with zipfile.ZipFile(stock) as s, zipfile.ZipFile(out + ".part", "w", zipfile.ZIP_DEFLATED) as z:
    for i in s.infolist():
        z.writestr(i, data if i.filename == D + "dosbox-x.exe" else s.read(i), compress_type=zipfile.ZIP_DEFLATED)
    z.writestr(D + "FURY-BUILD.txt", build)
    for n in ps:
        z.write(os.path.join(patches, n), D + "patches/" + n)
os.rename(out + ".part", out)
print(build.decode().replace("\r", ""), end="")
print(out, os.path.getsize(out), sha(open(out, "rb").read()))
