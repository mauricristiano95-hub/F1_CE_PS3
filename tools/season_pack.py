"""Season packs: an editable copy of the game's cars, drivers and database in MODS/<pack>/, compiled into the files
the runtime overrides (runtime/mods.cpp). The original game files are only read.

  python -I tools/season_pack.py template <USRDIR> <MODS/pack> [--models]
  python -I tools/season_pack.py helmets <USRDIR> <PLAYER_HELMETS/pack>
  python -I tools/season_pack.py build <USRDIR> <MODS/pack | PLAYER_HELMETS/pack>
  python -I tools/season_pack.py tyremaker <MODS/pack> <name> [--like Bridgestone|Michelin]

template writes (folders named by the database ids: CARS/Honda, DRIVERS/jbutton, ...)
  pack.ini                          name and description shown by the launcher / Content Manager
  DATABASE/entities.json            drivers, teams, cars, tracks: names, texts, car numbers, ratings (tools/entities.py)
  DATABASE/dynamics.json            per-team car performance: aero, engine, gearbox, failure rates (tools/dynamics.py)
  DATABASE/points.txt               the points of 1st, 2nd, ... (read by the game itself, runtime/mods.cpp)
  CARS/<team>/livery/*.png          base paint (uv0) and sponsor decals (uv1) of the race model, their low-detail
                                    (LOD) versions for distant cars, panels, driver branding swaps (one per car);
                                    names differ per team: template.json "teams" says which file is which
  CARS/<team>/driver/*.png          the drivers' overalls and arms seen in the car
  CARS/<team>/ui/*.png              team logo, driver select picture
  CARS/<team>/model/<team>.obj      with --models: the car (lod0) for Blender, with its textures as DDS; edit it
                                    (any vertex / triangle count, new UVs) and build imports the changed parts into
                                    the race model and the garage model (sg.py objimport; object names must stay;
                                    skinned parts are not); the decal UVs (uv1) are not in the OBJ
  CARS/<team>/model/<team>.glb      with --models: the same car as glTF with both UV sets (UVMap = paint,
                                    UVMap.001 = decals) and the scene tree, using the livery PNGs above (edit them,
                                    Image > Reload in Blender); run tools/blender_decals.py in Blender after the
                                    import to layer the decals on the paint. Export from Blender as glTF Binary
                                    over this file: build imports the changed parts, decal UVs included (when both
                                    the .glb and the .obj were edited, the .glb is used)
  DRIVERS/<id>/Helmet/*.png         helmet Colour and Specular (the paint; the shape is chosen in entities.json:
                                    helmet_model 0-2 = HELMETS/Helmet<n>, helmet_vent 0-5 = HELMETS/Vent<n>, -1 none)
  HELMETS/<shape>/<shape>.obj       with --models: the helmet shapes for Blender (in-car shells, vents and their
                                    low-detail versions, Garage_Helmet, Classic_<car>); build imports edited ones
                                    (the rigid one-bone kind: in-car and garage helmets; not the classic heads);
                                    <shape>.glb next to it, as the car's
  DRIVERS/<id>/ui/*.png             pre-race photo, driver select picture
  WHEELS/Alloys/*.png               wheel rims (colour _CA, normal _N, _Blurred_ for spinning wheels); a car names its
                                    rims in the database, several cars share some (Alloy_Default: Honda, MF1, Super
                                    Aguri): template.json "teams" lists each team's
  WHEELS/SideWalls/*.png, Tred/     tyre sidewalls (per tyre maker: _BS Bridgestone, _M Michelin) and treads
                                    the tyre maker of a car = its sidewall set, entities.json cars field68
                                    (Models/TextureSwaps/Wheels/SideWalls/<set>); tyremaker starts a custom maker:
                                    <name>_SideWall_Blur0-3.png (a copy of a maker's set to paint; Blur1-3 are the
                                    spinning versions, a missing one uses Blur0), added to the game by build
  WHEELS/model/Wheels.obj, .glb     with --models: the tyres and rims every car uses (lod0-2, punctured rims not
                                    exported), using the WHEELS/ PNGs; build imports edited ones like the cars'
  WHEELS/model/Wheels_Garage.*      the garage / menu car's wheels (stream.wad wheels_fe.sg), edited separately
  CREW/<team>/*.png                 pit crew overalls and textures
  COUNTRIES/<country>/*.png         flag, driver select picture (used for custom drivers)
  UI/<path>.png                     the other menu, HUD and TV overlay images whose paths are known
  template.json                     what each editable file replaces, where the game uses it ("used_in": the race
                                    car in game, the garage / menu car, menus), its original checksum; "teams": the
                                    role of each team's livery and wheel files
Where the textures are used: the race model (offline.wad CarParts.sg) is the car in races and time trials; the
garage model (stream.wad <Team>06_fe.sg) is the car in the menus (driver select garage). The garage model embeds its
own copy of the sponsor decals (build updates both from the one PNG) but takes the base paint from the race model
(tested 2026-11-14: a race-model-only paint change showed in the garage); LOD textures are race only.
Progress lines for the Content Manager: "PROGRESS <done>/<total> <what>".
helmets writes a player helmet pack instead (PLAYER_HELMETS/<pack>/ next to MODS, chosen apart from the season
pack and applied over it, so a player's own helmet goes with every season pack):
  PlayerNN/*.png                    the 45 helmet designs a player profile can choose (Colour, Specular;
                                    Drivers/Players/PlayerNN/Helmet/ in offline.wad)
  pack.ini, template.json           as above
The PNGs are upright (the game stores its textures upside down; build turns them back).
build compares every editable file with its template checksum and compiles the changed ones (PNG, or a DDS of the
same name used as it is, i.e. upside down; any size, encoded in the original's format) into marker folders:
  CARS/<team>/offline.wad/Models/<Model>/CarParts.sg      race model (textures embedded)
  CARS/<team>/stream.wad/Models/<Model>/<Model>_fe.sg     garage / menu model, when it embeds the texture too or
                                                          the OBJ changed parts it shares with the race model
  <folder>/<archive>.wad/<path>                           stand-alone textures, in every archive that has a copy
                                                          (and the custom tyre makers' new sidewall textures)
  WHEELS/model/<archive>.wad/models/wheels/...            edited wheel models
                                                          (PlayerNN/offline.wad/... in a helmet pack)
  DATABASE/offline.wad/entities.bin                       (the game reads offline.wad's copy, not update.wad's)
  DATABASE/offline.wad/dynamics.bin                       (likewise)
Files built earlier that no longer differ are removed, so reverting an edit restores the original.
"""
import concurrent.futures
import functools
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import dynamics  # noqa: E402
import entities  # noqa: E402
import gtf  # noqa: E402
import sg  # noqa: E402
import wad  # noqa: E402

