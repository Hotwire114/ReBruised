"""Black & Bruised launcher: PC settings (settings.ini in the project root) and Play.

    BlackAndBruised-Launcher.cmd   (python\\pythonw.exe -I tools\\launcher.pyw)

settings.ini is plain "key = value" text read by the game itself (port/main.cpp loadSettings), so the launcher is
optional: Play-BlackAndBruised.cmd uses the same file. The header shows docs/screens/18-title-logo.png when it exists
(a capture of the player's own game, local only); otherwise a drawn title.
"""
import os
import subprocess
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / "settings.ini"
EXE = ROOT / "build" / "program" / "bin" / "BlackAndBruised.exe"
ELF = ROOT / "game" / "disc" / "SLES_516.20"
ISO = ROOT / "game" / "BnB.iso"
BANNER = ROOT / "docs" / "screens" / "18-title-logo.png"

# ---- settings model (key: label, [(value, shown text)], default) -----------------------------------------------------
CHOICES = {
    "framerate": ("Frame rate", [("60", "60 fps"), ("50", "50 fps (PAL)")], "60"),
    "display": ("Display", [("windowed", "Windowed"), ("borderless", "Borderless"), ("exclusive", "Fullscreen")],
                "windowed"),
    "widescreen": ("Aspect ratio", [("off", "4:3"), ("on", "16:9")], "off"),
    "resolution": ("Resolution", [("auto", "Auto (fits your monitor)"), ("native", "Native 448p (PS2)"),
                                  ("720p", "720p"), ("1080p", "1080p"), ("1440p", "1440p"), ("4k", "4K"),
                                  ("lowest", "Lowest (slow PCs)")], "auto"),
    "antialiasing": ("Anti-aliasing", [("fxaa", "FXAA"), ("off", "Off"), ("ssaa2", "SSAA 2x"),
                                       ("ssaa2+fxaa", "SSAA 2x + FXAA")], "fxaa"),
    "anisotropic": ("Texture filtering", [("off", "Off"), ("2", "2x anisotropic"), ("4", "4x anisotropic"),
                                          ("8", "8x anisotropic"), ("16", "16x anisotropic")], "4"),
    "scaling": ("Window scaling", [("area", "Smooth"), ("sharp", "Sharp"), ("bilinear", "Bilinear"),
                                   ("nearest", "Pixelated")], "area"),
    "hud": ("HUD", [("hd", "HD"), ("soft", "Soft"), ("sharp", "Sharp")], "hd"),
}
SEGMENTED = ("framerate", "display", "widescreen", "hud")   # short choices: buttons; the rest: dropdowns
HINTS = {
    "framerate": "60 runs the game's own 60 Hz timing (as in the NTSC release); 50 is the original PAL speed.",
    "widescreen": "16:9 widens the view in fights; menus and movies stay 4:3. F9 switches while playing.",
    "display": "F11 or Alt+Enter switches between window and borderless fullscreen while playing.",
}
TOGGLES = {"sound": ("Sound", "on"), "show_fps": ("FPS in log", "off"), "debug_panel": ("Debug panel", "off")}
OTHER_DEFAULTS = {"movie_timeout": "0"}
ORDER = ["framerate", "display", "widescreen", "resolution", "antialiasing", "anisotropic", "scaling", "hud",
         "sound", "movie_timeout", "debug_panel", "show_fps"]


def read_settings():
    values = {k: v[2] for k, v in CHOICES.items()}
    values.update({k: v[1] for k, v in TOGGLES.items()})
    values.update(OTHER_DEFAULTS)
    lines = SETTINGS.read_text(encoding="utf-8-sig").splitlines() if SETTINGS.is_file() else []
    for line in lines:
        line = line.split("#", 1)[0]
        if "=" in line:
            k, v = line.split("=", 1)
            values[k.strip().lower()] = v.strip().lower()
    return values, lines


def write_settings(values, old_lines):
    """Keep the file's comments and order; replace values, append keys it does not have yet."""
    out, seen = [], set()
    for line in old_lines:
        body = line.split("#", 1)[0]
        if "=" in body:
            k = body.split("=", 1)[0].strip().lower()
            if k in values:
                out.append(f"{k} = {values[k]}")
                seen.add(k)
                continue
        out.append(line)
    if not old_lines:
        out.append("# Black & Bruised - PC settings (written by the launcher; hand-editable)")
    for k in ORDER:
        if k not in seen and k in values:
            out.append(f"{k} = {values[k]}")
    SETTINGS.write_text("\n".join(out) + "\n", encoding="utf-8")


