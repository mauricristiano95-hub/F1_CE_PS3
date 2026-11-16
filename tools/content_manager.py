"""F1 CE Content Manager: season packs and player helmet packs, on top of tools/season_pack.py.

  python tools/content_manager.py [<launcher folder> [<pack> [<tab> [<team id>]]]]

The launcher folder holds MODS/ (season packs) and PLAYER_HELMETS/ (helmet packs) and f1ce_launcher.ini, whose
[game] path is the default game folder. Settings are kept in content_manager.ini in the launcher folder.
Pack contents (see season_pack.py): edits are saved to the pack's files (DATABASE/entities.json, dynamics.json,
points.txt, the PNGs); Build compiles them into the files the game loads. The original game files are only read.
Each picture says where the game uses it: in game (the race car in races and time trials, from offline.wad), the
garage / menu car (stream.wad), or menus (season_pack.used_in).
"""
import configparser
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import season_pack  # noqa: E402

DRIVER_FIELDS = [('first_name', 'First name'), ('last_name', 'Last name'), ('short_name', 'Short name'),
                 ('display_name', 'Display name'), ('abbreviation', 'Abbreviation'), ('car_number', 'Car number'),
                 ('helmet_model', 'Helmet shape (0-2)'), ('helmet_vent', 'Helmet vent (0-5, -1 none)'),
                 ('rating1', 'Rating 1'), ('rating2', 'Rating 2'), ('rating3', 'Rating 3'), ('rating4', 'Rating 4'),
                 ('rating5', 'Rating 5'), ('rating6', 'Rating 6'), ('position_best', 'Expected position: best'),
                 ('position_typical', 'Expected position: typical'), ('position_worst', 'Expected position: worst'),
                 ('birth_date', 'Birth date'), ('birth_place', 'Birth place')]
TEAM_FIELDS = [('name', 'Name'), ('full_name', 'Full name'), ('name_with_engine', 'Name with engine'),
               ('driver1', 'Driver 1'), ('driver2', 'Driver 2'), ('base', 'Base')]
PERF_FIELDS = [('aero', 'drag', 'Drag'), ('aero', 'downforce', 'Downforce'), ('engine', 'engine_w18', 'Engine (w18)'),
               ('engine', 'max_speed_rad_s', 'Max engine speed (rad/s)'),
               ('engine', 'failure_rates', 'Failure rates'), ('gearbox', 'ratios', 'Gear ratios')]
THUMB = 150
USED_LEGEND = ('Where the game uses each picture: "in game" = the car in races and time trials; "garage" = the car '
               'in the menus (driver select garage). The garage car shows the race car\'s base paint and has its own '
               'copy of the sponsor decals (Build updates both); LOD = low-detail versions for distant cars.')


def fmt(v):
    if isinstance(v, list):
        return ', '.join(fmt(x) for x in v)
    if isinstance(v, float):
        return f'{v:.6g}'
    return '' if v is None else str(v)


def parse_like(text, old):
    """The edited text as the type of the old value."""
    text = text.strip()
    if isinstance(old, list):
        return [float(x) for x in text.replace(',', ' ').split()]
    if isinstance(old, bool):
        return text.lower() in ('1', 'true', 'yes')
    if isinstance(old, int):
        return int(float(text))
    if isinstance(old, float):
        return float(text)
    return text if text or old is not None else None