# database team id -> car model folder (Models/<folder>/CarParts.sg)
TEAMS = {'Renault': 'Renault06', 'McLaren': 'McLaren06', 'Ferrari': 'Ferrari06', 'Toyota': 'Toyota06',
         'Williams': 'Williams06', 'Honda': 'Honda06', 'RedBull': 'RedBull06', 'BMW': 'BMWSauber06',
         'Midland': 'Midland06', 'ToroRosso': 'ToroRosso06', 'SuperAguri': 'SuperAguri06', 'Lotus72e': 'Lotus_72e',
         'CooperClimax': 'CooperClimax', 'RenaultRS01': 'RenaultRS01', 'HondaRA272': 'HondaRA272',
         'Lotus49c': 'Lotus49c', 'WilliamsFW18': 'WilliamsFW18', 'WilliamsFW06': 'WilliamsFW06',
         'Alfa158': 'AlfaRomeo158'}
ARCHIVES = ['offline.wad', 'stream.wad']
HELMET_MODELS = ([('offline.wad', f'Animations/Driver/Helmet/{n}.sg') for n in
                  ['Helmet0', 'Helmet1', 'Helmet2', 'LodHelmet0', 'Vent0', 'Vent1', 'Vent2', 'Vent3', 'Vent4', 'Vent5',
                   'LodVent0', 'LodVent1', 'LodVent2', 'LodVent3', 'LodVent4']] +
                 [('stream.wad', 'Animations/FrontEnd/Avatar/Helmet.sg')] +
                 [('offline.wad', f'Animations/Driver/Classics/{c}/Head.sg') for c in
                  ['Lotus49c', 'Lotus72e', 'AlfaRomeo158', 'CooperClimax', 'HondaRA272', 'RenaultRS01', 'WilliamsFW06',
                   'WilliamsFW18']])
# tyre makers: a car names its sidewall texture set in entities.json cars field68 (the game adds _Blur0-3.gtf, the
# higher numbers drawn as the wheel spins faster); a custom maker is a set WHEELS/SideWalls/<name>_SideWall_Blur0-3
# made in the pack (new_tyre_maker), which build adds to the archives. The sidewall normal map has no lettering.
SIDEWALLS = 'Models/TextureSwaps/Wheels/SideWalls/'
TYRE_MAKERS = {'Bridgestone': 'Default_SideWall_BS', 'Michelin': 'Default_SideWall_M'}
SIDEWALL_LEVELS = 4
# the tyre models every car uses (entities cars field65 / field73): pack path stem, the meshes exported
WHEEL_MODELS = [('offline.wad', 'models/wheels/wheels.sg', 'WHEELS/model/Wheels', {'lod0', 'lod1', 'lod2'}),
                ('stream.wad', 'models/wheels/wheels_fe.sg', 'WHEELS/model/Wheels_Garage', None)]
