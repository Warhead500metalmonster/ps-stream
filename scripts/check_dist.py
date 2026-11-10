# Loads every DLL in dist and starts SDL the way the app does, without opening the app, so a package
# that can't start never gets released. v0.1.39 passed every other check and still didn't start:
# its SDL2.dll needed an SDL3.dll that wasn't there. Usage: python -I scripts/check_dist.py dist
import ctypes, os, sys
dist = os.path.abspath(sys.argv[1])
os.add_dll_directory(dist)
dlls = sorted(n for n in os.listdir(dist) if n.lower().endswith('.dll'))
failed = []
for name in dlls:
    try:
        ctypes.WinDLL(os.path.join(dist, name))
    except OSError as e:
        failed.append(f'{name}: {e}')
print(f'{len(dlls) - len(failed)} of {len(dlls)} DLLs load')
for f in failed:
    print('  failed:', f)
sdl = ctypes.CDLL(os.path.join(dist, 'SDL2.dll'))
sdl.SDL_GetError.restype = ctypes.c_char_p
version = (ctypes.c_uint8 * 3)()
sdl.SDL_GetVersion(version)
print('SDL', '.'.join(map(str, version)))
ok = not failed and version[0] == 2
# A build machine may have no sound card, so audio is only reported
for name, flag, needed in (('video', 0x20, True), ('gamepad', 0x2000, True), ('audio', 0x10, False)):
    started = sdl.SDL_InitSubSystem(flag) == 0
    print(f'SDL {name}:', 'ok' if started else 'failed, ' + sdl.SDL_GetError().decode(errors='replace'))
    ok = ok and (started or not needed)
sdl.SDL_Quit()
sys.exit(0 if ok else 1)
