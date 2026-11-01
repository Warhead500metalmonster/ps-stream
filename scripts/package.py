# Builds the download zip after build.sh: python -I scripts/package.py v0.1.0
import os, sys, zipfile

version = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.makedirs(os.path.join(root, 'release'), exist_ok=True)
out = os.path.join(root, 'release', f'PS-Stream-{version}-windows-x64.zip')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    z.write(os.path.join(root, 'PS Stream.exe'), 'PS Stream/PS Stream.exe')
    z.write(os.path.join(root, 'LICENSE'), 'PS Stream/LICENSE.txt')
    for name in sorted(os.listdir(os.path.join(root, 'dist'))):
        z.write(os.path.join(root, 'dist', name), f'PS Stream/dist/{name}')
print(out)