# wheel model texture names -> pack PNG stems (the models name the swapped textures by their source files)
WHEEL_TEXTURE_ALIASES = {'wheels_tyrewall_bs_blur0': 'default_sidewall_bs_blur0',
                         'default_tyretred_dry_ca': 'default_tyretred_dry_blur0_ca',
                         'default_tyretred_dry_s': 'default_tyretred_dry_blur0_s',
                         'default_tyretred_dry_n': 'default_tyretred_dry_blur0_n',
                         'wheels_tyretred_dry_n': 'default_tyretred_dry_blur0_n'}
POINTS_TXT = '''# Points of 1st, 2nd, 3rd ... (up to 22 places; places not listed score nothing). The game reads this file
# itself when the season pack is selected; the original system is 10 8 6 5 4 3 2 1. Example (2010): 25 18 15 12 10 8 6 4 2 1
10 8 6 5 4 3 2 1
'''
UI_PREFIXES = ('frontend/textures/', 'tvoverlays/', 'hud/textures/', 'loading/')


def sha1(path):
    return hashlib.sha1(Path(path).read_bytes()).hexdigest()


def progress(done, total, what):
    print(f'PROGRESS {done}/{total} {what}', flush=True)


def log(text):
    print(text, flush=True)


def tex_stem(path):
    return path.replace('\\', '/').split('/')[-1].rsplit('.', 1)[0].lower()


def livery_roles(game, team, model):
    """The race model's livery files: paint / decals (the two-layer material of the lod0 meshes) and their LOD
    versions (the two-layer material of the other meshes), as pack paths."""
    s = game.scene('offline.wad', f'Models/{model}/CarParts.sg')
    if s is None:
        return {}
    mats = {m.name: m for m in s.materials}
    tex = {t.name: t for t in s.textures}

    def car_tex(m, slot):
        """The embedded car texture of a material slot (not the shared ../ ones, e.g. the tyre dirt layer)."""
        t = tex.get(m.samplers.get(slot))
        ok = t is not None and t.gtf is not None and t.path.replace('\\', '/').lower().startswith('textures/')
        return t if ok else None
    roles, body = {}, {}
    for node, world, depth, path in s.walk():
        m = node.mesh and mats.get(node.mesh.material)
        if not m:
            continue
        lod = 'lod0' not in path
        paint, decals = car_tex(m, 'BaseColour'), car_tex(m, 'BaseColour2')
        if paint and decals:
            for t, role in ((paint, 'paint'), (decals, 'decals')):
                roles.setdefault(role + ('_lod' if lod else ''), f'CARS/{team}/livery/{tex_stem(t.path)}.png')
        elif paint and not lod and 'tre' not in tex_stem(paint.path):
            # the livery mesh (lotus49c_liveryshape, ...) first, else the most geometry
            body[paint.path] = body.get(paint.path, 0) + node.mesh.nverts + (10 ** 7 if node.name.endswith('liveryshape') else 0)
    if 'paint' not in roles and body:
        # one-layer liveries (most classic cars)
        roles['paint'] = f'CARS/{team}/livery/{tex_stem(max(body, key=body.get))}.png'
    return roles


def car_of(ents, team):
    """A team's car record in entities.json (Honda -> Honda2006; the classics share the team id)."""
    return next((c for c in ents['cars'] if c['id'] in (team + '2006', team)), None)


def wheel_roles(ents, team, files):
    """The wheel files a team's car names (entities cars: rims colour / normal, tyre sidewall and tread)."""
    car = car_of(ents, team)
    if car is None:
        return {}
    def rel_of(p):
        return 'WHEELS/' + '/'.join(p.replace('\\', '/').split('/')[3:]).rsplit('.', 1)[0] + '.png' if p else None
    out = {}
    colour, normal = rel_of(car.get('field62')), rel_of(car.get('field63'))
    for key, rel in (('rim', colour), ('rim_normal', normal)):
        if rel and rel in files:
            out[key] = rel
    if colour:
        blurred = colour.replace('_CA.png', '_Blurred_CA.png')
        if blurred in files:
            out['rim_blurred'] = blurred
        bn = colour.replace('_CA.png', '_Blurred_N.png')
        if bn in files:
            out['rim_blurred_normal'] = bn
    for key, field in (('sidewall', 'field68'), ('tread', 'field67')):
        base = rel_of((car.get(field) or '') + '.x') if car.get(field) else None
        if base:
            stem = base.rsplit('.', 1)[0].lower()
            out[key] = sorted(f for f in files if f.lower().startswith(stem))
    return out


def tyre_makers(pack):
    """{maker name: sidewall prefix}: the game's two and the custom sets in the pack's WHEELS/SideWalls."""
    out = dict(TYRE_MAKERS)
    builtin = {p.lower() for p in TYRE_MAKERS.values()}
    for f in sorted((Path(pack) / 'WHEELS' / 'SideWalls').glob('*_SideWall_Blur0.*')):
        prefix = f.stem[:-len('_Blur0')]
        if prefix.lower() not in builtin and f.suffix.lower() in ('.png', '.dds'):
            out.setdefault(prefix[:-len('_SideWall')], prefix)
    return out


