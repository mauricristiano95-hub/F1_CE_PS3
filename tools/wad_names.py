"""Recovers the paths of .wad entries (the archives keep only CRC-32 hashes, tools/wad.py).

  python -I tools/wad_names.py <USRDIR> <names.txt> [EBOOT.elf]

Candidates: printable strings of EBOOT.elf and of every decoded entry (Lua chunks carry their source name, scene
graphs and materials name their textures), each also tried lower/upper-cased, with .lua -> .lub, with sibling
extensions (.sg/.sgskel/.skel/.anim, .gtf/.dds/.ddsn), and joined to the directories of names already found.
Repeats until a pass finds nothing new; writes every name that matches an entry of any archive to names.txt
(appending to the names already there).
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import wad  # noqa: E402

STR = re.compile(rb'[\x21-\x7e][\x20-\x7e]{3,200}')
PATHLIKE = re.compile(r'^[@./\\]*[\w\- ]+([/\\][\w\- .]+)*\.\w{1,7}$')   # a file name with an extension


def strings(data):
    return {s for s in (m.group().decode('latin1').strip() for m in STR.finditer(data)) if PATHLIKE.match(s)}
EXT_GROUPS = [('.sg', '.sgskel', '.skel', '.sgSkel', '.anim', '.atganim', '.pvs', '.crowd'),
              ('.gtf', '.dds', '.ddsn', '.ddsa', '.png', '.tga'),
              ('.lua', '.lub'), ('.txt', '.bin', '.xml', '.ini'), ('.bpe', '.fxg', '.blf')]
EXT_OF = {e.lower(): g for g in EXT_GROUPS for e in g}


def variants(s):
    s = s.strip().strip('"').replace('\\', '/')
    while s.startswith(('./', '../', '/', '@')):
        s = s[3:] if s.startswith('../') else s[1:] if s[0] in '/@' else s[2:]
    if not s or len(s) > 200:
        return
    yield s
    stem, dot, ext = s.rpartition('.')
    if dot and ext and '/' not in ext:
        for e in EXT_OF.get('.' + ext.lower(), ()):
            yield stem + e


def main():
    usrdir = Path(sys.argv[1])
    out = Path(sys.argv[2])
    archives = sorted(usrdir.glob('*.wad'))
    wads = [wad.Wad(p) for p in archives]
    hashes = {}
    for w in wads:
        for e in w.entries:
            hashes.setdefault(e[0], []).append((w, e))
    found = {}
    if out.exists():
        for line in out.read_text(encoding='latin1').splitlines():
            if line and wad.name_hash(line) in hashes:
                found[wad.name_hash(line)] = line
    pending = set()
    if len(sys.argv) > 3:
        pending |= strings(Path(sys.argv[3]).read_bytes())
    # every decoded entry is a source of strings, once
    scanned = set()
    dirs = set()

    def try_name(s):
        h = wad.name_hash(s)
        if h in hashes and h not in found:
            found[h] = s
            return True
        return False

    bases = set()          # file names (with variants) seen anywhere, tried in every known directory
    rnd = 0
    while True:
        rnd += 1
        new = []
        new_bases = set()
        for s in pending:
            for v in variants(s):
                if try_name(v):
                    new.append(v)
                base = v.rsplit('/', 1)[-1]
                if '.' in base and base not in bases:
                    new_bases.add(base)
        pending.clear()
        new_dirs = {n.rsplit('/', 1)[0] + '/' for n in found.values() if '/' in n} - dirs
        for d in dirs:
            for b in new_bases:
                if try_name(d + b): new.append(d + b)
        bases |= new_bases
        for d in new_dirs:
            for b in bases:
                if try_name(d + b): new.append(d + b)
        dirs |= new_dirs
        # strings of entries not yet scanned (named or not)
        for h, lst in hashes.items():
            if h in scanned:
                continue
            scanned.add(h)
            w, e = lst[0]
            try:
                data = w.read(e)
            except Exception:
                continue
            pending |= strings(data)
        print(f'pass {rnd}: {len(new)} new, {len(found)} of {len(hashes)} named, {len(pending)} strings', flush=True)
        if not new and not pending:
            break
    out.write_text('\n'.join(sorted(found.values(), key=str.lower)) + '\n', encoding='latin1')


if __name__ == '__main__':
    main()