# ---- look ------------------------------------------------------------------------------------------------------------
BG = "#0b0d17"          # window
PANEL = "#141829"       # cards
PANEL_LINE = "#232946"
FIELD = "#1c2138"       # inputs, unselected segments
FIELD_HOVER = "#262c4a"
TEXT = "#eef0f7"
MUTED = "#8a91ad"
ORANGE = "#ff8a1f"      # the logo's orange
ORANGE_HOT = "#ffa64d"
YELLOW = "#ffc83d"
GOOD = "#5bd68a"
BAD = "#ff5a5a"


def pick_font(root, *names):
    available = set(root.tk.call("font", "families"))
    for n in names:
        if n in available:
            return n
    return names[-1]


class Segmented(tk.Frame):
    """Row of buttons, one selected (radio group)."""

    def __init__(self, master, options, value, font, on_change=None):
        super().__init__(master, bg=PANEL)
        self.options, self.value, self.on_change = options, value, on_change
        self.buttons = {}
        for i, (v, text) in enumerate(options):
            b = tk.Label(self, text=text, font=font, padx=14, pady=6, cursor="hand2", bd=0)
            b.grid(row=0, column=i, padx=(0 if i == 0 else 2, 0))
            b.bind("<Button-1>", lambda _e, v=v: self.set(v, notify=True))
            b.bind("<Enter>", lambda _e, v=v: self._paint(hover=v))
            b.bind("<Leave>", lambda _e: self._paint())
            self.buttons[v] = b
        self._paint()

    def set(self, v, notify=False):
        self.value = v
        self._paint()
        if notify and self.on_change:
            self.on_change(v)

    def get(self):
        return self.value

    def _paint(self, hover=None):
        for v, b in self.buttons.items():
            if v == self.value:
                b.configure(bg=ORANGE, fg="#1a0f00")
            else:
                b.configure(bg=FIELD_HOVER if v == hover else FIELD, fg=TEXT)


class Switch(tk.Canvas):
    """On/off toggle."""

    W, H = 44, 24

    def __init__(self, master, on):
        super().__init__(master, width=self.W, height=self.H, bg=PANEL, highlightthickness=0, cursor="hand2")
        self.on = on
        self.bind("<Button-1>", lambda _e: self.toggle())
        self._paint()

    def toggle(self):
        self.on = not self.on
        self._paint()

    def set(self, on):
        self.on = on
        self._paint()

    def _paint(self):
        self.delete("all")
        w, h, r = self.W, self.H, self.H // 2
        fill = ORANGE if self.on else FIELD_HOVER
        self.create_oval(0, 0, h, h, fill=fill, outline=fill)
        self.create_oval(w - h, 0, w, h, fill=fill, outline=fill)
        self.create_rectangle(r, 0, w - r, h, fill=fill, outline=fill)
        x = w - r if self.on else r
        self.create_oval(x - r + 3, 3, x + r - 3, h - 3, fill=TEXT if self.on else MUTED, outline="")