def tyre_maker_of(car, makers):
    """The maker name of a car's sidewalls (its prefix when no maker has it), None when the car names none."""
    prefix = (car.get('field68') or '').replace('\\', '/').split('/')[-1]
    return next((n for n, p in makers.items() if p.lower() == prefix.lower()), prefix or None)


def set_tyre_maker(car, makers, name):
    car['field68'] = SIDEWALLS + makers[name]


def sidewall_files(pack, prefix):
    """The pack's PNGs of a sidewall set (Blur0 first)."""
    return sorted((Path(pack) / 'WHEELS' / 'SideWalls').glob(f'{prefix}_Blur?.png'))


def new_tyre_maker(pack, name, like='Bridgestone'):
    """Starts a custom maker: a copy of a maker's sidewall PNGs as WHEELS/SideWalls/<name>_SideWall_Blur0-3.png."""
    if not name or not all(c.isalnum() or c in '_-' for c in name):
        raise ValueError('tyre maker names: letters, digits, _ and - only')
    makers = tyre_makers(pack)
    if name.lower() in (n.lower() for n in makers):
        raise ValueError(f'tyre maker {name} exists already')
    src = Path(pack) / 'WHEELS' / 'SideWalls'
    made = []
    for n in range(SIDEWALL_LEVELS):
        f = src / f'{makers[like]}_Blur{n}.png'
        if not f.exists():
            raise ValueError(f'{f} is missing: create the pack again')
        made.append(src / f'{name}_SideWall_Blur{n}.png')
        made[-1].write_bytes(f.read_bytes())
    return made


def wheel_texture_png(pack, t):
    """The pack PNG a wheel model texture shows (rims, sidewalls, treads from WHEELS/), else None."""
    stem = tex_stem(t.path)
    stem = WHEEL_TEXTURE_ALIASES.get(stem, stem)
    return next((p for p in (Path(pack) / 'WHEELS').rglob('*.png') if p.stem.lower() == stem
                 and 'model' not in p.parts), None)


def car_texture_png(livery, t):
    """The pack livery PNG of a car model texture (the embedded textures/... ones), else None."""
    f = livery / f'{tex_stem(t.path)}.png'
    return f if t.path.replace('\\', '/').lower().startswith('textures/') and f.exists() else None


def used_in(rel, targets, roles=None):
    """Where the game uses a pack file: 'race' (the car in races / time trials, or in-game textures), 'garage'
    (the garage / menu car), 'menus' (menu and loading screens)."""
    out = set()
    for archive, path, key in targets:
        lo = path.lower()
        if lo.endswith('carparts.sg'):
            out.add('race')
        elif lo.endswith('_fe.sg'):
            out.add('garage')
        elif lo.startswith(('frontend/', 'loading/')):
            out.add('menus')
        elif archive == 'stream.wad':
            out.add('garage')
        else:
            out.add('race')
    if roles and rel == roles.get('paint'):
        out.add('garage')       # the garage model takes the race model's base paint
    return sorted(out)


USED_IN_TEXT = {'race': 'in game (races, time trials)', 'garage': 'garage / menu car', 'menus': 'menus'}


class Game:
    def __init__(self, usrdir):
        self.usrdir = Path(usrdir)
        self.wads = {a: wad.Wad(self.usrdir / a) for a in ARCHIVES}
        names = [n.strip() for n in open(Path(__file__).with_name('wad_names.txt'), encoding='latin1') if n.strip()]
        self.names = list({wad.name_hash(n): n for n in reversed(names)}.values())   # one spelling per hash

    def get(self, archive, path):
        # the car models are read once per texture: keep them unpacked (LZSS in Python, ~0.5 s each)
        if path.lower().endswith('.sg'):
            return self.scene(archive, path).data if self.scene(archive, path) else None
        return self.wads[archive].get(path)

    @functools.lru_cache(maxsize=None)
    def scene(self, archive, path):
        data = self.wads[archive].get(path)
        try:
            return sg.SceneGraph(data) if data is not None else None
        except ValueError as e:      # the other mesh storage kind (some classic drivers' heads), not decoded
            print(f'{path}: skipped ({e})')
            return None

    def where(self, path):
        """Archives that have an entry of this path."""
        return [a for a in ARCHIVES if wad.name_hash(path) in self.wads[a].by_hash]


def garage_models(game, model):
    """The garage / menu models of a car: [(archive, wad path)]."""
    return [('stream.wad', n) for n in game.names
            if n.lower().startswith(f'models/{model.lower()}/') and n.lower().endswith('_fe.sg')
            and 'stream.wad' in game.where(n)]