def load_photo(path, size=THUMB):
    try:
        img = tk.PhotoImage(file=str(path))
    except (tk.TclError, OSError):
        return None
    k = max(1, -(-max(img.width(), img.height()) // size))
    return img.subsample(k, k) if k > 1 else img


def open_path(p):
    if sys.platform == 'win32':
        os.startfile(str(p))
    else:
        subprocess.Popen(['xdg-open', str(p)])


class ImageSlot(ttk.Frame):
    """A preview with Replace / Open buttons for one editable PNG of a pack."""

    def __init__(self, master, app, title):
        super().__init__(master)
        self.app, self.path, self.photo = app, None, None
        ttk.Label(self, text=title, font=('Segoe UI', 9, 'bold')).pack(anchor='w')
        self.pic = ttk.Label(self, relief='sunken', anchor='center')
        self.pic.pack(fill='both', expand=True)
        self.info = ttk.Label(self, foreground='#555', wraplength=230, justify='left')
        self.info.pack(anchor='w')
        row = ttk.Frame(self)
        row.pack(fill='x')
        ttk.Button(row, text='Replace...', command=self.replace).pack(side='left')
        ttk.Button(row, text='Open', command=lambda: self.path and open_path(self.path)).pack(side='left')

    def show(self, path, note=''):
        self.path = Path(path) if path else None
        exists = bool(self.path and self.path.exists())
        self.photo = load_photo(self.path) if exists else None
        self.pic.configure(image=self.photo or '', text='' if self.photo else '(none)')
        lines = []
        if self.path:
            lines.append(self.path.name if exists else f'{self.path.name}: not in this pack')
            used = self.app.used_in(self.path)
            if used:
                lines.append('Used: ' + ', '.join(season_pack.USED_IN_TEXT[u] for u in used))
        elif not note:
            lines.append('this car has none')
        if note:
            lines.append(note)
        self.info.configure(text='\n'.join(lines))

    def replace(self):
        if not self.path:
            return
        src = filedialog.askopenfilename(title=f'Replace {self.path.name}',
                                         filetypes=[('Images', '*.png *.dds'), ('All files', '*.*')])
        if not src:
            return
        src = Path(src)
        if src.suffix.lower() == '.dds':      # used as it is (upside down, as the game stores it)
            shutil.copyfile(src, self.path.with_suffix('.dds'))
        else:
            shutil.copyfile(src, self.path)
            self.path.with_suffix('.dds').unlink(missing_ok=True)
        self.app.log(f'{self.path.relative_to(self.app.pack)}: replaced by {src.name} (Build to apply)')
        self.show(self.path)


class Form(ttk.Frame):
    """Label + entry rows bound to a dict."""

    def __init__(self, master, fields):
        super().__init__(master)
        self.fields, self.vars, self.data = fields, {}, None
        for r, (key, label) in enumerate(fields):
            ttk.Label(self, text=label).grid(row=r, column=0, sticky='w', padx=(0, 8), pady=1)
            v = tk.StringVar()
            ttk.Entry(self, textvariable=v, width=44).grid(row=r, column=1, sticky='we', pady=1)
            self.vars[key] = v
        self.columnconfigure(1, weight=1)

    def load(self, data, get=lambda d, k: d.get(k)):
        self.data = data
        for key, _ in self.fields:
            self.vars[key].set(fmt(get(data, key)) if data is not None else '')

    def store(self, get=lambda d, k: d.get(k), put=lambda d, k, v: d.__setitem__(k, v)):
        if self.data is None:
            return 0
        n = 0
        for key, label in self.fields:
            old = get(self.data, key)
            if old is None and not self.vars[key].get().strip():
                continue
            try:
                new = parse_like(self.vars[key].get(), old)
            except ValueError:
                raise ValueError(f'{label}: "{self.vars[key].get()}" is not a valid value')
            if new != old and fmt(new) != fmt(old):
                put(self.data, key, new)
                n += 1
        return n


class App(tk.Tk):
    def __init__(self, launcher_dir):
        super().__init__()
        self.title('F1 CE Content Manager')
        self.geometry('1280x860')
        self.minsize(1000, 700)
        self.q = queue.Queue()
        self.busy = False
        self.pack, self.kind = None, None
        self.ents = self.dyn = None
        self.manifest, self.roles = {}, {}
        self.cfg_path = None
        self.launcher_var, self.game_var = tk.StringVar(), tk.StringVar()
        self._build_ui()
        self.set_launcher_dir(launcher_dir)
        self.after(100, self._poll)

    # ------------------------------------------------------------------ layout
    def _build_ui(self):
        top = ttk.Frame(self, padding=6)
        top.pack(fill='x')
        for r, (label, var, cmd) in enumerate([('Launcher folder (MODS, PLAYER_HELMETS)', self.launcher_var,
                                                 self.browse_launcher),
                                                ('Game folder (PS3_GAME)', self.game_var, self.browse_game)]):
            ttk.Label(top, text=label).grid(row=r, column=0, sticky='w')
            ttk.Entry(top, textvariable=var).grid(row=r, column=1, sticky='we', padx=6)
            ttk.Button(top, text='Browse...', command=cmd).grid(row=r, column=2)
        top.columnconfigure(1, weight=1)

        # bottom first, so the log and the progress bar keep their place when the window is small
        self.logbox = tk.Text(self, height=8, state='disabled', wrap='none')
        self.logbox.pack(side='bottom', fill='x', padx=6, pady=6)
        status = ttk.Frame(self)
        status.pack(side='bottom', fill='x', padx=6, pady=(6, 0))
        self.bar = ttk.Progressbar(status, mode='determinate', maximum=1000)
        self.bar.pack(side='left', fill='x', expand=True)
        self.status_var = tk.StringVar(value='Ready')
        ttk.Label(status, textvariable=self.status_var, width=70).pack(side='left', padx=8)

        body = ttk.PanedWindow(self, orient='horizontal')
        body.pack(fill='both', expand=True, padx=6)
        left = ttk.Frame(body)
        body.add(left, weight=1)
        self.tree = ttk.Treeview(left, show='tree', selectmode='browse')
        self.tree.pack(fill='both', expand=True)
        self.tree.bind('<<TreeviewSelect>>', lambda e: self.select_pack())
        for text, cmd in [('New season pack...', self.new_season_pack), ('New helmet pack...', self.new_helmet_pack),
                          ('Save', self.save), ('Build', self.build), ('Open folder', self.open_pack),
                          ('Refresh', self.refresh)]:
            ttk.Button(left, text=text, command=cmd).pack(fill='x')
        self.models_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(left, text='New packs with 3D models (OBJ + glTF for Blender)', variable=self.models_var).pack(anchor='w')

        right = ttk.Frame(body)
        body.add(right, weight=4)
        self.header = ttk.Label(right, text='Select or create a pack', font=('Segoe UI', 13, 'bold'))
        self.header.pack(anchor='w', pady=4)
        self.nb = ttk.Notebook(right)
        self.nb.pack(fill='both', expand=True)
        self._teams_tab()
        self._rims_tab()
        self._drivers_tab()
        self._points_tab()
        self._helmets_tab()



    def _list_and_detail(self, title):
        tab = ttk.Frame(self.nb, padding=6)
        self.nb.add(tab, text=title)
        lst = tk.Listbox(tab, width=24, exportselection=False)
        lst.pack(side='left', fill='y')
        detail = ttk.Frame(tab, padding=(10, 0))
        detail.pack(side='left', fill='both', expand=True)
        return tab, lst, detail

    def _teams_tab(self):
        self.teams_tab, self.team_list, d = self._list_and_detail('Teams')
        self.team_list.bind('<<ListboxSelect>>', lambda e: self.show_team())
        cols = ttk.Frame(d)
        cols.pack(fill='x')
        self.team_form = Form(cols, TEAM_FIELDS)
        self.team_form.pack(side='left', fill='x', expand=True, anchor='n')
        perf = ttk.LabelFrame(cols, text='dynamics.bin values (meanings unconfirmed: no pace change seen in a test)', padding=6)
        perf.pack(side='left', fill='x', expand=True, anchor='n', padx=(10, 0))
        self.perf_form = Form(perf, [(f'{g}.{k}', label) for g, k, label in PERF_FIELDS])
        self.perf_form.pack(fill='x')
        ttk.Label(d, text=USED_LEGEND, wraplength=900, justify='left', foreground='#555').pack(anchor='w', pady=(6, 0))
        imgs = ttk.Frame(d)
        imgs.pack(fill='both', expand=True, pady=4)
        self.team_imgs = [ImageSlot(imgs, self, t) for t in
                          ('Base paint', 'Sponsor decals', 'Team logo (menus)', 'LOD base paint', 'LOD sponsor decals',
                           'Driver overalls (in the car)')]
        for i, slot in enumerate(self.team_imgs):
            slot.grid(row=i // 3, column=i % 3, sticky='nsew', padx=4, pady=2)
        for c in range(3):
            imgs.columnconfigure(c, weight=1)
        row = ttk.Frame(d)
        row.pack(fill='x')
        ttk.Button(row, text='Open livery folder', command=lambda: self.open_sub('livery')).pack(side='left')
        ttk.Button(row, text='Open 3D model folder', command=lambda: self.open_sub('model')).pack(side='left')

    def _rims_tab(self):
        self.rims_tab, self.rim_list, d = self._list_and_detail('Rims and tyres')
        self.rim_list.bind('<<ListboxSelect>>', lambda e: self.show_rims())
        ttk.Label(d, wraplength=900, justify='left', foreground='#555',
                  text='The rims and tyres a car uses are named in the database; several cars share some files (the '
                       '"Shared with" line), so a change there changes every car listed. "Blurred" versions are drawn '
                       'when the wheels spin fast. The garage car uses the same wheel textures.').pack(anchor='w')
        row = ttk.Frame(d)
        row.pack(fill='x', pady=(6, 0))
        ttk.Label(row, text='Tyre maker (sidewall)').pack(side='left')
        self.maker_var = tk.StringVar()
        self.maker_box = ttk.Combobox(row, textvariable=self.maker_var, state='readonly', width=24)
        self.maker_box.pack(side='left', padx=6)
        self.maker_box.bind('<<ComboboxSelected>>', lambda e: self.set_maker())
        ttk.Button(row, text='New tyre maker...', command=self.new_maker).pack(side='left')
        ttk.Button(row, text='Open 3D tyre model folder',
                   command=lambda: open_path(self.pack / 'WHEELS' / 'model') if self.pack and
                   (self.pack / 'WHEELS' / 'model').is_dir() else messagebox.showinfo(
                       'Content Manager', 'This pack has no 3D models: create it with "New packs with 3D '
                       'models".')).pack(side='left', padx=6)
        imgs = ttk.Frame(d)
        imgs.pack(fill='both', expand=True, pady=4)
        self.rim_imgs = [ImageSlot(imgs, self, t) for t in
                         ('Rim', 'Rim, spinning (blurred)', 'Rim normal map', 'Tyre sidewall', 'Tyre tread (dry)',
                          'Tyre tread (wet)')]
        for i, slot in enumerate(self.rim_imgs):
            slot.grid(row=i // 3, column=i % 3, sticky='nsew', padx=4, pady=2)
        for c in range(3):
            imgs.columnconfigure(c, weight=1)

    def _drivers_tab(self):
        self.drivers_tab, self.driver_list, d = self._list_and_detail('Drivers')
        self.driver_list.bind('<<ListboxSelect>>', lambda e: self.show_driver())
        cols = ttk.Frame(d)
        cols.pack(fill='both', expand=True)
        self.driver_form = Form(cols, DRIVER_FIELDS)
        self.driver_form.pack(side='left', fill='x', expand=True, anchor='n')
        imgs = ttk.Frame(cols)
        imgs.pack(side='left', fill='both', padx=10)
        self.driver_imgs = [ImageSlot(imgs, self, t) for t in ('Helmet paint (in the car)', 'Pre-race photo (menus)')]
        for s in self.driver_imgs:
            s.pack(fill='both', expand=True, pady=4)

    def _points_tab(self):
        self.points_tab = ttk.Frame(self.nb, padding=6)
        self.nb.add(self.points_tab, text='Points')
        ttk.Label(self.points_tab, justify='left',
                  text='Points of 1st, 2nd, 3rd ... separated by spaces (up to 22 places). The original system is '
                       '10 8 6 5 4 3 2 1.\nThe game reads this when the season pack is selected; no Build needed.'
                  ).pack(anchor='w')
        self.points_var = tk.StringVar()
        ttk.Entry(self.points_tab, textvariable=self.points_var, font=('Consolas', 13)).pack(fill='x', pady=8)
        row = ttk.Frame(self.points_tab)
        row.pack(anchor='w')
        for label, pts in [('2006 (original)', '10 8 6 5 4 3 2 1'), ('2003-2009', '10 8 6 5 4 3 2 1'),
                           ('1991-2002', '10 6 4 3 2 1'), ('2010+', '25 18 15 12 10 8 6 4 2 1')]:
            ttk.Button(row, text=label, command=lambda p=pts: self.points_var.set(p)).pack(side='left')

    def _helmets_tab(self):
        self.helmets_tab = ttk.Frame(self.nb, padding=6)
        self.nb.add(self.helmets_tab, text='Player helmets')
        canvas = tk.Canvas(self.helmets_tab, highlightthickness=0)
        sb = ttk.Scrollbar(self.helmets_tab, orient='vertical', command=canvas.yview)
        canvas.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y')
        canvas.pack(fill='both', expand=True)
        self.helmet_grid = ttk.Frame(canvas)
        canvas.create_window((0, 0), window=self.helmet_grid, anchor='nw')
        self.helmet_grid.bind('<Configure>', lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
        self.helmet_slots = []

    # ------------------------------------------------------------------ folders and settings
    def set_launcher_dir(self, d):
        d = Path(d).resolve()
        self.launcher_var.set(str(d))
        self.cfg_path = d / 'content_manager.ini'
        cfg = configparser.ConfigParser()
        cfg.read(self.cfg_path, encoding='utf-8')
        game = cfg.get('paths', 'game', fallback='')
        if not game:
            li = configparser.ConfigParser(strict=False, interpolation=None)
            try:
                li.read(d / 'f1ce_launcher.ini', encoding='utf-8')
                game = li.get('game', 'path', fallback='')
            except configparser.Error:
                game = ''
        self.game_var.set(game)
        self.refresh()

    def save_settings(self):
        cfg = configparser.ConfigParser()
        cfg['paths'] = {'game': self.game_var.get()}
        try:
            with open(self.cfg_path, 'w', encoding='utf-8') as f:
                cfg.write(f)
        except OSError:
            pass

    def browse_launcher(self):
        d = filedialog.askdirectory(title='Launcher folder', initialdir=self.launcher_var.get())
        if d:
            self.set_launcher_dir(d)

    def browse_game(self):
        d = filedialog.askdirectory(title='Game folder (PS3_GAME)', initialdir=self.game_var.get())
        if d:
            self.game_var.set(d)
            self.save_settings()

    def usrdir(self):
        g = Path(self.game_var.get())
        for c in (g / 'USRDIR', g, g / 'PS3_GAME' / 'USRDIR'):
            if (c / 'offline.wad').exists():
                return c
        return None

    def folders(self):
        root = Path(self.launcher_var.get())
        return {'season': root / 'MODS', 'helmet': root / 'PLAYER_HELMETS'}

    def refresh(self):
        self.tree.delete(*self.tree.get_children())
        for kind, title in (('season', 'Season packs (MODS)'), ('helmet', 'Player helmet packs (PLAYER_HELMETS)')):
            node = self.tree.insert('', 'end', text=title, open=True, values=())
            d = self.folders()[kind]
            if d.is_dir():
                for p in sorted(x for x in d.iterdir() if x.is_dir() and not x.name.startswith('.')):
                    self.tree.insert(node, 'end', iid=f'{kind}|{p}', text=p.name)

    # ------------------------------------------------------------------ pack selection
    def select_pack(self):
        sel = self.tree.selection()
        if not sel or '|' not in sel[0]:
            return
        kind, path = sel[0].split('|', 1)
        self.pack, self.kind = Path(path), kind
        built = (self.pack / 'template.json').exists()
        self.header.configure(text=f'{self.pack.name}  ({"season pack" if kind == "season" else "helmet pack"}'
                                   f'{"" if built else ", no template: files only"})')
        for tab in (self.teams_tab, self.rims_tab, self.drivers_tab, self.points_tab):
            self.nb.tab(tab, state='normal' if kind == 'season' else 'hidden')
        self.nb.tab(self.helmets_tab, state='normal' if kind == 'helmet' else 'hidden')
        self.nb.select(self.teams_tab if kind == 'season' else self.helmets_tab)
        if kind == 'season':
            self.load_season()
        else:
            self.load_helmets()

    def load_json(self, rel):
        p = self.pack / rel
        return json.loads(p.read_text(encoding='utf-8')) if p.exists() else None

    def load_manifest(self):
        self.manifest = self.load_json('template.json') or {}
        self.roles = self.manifest.get('teams')
        if self.roles is None and self.manifest:
            # packs made before 2026-11-14: find the livery roles in the game's models now
            self.roles = {}
            usr = self.usrdir()
            if usr:
                self.status_var.set('Reading the car models for this older pack...')
                self.update_idletasks()
                game = season_pack.Game(usr)
                for team, model in season_pack.TEAMS.items():
                    self.roles[team] = season_pack.livery_roles(game, team, model)
                    self.roles[team].update(season_pack.wheel_roles(self.ents or {'cars': []}, team,
                                                                    self.manifest.get('files', {})))
                self.status_var.set('Ready')
            self.log(f'{self.pack.name}: made by an older version (no WHEELS folder, no "used in" data): create a '
                     f'new pack to get every feature')

    def used_in(self, path):
        try:
            rel = Path(path).relative_to(self.pack).as_posix()
        except ValueError:
            return []
        rel = rel[:-4] + '.png' if rel.lower().endswith('.dds') else rel
        info = self.manifest.get('files', {}).get(rel)
        if not info:
            return []
        team = Path(rel).parts[1] if rel.startswith('CARS/') else None
        return info.get('used_in') or season_pack.used_in(rel, info['targets'], (self.roles or {}).get(team))

    def load_season(self):
        self.ents = self.load_json('DATABASE/entities.json')
        self.load_manifest()
        self.dyn = self.load_json('DATABASE/dynamics.json')
        self.team_list.delete(0, 'end')
        self.driver_list.delete(0, 'end')
        self.teams = [t for t in (self.ents or {}).get('teams', []) if t['id'] in season_pack.TEAMS]
        self.drivers = (self.ents or {}).get('drivers', [])
        self.rim_list.delete(0, 'end')
        for t in self.teams:
            self.team_list.insert('end', t.get('name') or t['id'])
            self.rim_list.insert('end', t.get('name') or t['id'])
        for dr in self.drivers:
            self.driver_list.insert('end', f'{dr.get("car_number", "")}  {dr.get("display_name") or dr["id"]}')
        pts = self.pack / 'DATABASE' / 'points.txt'
        self.points_var.set(' '.join(x for line in (pts.read_text(encoding='utf-8').splitlines() if pts.exists()
                                                      else []) for x in line.split('#')[0].split()))
        self.team_form.load(None)
        self.perf_form.load(None)
        self.driver_form.load(None)
        if self.teams:
            self.team_list.selection_set(0)
            self.show_team()
            self.rim_list.selection_set(0)
            self.show_rims()
        if self.drivers:
            self.driver_list.selection_set(0)
            self.show_driver()

    def cur(self, lst, items):
        s = lst.curselection()
        return items[s[0]] if s and s[0] < len(items) else None

    def perf_of(self, team):
        return (self.dyn or {}).get('teams', {}).get(team['id'].lower()) if team else None

    def show_team(self):
        self.store_forms()
        t = self.cur(self.team_list, self.teams)
        self.shown_team = t
        self.team_form.load(t)
        self.perf_form.load(self.perf_of(t), get=lambda d, k: d.get(k.split('.')[0], {}).get(k.split('.')[1]))
        if not t:
            return
        car = self.pack / 'CARS' / t['id']
        roles = (self.roles or {}).get(t['id'], {})
        role = lambda k: self.pack / roles[k] if roles.get(k) else None  # noqa: E731
        self.team_imgs[0].show(role('paint'))
        self.team_imgs[1].show(role('decals'), '' if roles.get('decals') else 'one-layer livery: no decal sheet')
        self.team_imgs[2].show(car / 'ui' / 'TeamLogo_Big.png')
        same = roles.get('paint_lod') and roles.get('paint_lod') == roles.get('paint')
        self.team_imgs[3].show(role('paint_lod'), 'the same file as the base paint' if same else '')
        self.team_imgs[4].show(role('decals_lod'))
        drv = car / 'driver'
        self.team_imgs[5].show(next(iter(sorted(drv.glob('InCarOveralls*.png'))), None) if drv.is_dir() else None)

    def show_rims(self):
        t = self.cur(self.rim_list, self.teams)
        if not t:
            return
        roles = (self.roles or {}).get(t['id'], {})

        def shared_rim(key, rel):
            names = [x.get('name') or x['id'] for x in self.teams if x is not t and rel and
                     ((self.roles or {}).get(x['id'], {}).get(key) == rel)]
            return 'Shared with: ' + ', '.join(names) if names else 'Used by this car only'
        for slot, key in zip(self.rim_imgs[:3], ('rim', 'rim_blurred', 'rim_normal')):
            rel = roles.get(key)
            slot.show(self.pack / rel if rel else None, shared_rim(key, rel) if rel else '')
        makers = season_pack.tyre_makers(self.pack)
        car = season_pack.car_of(self.ents or {'cars': []}, t['id'])
        # the classic cars' wheels are part of the car model: no tyre maker to choose
        maker = season_pack.tyre_maker_of(car, makers) if car and car.get('field65') else None
        self.maker_box['values'] = list(makers)
        self.maker_box.configure(state='readonly' if maker else 'disabled')
        self.maker_var.set(maker or '')
        # the sidewall set the car names now (the tyre maker can change after the template)
        side = [Path(f).relative_to(self.pack).as_posix() for f in season_pack.sidewall_files(self.pack, makers[maker])] \
            if maker in makers else roles.get('sidewall') or []
        users = [x.get('name') or x['id'] for x in self.teams if x is not t and maker and
                 season_pack.tyre_maker_of(season_pack.car_of(self.ents, x['id']) or {}, makers) == maker]
        tread = roles.get('tread') or []
        pick = lambda files, word: next((f for f in files if word in f.lower()), files[0] if files else None)  # noqa
        s0 = pick(side, 'blur0')
        self.rim_imgs[3].show(self.pack / s0 if s0 else None,
                              f'{len(side)} versions (Blur0-3); ' +
                              (f'Shared with: {", ".join(users)}' if users else 'Used by this car only') if side else '')
        for slot, word in ((self.rim_imgs[4], 'dry_blur0_ca'), (self.rim_imgs[5], 'wet_blur0_ca')):
            f = next((x for x in tread if word in x.lower()), None)
            slot.show(self.pack / f if f else None, 'shared by every car' if f else '')

    def set_maker(self):
        t = self.cur(self.rim_list, self.teams)
        car = season_pack.car_of(self.ents or {'cars': []}, t['id']) if t else None
        if car is None:
            return
        makers = season_pack.tyre_makers(self.pack)
        if self.maker_var.get() in makers and season_pack.tyre_maker_of(car, makers) != self.maker_var.get():
            season_pack.set_tyre_maker(car, makers, self.maker_var.get())
            self.log(f'{t.get("name") or t["id"]}: tyre maker {self.maker_var.get()} (Save and Build to apply)')
        self.show_rims()

    def new_maker(self):
        if not self.pack or self.kind != 'season':
            return
        name = self.ask_name('New tyre maker', 'Tyre maker name:')
        if not name:
            return
        like = self.maker_var.get() if self.maker_var.get() in season_pack.TYRE_MAKERS else 'Bridgestone'
        try:
            made = season_pack.new_tyre_maker(self.pack, name, like)
        except ValueError as e:
            messagebox.showerror('Content Manager', str(e))
            return
        self.log(f'tyre maker {name}: {len(made)} sidewall pictures copied from {like} into WHEELS/SideWalls '
                 f'({made[0].name} ... ; Blur1-3 are drawn as the wheels spin faster). Paint them, choose the maker '
                 f'for the teams, then Save and Build')
        self.maker_var.set(name)
        self.set_maker()

    def show_driver(self):
        self.store_forms()
        dr = self.cur(self.driver_list, self.drivers)
        self.shown_driver = dr
        self.driver_form.load(dr)
        if dr:
            base = self.pack / 'DRIVERS' / dr['id']
            self.driver_imgs[0].show(base / 'Helmet' / 'Colour.png')
            self.driver_imgs[1].show(base / 'ui' / 'PreRace.png')

    def open_sub(self, sub):
        t = getattr(self, 'shown_team', None)
        if t and (self.pack / 'CARS' / t['id'] / sub).is_dir():
            open_path(self.pack / 'CARS' / t['id'] / sub)

    def load_helmets(self):
        self.manifest, self.roles = self.load_json('template.json') or {}, {}
        for w in self.helmet_grid.winfo_children():
            w.destroy()
        self.helmet_slots = []
        dirs = sorted(p for p in self.pack.iterdir() if p.is_dir() and (p / 'Colour.png').exists())
        for i, d in enumerate(dirs):
            s = ImageSlot(self.helmet_grid, self, d.name)
            s.grid(row=i // 6, column=i % 6, padx=4, pady=4)
            s.show(d / 'Colour.png')
            self.helmet_slots.append(s)
        if not dirs:
            ttk.Label(self.helmet_grid, text='No helmet designs in this folder.').grid()

    # ------------------------------------------------------------------ saving
    def store_forms(self):
        """Form edits back into the loaded data (not to disk)."""
        if self.kind != 'season':
            return 0
        n = 0
        if getattr(self, 'shown_team', None) is not None:
            n += self.team_form.store()
            n += self.perf_form.store(get=lambda d, k: d.get(k.split('.')[0], {}).get(k.split('.')[1]),
                                      put=lambda d, k, v: d.setdefault(k.split('.')[0], {}).__setitem__(
                                          k.split('.')[1], v))
        if getattr(self, 'shown_driver', None) is not None:
            n += self.driver_form.store()
        return n

    def save(self):
        if self.kind != 'season' or not self.pack:
            return True
        try:
            self.store_forms()
        except ValueError as e:
            messagebox.showerror('Content Manager', str(e))
            return False
        db = self.pack / 'DATABASE'
        if self.ents is not None:
            (db / 'entities.json').write_text(json.dumps(self.ents, indent=1, ensure_ascii=False), encoding='utf-8',
                                              newline='\n')
        if self.dyn is not None:
            (db / 'dynamics.json').write_text(json.dumps(self.dyn, indent=1), encoding='utf-8', newline='\n')
        pts = self.points_var.get().replace(',', ' ').split()
        if pts:
            if not all(p.isdigit() for p in pts):
                messagebox.showerror('Content Manager', 'Points: whole numbers separated by spaces')
                return False
            db.mkdir(parents=True, exist_ok=True)
            (db / 'points.txt').write_text(season_pack.POINTS_TXT.rsplit('\n', 2)[0] + '\n' + ' '.join(pts) + '\n',
                                           encoding='utf-8', newline='\n')
        self.log(f'{self.pack.name}: saved')
        return True

    # ------------------------------------------------------------------ template / build (season_pack.py)
    def run_tool(self, args, done=None):
        if self.busy:
            messagebox.showinfo('Content Manager', 'Still working on the previous task.')
            return
        usr = self.usrdir()
        if not usr:
            messagebox.showerror('Content Manager', 'Choose the game folder (PS3_GAME, with USRDIR/offline.wad).')
            return
        self.save_settings()
        self.busy = True
        cmd = [sys.executable, '-I', '-u', str(TOOLS / 'season_pack.py'), args[0], str(usr)] + args[1:]
        self.bar.configure(mode='indeterminate')
        self.bar.start(15)
        self.status_var.set('Starting...')
        self.log('> ' + ' '.join(args))

        def work():
            try:
                p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding='utf-8', errors='replace',
                                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                for line in p.stdout:
                    self.q.put(('log', line.rstrip()))
                rc = p.wait()
            except OSError as e:
                self.q.put(('log', str(e)))
                rc = -1
            self.q.put(('done', (rc, done)))
        threading.Thread(target=work, daemon=True).start()

    def _poll(self):
        try:
            while True:
                what, val = self.q.get_nowait()
                if what == 'log' and val.startswith('PROGRESS '):
                    frac, _, text = val[9:].partition(' ')
                    a, _, b = frac.partition('/')
                    if self.bar['mode'] != 'determinate':
                        self.bar.stop()
                        self.bar.configure(mode='determinate')
                    self.bar['value'] = 1000 * int(a) / max(1, int(b))
                    self.status_var.set(f'{text} ({a}/{b})')
                elif what == 'log':
                    self.log(val)
                else:
                    rc, done = val
                    self.busy = False
                    self.bar.stop()
                    self.bar.configure(mode='determinate')
                    self.bar['value'] = 1000 if rc == 0 else 0
                    self.status_var.set('Done' if rc == 0 else f'Failed (exit code {rc})')
                    self.log('done' if rc == 0 else f'failed (exit code {rc})')
                    if rc == 0 and done:
                        done()
        except queue.Empty:
            pass
        self.after(100, self._poll)

    def ask_name(self, title, label='Folder name:'):
        win = tk.Toplevel(self)
        win.title(title)
        win.transient(self)
        v = tk.StringVar()
        ttk.Label(win, text=label, padding=8).pack(side='left')
        e = ttk.Entry(win, textvariable=v, width=30)
        e.pack(side='left', padx=4)
        out = []
        ok = lambda *_: (out.append(v.get().strip()), win.destroy())  # noqa: E731
        ttk.Button(win, text='Create', command=ok).pack(side='left', padx=8)
        e.bind('<Return>', ok)
        e.focus_set()
        win.grab_set()
        self.wait_window(win)
        name = out[0] if out else ''
        if name and any(c in name for c in '\\/:*?"<>|'):
            messagebox.showerror('Content Manager', 'Not a valid folder name')
            return ''
        return name

    def new_pack(self, kind, cmd, extra):
        name = self.ask_name('New season pack' if kind == 'season' else 'New player helmet pack')
        if not name:
            return
        dest = self.folders()[kind] / name
        if (dest / 'template.json').exists():
            messagebox.showerror('Content Manager', f'{dest} already exists')
            return
        dest.parent.mkdir(parents=True, exist_ok=True)

        def done():
            self.refresh()
            iid = f'{kind}|{dest}'
            if self.tree.exists(iid):
                self.tree.selection_set(iid)
        self.run_tool([cmd, str(dest)] + extra, done)

    def new_season_pack(self):
        self.new_pack('season', 'template', ['--models'] if self.models_var.get() else [])

    def new_helmet_pack(self):
        self.new_pack('helmet', 'helmets', [])

    def build(self):
        if not self.pack:
            return
        if not (self.pack / 'template.json').exists():
            messagebox.showerror('Content Manager', 'This pack has no template.json (not made by the Content '
                                                    'Manager / season_pack.py): nothing to build.')
            return
        if self.save():
            self.run_tool(['build', str(self.pack)])

    def open_pack(self):
        if self.pack:
            open_path(self.pack)

    def log(self, text):
        self.logbox.configure(state='normal')
        self.logbox.insert('end', text + '\n')
        self.logbox.see('end')
        self.logbox.configure(state='disabled')


def main(argv):
    app = App(argv[0] if argv else Path.cwd())
    if len(argv) > 1:   # a pack to open: its folder name (season packs first)
        for kind, d in app.folders().items():
            iid = f'{kind}|{d / argv[1]}'
            if app.tree.exists(iid):
                app.tree.selection_set(iid)
                break
    if len(argv) > 2:   # a tab, and a team of it (for tests and shortcuts)
        app.update()
        for tab in app.nb.tabs():
            if app.nb.tab(tab, 'text') == argv[2]:
                app.nb.select(tab)
        if len(argv) > 3:
            ids = [t['id'] for t in getattr(app, 'teams', [])]
            if argv[3] in ids:
                for lst, show in ((app.team_list, app.show_team), (app.rim_list, app.show_rims)):
                    lst.selection_clear(0, 'end')
                    lst.selection_set(ids.index(argv[3]))
                    show()
    app.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