class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Black & Bruised")
        self.configure(bg=BG)
        self.resizable(False, False)
        self.values, self.lines = read_settings()
        head = pick_font(self, "Impact", "Segoe UI Black", "Arial Black", "Arial")
        body = pick_font(self, "Segoe UI", "Arial")
        self.f_title = (head, 30)
        self.f_section = (body, 10, "bold")
        self.f_label = (body, 10)
        self.f_small = (body, 9)
        self.f_play = (head, 22)

        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("Dark.TCombobox", fieldbackground=FIELD, background=FIELD, foreground=TEXT, arrowcolor=ORANGE,
                        bordercolor=PANEL_LINE, lightcolor=FIELD, darkcolor=FIELD, padding=5)
        style.map("Dark.TCombobox", fieldbackground=[("readonly", FIELD)], foreground=[("readonly", TEXT)],
                  selectbackground=[("readonly", FIELD)], selectforeground=[("readonly", TEXT)],
                  background=[("active", FIELD_HOVER)])
        self.option_add("*TCombobox*Listbox.background", FIELD)
        self.option_add("*TCombobox*Listbox.foreground", TEXT)
        self.option_add("*TCombobox*Listbox.selectBackground", ORANGE)
        self.option_add("*TCombobox*Listbox.selectForeground", "#1a0f00")
        self.option_add("*TCombobox*Listbox.font", self.f_label)

        self._banner()
        cards = tk.Frame(self, bg=BG)
        cards.pack(fill="x", padx=18, pady=(14, 0))
        game = self._card(cards, "GAME")
        game.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        gfx = self._card(cards, "PICTURE")
        gfx.grid(row=0, column=1, sticky="nsew", padx=(8, 0))
        cards.columnconfigure(0, weight=1)
        cards.columnconfigure(1, weight=1)

        self.widgets = {}
        self.hint = tk.Label(self, text="", bg=BG, fg=MUTED, font=self.f_small, anchor="w", justify="left")
        for key in ("framerate", "display", "widescreen"):
            self._choice(game, key)
        for key, (label, _default) in TOGGLES.items():
            self._toggle(game, key, label)
        for key in ("resolution", "antialiasing", "anisotropic", "scaling", "hud"):
            self._choice(gfx, key)

        self.hint.pack(fill="x", padx=22, pady=(10, 0))
        self._footer()
        self.bind("<Return>", lambda _e: self.play())
        self.bind("<Escape>", lambda _e: self.destroy())
        self.update_idletasks()
        x = (self.winfo_screenwidth() - self.winfo_width()) // 2
        y = max(0, (self.winfo_screenheight() - self.winfo_height()) // 3)
        self.geometry(f"+{x}+{y}")

    # ---- layout pieces -------------------------------------------------------------------------------------------
    def _banner(self):
        w, h = 760, 230
        c = tk.Canvas(self, width=w, height=h, bg="#000000", highlightthickness=0)
        c.pack()
        shown = False
        if BANNER.is_file():
            try:
                img = tk.PhotoImage(file=str(BANNER))          # 640x448 title capture, logo centred at about y 220
                scaled = img.zoom(2).subsample(3)              # 2/3: the whole logo fits the banner
                self.banner_img = scaled
                c.create_image(w // 2, (h - 40) // 2 - 147, image=scaled, anchor="n")   # logo centre at mid-banner
                self.icon_img = img.subsample(6)
                self.iconphoto(True, self.icon_img)
                shown = True
            except tk.TclError:
                shown = False
        if not shown:
            for i in range(h):                                # dark gradient with a drawn title
                t = i / h
                col = "#%02x%02x%02x" % (int(11 + 20 * t), int(13 + 10 * t), int(23 + 30 * t))
                c.create_line(0, i, w, i, fill=col)
            for dx, dy in ((3, 3), (-2, -2), (2, -2), (-2, 2)):
                c.create_text(w // 2 + dx, 88 + dy, text="BLACK & BRUISED", font=(self.f_title[0], 46), fill="#000")
            c.create_text(w // 2, 88, text="BLACK & BRUISED", font=(self.f_title[0], 46), fill=ORANGE)
        # fade into the window colour + caption
        for i in range(40):
            t = i / 39
            c.create_line(0, h - 40 + i, w, h - 40 + i, fill="#%02x%02x%02x" % (int(11 * t), int(13 * t), int(23 * t)))
        c.create_text(20, h - 16, anchor="w", text="NATIVE PC RECOMPILATION", font=self.f_section, fill=YELLOW)
        c.create_text(w - 20, h - 16, anchor="e", text="PS2  ·  SLES-51620", font=self.f_small, fill=MUTED)

    def _card(self, master, title):
        outer = tk.Frame(master, bg=PANEL_LINE)
        inner = tk.Frame(outer, bg=PANEL, padx=16, pady=12)
        inner.pack(fill="both", expand=True, padx=1, pady=1)
        tk.Label(inner, text=title, bg=PANEL, fg=ORANGE, font=self.f_section).pack(anchor="w", pady=(0, 6))
        outer.inner = inner
        return outer

    def _row(self, card, label):
        row = tk.Frame(card.inner, bg=PANEL)
        row.pack(fill="x", pady=5)
        tk.Label(row, text=label, bg=PANEL, fg=TEXT, font=self.f_label, width=15, anchor="w").pack(side="left")
        return row

    def _choice(self, card, key):
        label, options, default = CHOICES[key]
        row = self._row(card, label)
        current = self.values.get(key, default)
        if key in SEGMENTED:
            w = Segmented(row, options, current if current in [v for v, _ in options] else default, self.f_small,
                          on_change=lambda _v, k=key: self._show_hint(k))
            w.pack(side="left")
        else:
            texts = [t for _v, t in options]
            w = ttk.Combobox(row, values=texts, state="readonly", style="Dark.TCombobox", width=24, font=self.f_label)
            match = [t for v, t in options if v == current]
            w.set(match[0] if match else texts[0])
            w.pack(side="left")
            w.bind("<<ComboboxSelected>>", lambda _e, k=key: (self._show_hint(k), self.focus()))
        for ev in ("<Enter>",):
            row.bind(ev, lambda _e, k=key: self._show_hint(k))
        self.widgets[key] = w

    def _toggle(self, card, key, label):
        row = self._row(card, label)
        s = Switch(row, self.values.get(key, "off") in ("1", "on", "true", "yes"))
        s.pack(side="left")
        self.widgets[key] = s

    def _show_hint(self, key):
        self.hint.configure(text=HINTS.get(key, ""))

    def _footer(self):
        bar = tk.Frame(self, bg=BG)
        bar.pack(fill="x", padx=18, pady=(12, 16))
        ready = EXE.is_file() and ELF.is_file()
        status = ("●  Ready" if ready else "●  Game not built yet (tools\\build_program.ps1)")
        info = tk.Frame(bar, bg=BG)
        info.pack(side="left", fill="y")
        tk.Label(info, text=status, bg=BG, fg=GOOD if ready else BAD, font=self.f_label).pack(anchor="w")
        tk.Label(info, text="Pad: any XInput controller   ·   Keys: WASD, X/Space = Cross, Enter = Start",
                 bg=BG, fg=MUTED, font=self.f_small).pack(anchor="w")
        self.status = tk.Label(info, text="", bg=BG, fg=MUTED, font=self.f_small)
        self.status.pack(anchor="w")

        play = tk.Label(bar, text="PLAY", font=self.f_play, bg=ORANGE, fg="#1a0f00", padx=44, pady=6, cursor="hand2")
        play.pack(side="right")
        play.bind("<Enter>", lambda _e: play.configure(bg=ORANGE_HOT))
        play.bind("<Leave>", lambda _e: play.configure(bg=ORANGE))
        play.bind("<Button-1>", lambda _e: self.play())
        for text, cmd in (("Save", self.save), ("Defaults", self.defaults)):
            b = tk.Label(bar, text=text, font=self.f_label, bg=FIELD, fg=TEXT, padx=14, pady=9, cursor="hand2")
            b.pack(side="right", padx=(0, 8))
            b.bind("<Enter>", lambda _e, b=b: b.configure(bg=FIELD_HOVER))
            b.bind("<Leave>", lambda _e, b=b: b.configure(bg=FIELD))
            b.bind("<Button-1>", lambda _e, c=cmd: c())

    # ---- actions ---------------------------------------------------------------------------------------------------
    def collect(self):
        values = dict(self.values)
        for key, w in self.widgets.items():
            if key in TOGGLES:
                values[key] = "on" if w.on else "off"
            elif isinstance(w, Segmented):
                values[key] = w.get()
            else:
                options = CHOICES[key][1]
                values[key] = next((v for v, t in options if t == w.get()), options[0][0])
        return values

    def defaults(self):
        for key, w in self.widgets.items():
            if key in TOGGLES:
                w.set(TOGGLES[key][1] == "on")
            elif isinstance(w, Segmented):
                w.set(CHOICES[key][2])
            else:
                options = CHOICES[key][1]
                w.set(next(t for v, t in options if v == CHOICES[key][2]))
        self.status.configure(text="Defaults set (not saved yet).")

    def save(self):
        self.values = self.collect()
        write_settings(self.values, self.lines)
        self.values, self.lines = read_settings()
        self.status.configure(text="Saved.")

    def play(self):
        self.save()
        missing = [p for p in (EXE, ELF) if not p.is_file()]
        if missing:
            messagebox.showerror("Black & Bruised", "Missing:\n" + "\n".join(str(p) for p in missing))
            return
        (ROOT / "logs").mkdir(exist_ok=True)
        log = open(ROOT / "logs" / "play.log", "wb")
        err = open(ROOT / "logs" / "play.err", "wb")
        subprocess.Popen([str(EXE), str(ELF), str(ISO)], cwd=str(ROOT), env=dict(os.environ), stdout=log,
                         stderr=err, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.destroy()


if __name__ == "__main__":
    Launcher().mainloop()