def model_sources(game, team, model):
    """Team textures embedded in the race and garage models: {pack path: [(archive, wad path, texture key)]}."""
    out = {}
    models = [('offline.wad', f'Models/{model}/CarParts.sg')] + garage_models(game, model)
    for archive, path in models:
        data = game.get(archive, path)
        if data is None:
            continue
        for t in game.scene(archive, path).textures:
            if t.gtf is None or t.gtf.cube or not t.path.replace('\\', '/').lower().startswith('textures/'):
                continue
            stem = t.path.replace('\\', '/').split('/')[-1].rsplit('.', 1)[0].lower()
            out.setdefault(f'CARS/{team}/livery/{stem}.png', []).append((archive, path, stem))
    return out


def player_helmet_sources(game):
    """The player profile's helmet designs: {helmet pack path: [(archive, wad path, None)]}."""
    out = {}
    for n in game.names:
        parts = n.replace('\\', '/')[:-4].split('/')
        if n.lower().endswith('.gtf') and len(parts) == 5 and [x.lower() for x in parts[:2]] == ['drivers', 'players']:
            for archive in game.where(n):
                out.setdefault(f'{parts[2]}/{parts[4]}.png', []).append((archive, n, None))
    return out


def standalone_sources(game, ents):
    """Stand-alone textures with known paths (not the player helmets): {pack path: [(archive, wad path, None)]}."""
    drivers = {r['id'].lower(): r['id'] for r in ents['drivers']}
    teams = {t.lower(): t for t in TEAMS}
    teams.update({r['id'].lower(): r['id'] for r in ents['teams'] if r['id'] in TEAMS})
    countries = {r['id'].lower(): r['id'] for r in ents['countries']}
    swaps = {}          # file name prefix -> team (driver branding swaps)
    for team, model in TEAMS.items():
        swaps[model.lower() + '_'] = team
    out = {}
    for n in game.names:
        lo = n.lower().replace('\\', '/')
        if not lo.endswith('.gtf'):
            continue
        parts = lo[:-4].split('/')
        orig = n.replace('\\', '/')[:-4].split('/')
        rel = None
        if lo.startswith('models/textureswaps/wheels/'):
            rel = 'WHEELS/' + '/'.join(orig[3:])
        elif parts[0] == 'drivers' and len(parts) > 2 and parts[1] in drivers:
            rel = f'DRIVERS/{drivers[parts[1]]}/' + '/'.join(orig[2:])
        elif parts[0] == 'teams' and len(parts) > 3 and parts[2] == 'pitcrew' and parts[1] in teams:
            rel = f'CREW/{teams[parts[1]]}/' + '/'.join(orig[3:])
        elif parts[0] == 'teams' and len(parts) > 2 and parts[1] in teams:
            rel = f'CARS/{teams[parts[1]]}/driver/' + '/'.join(orig[2:])
        elif parts[-1].startswith(tuple(swaps)) and ('textureswaps' in parts or parts[0] == 'effects'):
            team = next(t for p, t in swaps.items() if parts[-1].startswith(p))
            rel = f'CARS/{team}/livery/{parts[-1]}'
        elif lo.startswith('frontend/textures/teamlogos/big/') and parts[-1] in teams:
            rel = f'CARS/{teams[parts[-1]]}/ui/TeamLogo_Big'
        elif lo.startswith('frontend/textures/prerace/drivers/') and parts[-1] in drivers:
            rel = f'DRIVERS/{drivers[parts[-1]]}/ui/PreRace'
        elif lo.startswith('frontend/textures/prerace/driver_select/'):
            x = parts[-1]
            rel = f'DRIVERS/{drivers[x]}/ui/DriverSelect' if x in drivers else \
                f'CARS/{teams[x]}/ui/DriverSelect' if x in teams else \
                f'COUNTRIES/{countries.get(x, orig[-1])}/DriverSelect'
        elif lo.startswith('frontend/textures/profile/flags/'):
            rel = f'COUNTRIES/{countries.get(parts[-1], orig[-1])}/Flag'
        elif lo.startswith(UI_PREFIXES):
            rel = 'UI/' + '/'.join(parts[:-1] + orig[-1:])     # the game spells folders both ways (FrontEnd)
        if rel is None:
            continue
        for archive in game.where(n):
            out.setdefault(rel + '.png', []).append((archive, n, None))
    return out


def write_upright_png(job):
    """(GTF bytes, png path): the texture's top mip level as an upright PNG (the game stores textures upside down)."""
    raw, png = job
    t = gtf.Gtf(raw)
    Path(png).parent.mkdir(parents=True, exist_ok=True)
    w, h, px = gtf.decode_level(t, 0)
    gtf.write_png(png, w, h, gtf.flip_rows(w, h, px))


def group_of(rel):
    """Folder of the pack the built files of an editable file go to (CARS/Honda, DRIVERS/jbutton, UI, Player05)."""
    p = Path(rel).parts
    return p[0] if p[0] == 'UI' or len(p) < 3 else f'{p[0]}/{p[1]}'


