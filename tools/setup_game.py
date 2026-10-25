#!/usr/bin/env python3
"""One-step setup from your own Ape Escape 2 disc image.

  1. extracts the disc (your .iso) into the game folder (default: ./game)
  2. checks that it is the PAL release this port is written for (SCES-50885)
  3. generates the C sources in port/gen from the game's executable

Nothing is downloaded and nothing leaves your machine. port/gen and the game folder
are yours alone: never commit or share them.

usage: python3 tools/setup_game.py <your.iso | extracted disc folder> [--game-dir DIR] [--force]
"""
import argparse, hashlib, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import iso_extract, vu_programs   # noqa: E402

ELF = 'SCES_508.85'
ELF_SHA256 = 'b068d3694925f50eb0f1a69b94a13f04bb0d53e931ea1cc0f6e5475401128cda'
IRX_SHA256 = '4368ec57f78280e4f6fe5c3cabbf596d538c0a130bce32b6270b9b0df2f92944'


def find_ci(folder, rel):
    cur = folder
    for part in rel.split('/'):
        if not os.path.isdir(cur): return None
        m = [e for e in os.listdir(cur) if e.lower() == part.lower()]
        if not m: return None
        cur = os.path.join(cur, m[0])
    return cur


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def run(args, cwd):
    print('  $ ' + ' '.join(os.path.relpath(a, ROOT) if os.path.isabs(a) else a for a in args))
    subprocess.check_call([sys.executable] + args, cwd=cwd)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', help='your disc image (.iso/.bin) or a folder with the extracted disc')
    ap.add_argument('--game-dir', default=os.path.join(ROOT, 'game'), help='where to put the game files (default: ./game)')
    ap.add_argument('--force', action='store_true', help='continue even if the executable is not the expected version')
    a = ap.parse_args()

    try:
        import elftools  # noqa: F401
    except ImportError:
        sys.exit('pyelftools is missing: run  %s -m pip install pyelftools' % os.path.basename(sys.executable))

    src = os.path.abspath(a.source)
    game = os.path.abspath(a.game_dir)
    if os.path.isdir(src):
        game = src
        print('[1/3] using the extracted disc in %s' % game)
    elif os.path.isfile(src):
        print('[1/3] extracting %s -> %s' % (src, game))
        iso_extract.extract(src, game)
    else:
        sys.exit('%s: no such file or folder' % src)

    elf = find_ci(game, ELF)
    irx = find_ci(game, 'MODULES/SNDN2DRV.IRX')
    if not elf or not irx:
        sys.exit('%s has no %s or MODULES/SNDN2DRV.IRX: is this Ape Escape 2 (PAL, SCES-50885)?' % (game, ELF))
    print('[2/3] checking the game version')
    ok = sha256(elf) == ELF_SHA256 and sha256(irx) == IRX_SHA256
    if ok: print('  PAL SCES-50885: OK')
    elif a.force: print('  WARNING: unexpected checksum; the port will most likely not work (--force given)')
    else: sys.exit('  %s does not match the PAL release (SCES-50885) this port needs.\n'
                   '  Other releases (NTSC-U, NTSC-J) use different code addresses and are not supported.\n'
                   '  Use --force to try anyway.' % elf)

    print('[3/3] generating port/gen from the game executable (a few seconds)')
    gen = os.path.join(ROOT, 'port', 'gen')
    vudir = os.path.join(ROOT, 'port', 'build-vu')
    shutil.rmtree(gen, ignore_errors=True); os.makedirs(gen)
    shutil.rmtree(vudir, ignore_errors=True)
    rc = os.path.join(ROOT, 'recomp')
    run([os.path.join(rc, 'recomp.py'), elf, gen], rc)
    print('  %d VU programs rebuilt from the executable' %
          vu_programs.build(os.path.join(rc, 'vu_programs.recipe'), elf, vudir))
    run([os.path.join(ROOT, 'tools', 'vu_recomp.py'), vudir, os.path.join(gen, 'vu1_progs.c')], rc)
    run([os.path.join(rc, 'iop_recomp.py'), irx, '1f0000', 'sndn', os.path.join(gen, 'iop_sndn.c')], rc)
    shutil.rmtree(vudir, ignore_errors=True)
    print('\nDone. Game files: %s\nNext: build for your platform (see README.md), then run\n  ae2 "%s"' % (game, game))


if __name__ == '__main__':
    main()