def cmd_template(usrdir, pack, models):
    game = Game(usrdir)
    pack = Path(pack)
    if (pack / 'template.json').exists():
        raise SystemExit(f'{pack} already has a template; delete it first')
    ents = entities.export(entities.parse(game.get('offline.wad', 'entities.bin')))
    log(f'Database: {len(ents["drivers"])} drivers, {len(ents["teams"])} teams, {len(ents["cars"])} cars')
    sources, models_written, team_roles, gltf_jobs = {}, {}, {}, []
    for i, (team, model) in enumerate(TEAMS.items()):
        progress(i, len(TEAMS), f'reading the {team} car models')
        if game.get('offline.wad', f'Models/{model}/CarParts.sg') is None:
            log(f'{team}: no race model, skipped')
            continue
        found = model_sources(game, team, model)
        for rel, targets in found.items():
            sources.setdefault(rel, []).extend(targets)
        team_roles[team] = livery_roles(game, team, model)
        log(f'{team}: {len(found)} car textures (race model' +
            (', garage model' if garage_models(game, model) else '') + ')' +
            ''.join(f', {k} {Path(v).name}' for k, v in team_roles[team].items() if not k.endswith('_lod')))
        if models:
            mdir = pack / 'CARS' / team / 'model'
            mdir.mkdir(parents=True, exist_ok=True)
            sg.cmd_obj(game.scene("offline.wad", f"Models/{model}/CarParts.sg"), mdir / f'{team}.obj',
                       {'lod0'})
            models_written[f'CARS/{team}/model/{team}.obj'] = ['offline.wad', f'Models/{model}/CarParts.sg']
            gltf_jobs.append((('offline.wad', f'Models/{model}/CarParts.sg'), f'CARS/{team}/model/{team}.glb',
                              {'lod0'}, functools.partial(car_texture_png, pack / 'CARS' / team / 'livery')))
    progress(len(TEAMS), len(TEAMS), 'car models read')
    found = standalone_sources(game, ents)
    for rel, targets in found.items():
        sources.setdefault(rel, []).extend(targets)
    log(f'{len(found)} other textures with known names (drivers, helmets, crew, wheels, menus)')
    for team in team_roles:
        team_roles[team].update(wheel_roles(ents, team, sources))
    if models:
        log('Helmet shapes: exporting OBJ models')
        # helmet shapes, shared by the drivers (entities.json: helmet_model, helmet_vent); the paint is per driver
        for archive, path in HELMET_MODELS:
            if game.scene(archive, path) is None:
                continue
            stem = path.split('/')[-1].rsplit('.', 1)[0]
            if path.lower().startswith('animations/driver/classics/'):
                stem = 'Classic_' + path.split('/')[-2]
            elif path.lower().startswith('animations/frontend/'):
                stem = 'Garage_Helmet'
            rel = f'HELMETS/{stem}/{stem}.obj'
            (pack / rel).parent.mkdir(parents=True, exist_ok=True)
            sg.cmd_obj(game.scene(archive, path), pack / rel)
            models_written[rel] = [archive, path]
            gltf_jobs.append(((archive, path), rel[:-4] + '.glb', None, None))
        log('Tyres: exporting the wheel models')
        # shared by every car; the rims, sidewalls and treads are the WHEELS/ textures each car names
        for archive, path, stem, only in WHEEL_MODELS:
            if game.scene(archive, path) is None:
                continue
            (pack / stem).parent.mkdir(parents=True, exist_ok=True)
            sg.cmd_obj(game.scene(archive, path), pack / f'{stem}.obj', only)
            models_written[f'{stem}.obj'] = [archive, path]
            gltf_jobs.append(((archive, path), f'{stem}.glb', only, functools.partial(wheel_texture_png, pack)))
    manifest = {'files': {}, 'built': [], 'teams': team_roles}
    write_images(pack, sources, game, manifest)
    if gltf_jobs:
        log('Exporting glTF models (both UV sets, for Blender)')
    # the glTF files use the pack's PNGs themselves: written after them
    for (archive, path), rel, only, texture_file in gltf_jobs:
        sg.cmd_gltf(game.scene(archive, path), pack / rel, only, texture_file=texture_file)
        models_written[rel] = [archive, path]
    manifest['models'] = {rel: {'sha1': sha1(pack / rel), 'target': t} for rel, t in models_written.items()}
    for rel, info in manifest['files'].items():
        team = Path(rel).parts[1] if rel.startswith('CARS/') else None
        info['used_in'] = used_in(rel, info['targets'], team_roles.get(team))
    log('Database files: entities.json, dynamics.json, points.txt')
    db = pack / 'DATABASE'
    db.mkdir(parents=True, exist_ok=True)
    (db / 'entities.json').write_text(json.dumps(ents, indent=1, ensure_ascii=False), encoding='utf-8', newline='\n')
    manifest['database'] = sha1(db / 'entities.json')
    dyn = dynamics.export(dynamics.parse(game.get('offline.wad', 'dynamics.bin')))
    (db / 'dynamics.json').write_text(json.dumps(dyn, indent=1), encoding='utf-8', newline='\n')
    manifest['dynamics'] = sha1(db / 'dynamics.json')
    if not (db / 'points.txt').exists():
        (db / 'points.txt').write_text(POINTS_TXT, encoding='utf-8', newline='\n')
    finish_template(pack, manifest)


def cmd_helmets(usrdir, pack):
    game = Game(usrdir)
    pack = Path(pack)
    if (pack / 'template.json').exists():
        raise SystemExit(f'{pack} already has a template; delete it first')
    manifest = {'files': {}, 'built': []}
    write_images(pack, player_helmet_sources(game), game, manifest)
    for rel, info in manifest['files'].items():
        info['used_in'] = used_in(rel, info['targets'])
    finish_template(pack, manifest)


def write_images(pack, sources, game, manifest):
    """Writes every source texture as an upright PNG and records it in the manifest."""
    jobs = []
    items = sorted(sources.items())
    for i, (rel, targets) in enumerate(items):
        if i % 50 == 0:
            progress(i, len(items), 'extracting textures from the game files')
        archive, path, key = targets[0]
        try:
            if key:
                tex = sg.find_texture(game.scene(archive, path), key)
                raw = game.scene(archive, path).data[tex.gtf_at:tex.gtf_end]
            else:
                raw = game.get(archive, path)
            if gtf.Gtf(raw).cube:
                continue
        except ValueError:
            continue        # a format gtf.py does not handle
        jobs.append((raw, str(pack / rel)))
    log(f'Decoding {len(jobs)} textures to PNG')
    # decoding is pure Python: one process per core
    with concurrent.futures.ProcessPoolExecutor() as pool:
        for i, _ in enumerate(pool.map(write_upright_png, jobs, chunksize=4)):
            if i % 10 == 0 or i + 1 == len(jobs):
                progress(i + 1, len(jobs), f'writing PNG {Path(jobs[i][1]).relative_to(pack).as_posix()}')
    for rel, targets in sorted(sources.items()):
        if (pack / rel).exists():
            manifest['files'][rel] = {'sha1': sha1(pack / rel), 'targets': targets}


def finish_template(pack, manifest):
    if not (pack / 'pack.ini').exists():
        (pack / 'pack.ini').write_text(f'[pack]\nname={pack.name}\ndescription=\n', newline='\n')
    (pack / 'template.json').write_text(json.dumps(manifest, indent=1), newline='\n')
    groups = {}
    for rel in manifest['files']:
        g = Path(rel).parts[0] if len(Path(rel).parts) > 2 else 'player helmet'
        groups[g] = groups.get(g, 0) + 1
    print(f'{pack}: template written: ' + ', '.join(f'{k} {v}' for k, v in sorted(groups.items())) + ' images')


def cmd_build(usrdir, pack):
    game = Game(usrdir)
    pack = Path(pack)
    manifest = json.loads((pack / 'template.json').read_text())
    sg_changes = {}         # (group, archive, wad path) -> [(texture key, GTF bytes)]
    outputs = {}            # pack-relative output path -> bytes
    for rel, info in manifest['files'].items():
        src = pack / rel
        dds = src.with_suffix('.dds')
        if dds.exists():
            src = dds
        elif not src.exists() or sha1(src) == info['sha1']:
            continue
        group = group_of(rel)
        for archive, path, key in info['targets']:
            if key is None:
                orig = gtf.Gtf(game.get(archive, path))
                outputs[f'{group}/{archive}/{path}'] = sg.texture_from_file(src, orig, flip=True)
            else:
                orig = sg.find_texture(game.scene(archive, path), key).gtf
                sg_changes.setdefault((group, archive, path), []).append(
                    (key, sg.texture_from_file(src, orig, flip=True)))
        print(f'{rel}: changed')
    imported = {}           # (group, archive, wad path) -> model with the edited OBJ / glTF geometry
    # glTF first (it carries the decal UVs): an OBJ edited as well for the same model is ignored
    for rel, info in sorted(manifest.get('models', {}).items(), key=lambda kv: not kv[0].endswith('.glb')):
        if not (pack / rel).exists() or sha1(pack / rel) == info['sha1']:
            continue
        print(f'{rel}: changed')
        group, (archive, path) = group_of(rel), info['target']
        if (group, archive, path) in imported:
            print(f'  ignored: the .glb of this model was edited too and is used')
            continue
        if rel.endswith('.glb'):
            src = (pack / rel).read_bytes()
            load = lambda data, **kw: sg.import_gltf(data, src, base_dir=(pack / rel).parent, **kw)  # noqa: E731
        else:
            src = (pack / rel).read_text(encoding='utf-8', errors='replace')
            load = lambda data, **kw: sg.import_obj(data, src, **kw)  # noqa: E731
        race, names = game.get(archive, path), []
        imported[(group, archive, path)] = load(race, log=lambda m: print('  ' + m), changed=names)
        if path.endswith('/CarParts.sg') and names:
            # the garage model has the same lod0 parts: the same edits, placed by the race model's transforms
            for fe_archive, fe_path in garage_models(game, path.split('/')[1]):
                fe = load(game.get(fe_archive, fe_path), log=lambda m: print(f'  {fe_path}: ' + m),
                          ref=race, only=set(names))
                if fe != game.get(fe_archive, fe_path):
                    imported[(group, fe_archive, fe_path)] = fe
    for key in sorted(set(sg_changes) | set(imported)):
        group, archive, path = key
        data = imported.get(key) or game.get(archive, path)
        if key in sg_changes:
            data = sg.replace_textures(data, sg_changes[key])
        outputs[f'{group}/{archive}/{path}'] = data
    generated = build_tyre_makers(game, pack, manifest.get('generated', {}), outputs)
    db = pack / 'DATABASE' / 'entities.json'
    if db.exists() and sha1(db) != manifest.get('database'):
        ents = entities.parse(game.get('offline.wad', 'entities.bin'))
        n = entities.apply(ents, json.loads(db.read_text(encoding='utf-8')))
        if n:
            outputs['DATABASE/offline.wad/entities.bin'] = entities.build(ents)
            print(f'DATABASE/entities.json: {n} change(s)')
    db = pack / 'DATABASE' / 'dynamics.json'
    if db.exists() and sha1(db) != manifest.get('dynamics'):
        dyn = dynamics.parse(game.get('offline.wad', 'dynamics.bin'))
        n = dynamics.apply(dyn, json.loads(db.read_text(encoding='utf-8')))
        if n:
            outputs['DATABASE/offline.wad/dynamics.bin'] = dynamics.build(dyn)
            print(f'DATABASE/dynamics.json: {n} change(s)')
    for rel in manifest.get('built', []):
        if rel not in outputs and (pack / rel).exists():
            (pack / rel).unlink()
            print(f'{rel}: removed (back to the original)')
    for rel, data in outputs.items():
        p = pack / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        print(f'{rel}: built ({len(data)} bytes)')
    manifest['built'] = sorted(outputs)
    manifest['generated'] = generated
    (pack / 'template.json').write_text(json.dumps(manifest, indent=1), newline='\n')
    print(f'{pack}: {len(outputs)} game file(s)')


def build_tyre_makers(game, pack, generated, outputs):
    """The custom tyre makers' sidewall sets as new entries of every archive holding the original sets (encoded like
    the Bridgestone level of the same number; a missing level uses Blur0). Encoding is slow: a level whose PNG did
    not change since the last build keeps its built file. Returns {output: source checksum} for the next build."""
    makers = tyre_makers(pack)
    used = {}
    ents = pack / 'DATABASE' / 'entities.json'
    for car in json.loads(ents.read_text(encoding='utf-8')).get('cars', []) if ents.exists() else []:
        if car.get('field68') and car.get('field65'):     # the classic cars' wheels are part of the car model
            used.setdefault(tyre_maker_of(car, makers), []).append(car['id'])
    for name, cars in used.items():
        if name not in makers:
            print(f'WARNING: {", ".join(cars)}: tyre maker {name} has no WHEELS/SideWalls/{name}_Blur0-3 files '
                  f'(the game would have no sidewall texture)')
    out = {}
    for name, prefix in makers.items():
        if name in TYRE_MAKERS:
            continue
        files = {int(f.stem[-1]): f for f in sidewall_files(pack, prefix)}
        for n in range(SIDEWALL_LEVELS):
            src = files.get(n, files[0])
            like = f'{SIDEWALLS}{TYRE_MAKERS["Bridgestone"]}_Blur{n}.gtf'
            data = None
            for archive in game.where(like):
                rel = f'WHEELS/SideWalls/{archive}/{SIDEWALLS}{prefix}_Blur{n}.gtf'
                if data is None and generated.get(rel) == sha1(src) and (pack / rel).exists():
                    data = (pack / rel).read_bytes()
                if data is None:
                    data = sg.texture_from_file(src, gtf.Gtf(game.get(archive, like)), flip=True)
                outputs[rel] = data
                out[rel] = sha1(src)
        print(f'tyre maker {name}: {SIDEWALL_LEVELS} sidewall textures' +
              (f' (Blur{", Blur".join(str(n) for n in range(SIDEWALL_LEVELS) if n not in files)} from Blur0)'
               if len(files) < SIDEWALL_LEVELS else ''))
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 1
    if argv[0] == 'template':
        cmd_template(argv[1], argv[2], '--models' in argv[3:])
    elif argv[0] == 'helmets':
        cmd_helmets(argv[1], argv[2])
    elif argv[0] == 'build':
        cmd_build(argv[1], argv[2])
    elif argv[0] == 'tyremaker':
        like = argv[argv.index('--like') + 1] if '--like' in argv else 'Bridgestone'
        for f in new_tyre_maker(argv[1], argv[2], like):
            print(f)
    else:
        print(__doc__); return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
