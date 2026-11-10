"""The builder's window (tkinter: part of Python, nothing to install)."""
import os
import queue
import threading
import traceback
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import __version__, pipeline
from .discs import Disc, DiscError

DUMPS = [('Disc images', '*.cue *.ccd *.img *.bin *.iso *.CUE *.CCD *.IMG *.BIN *.ISO'), ('All files', '*')]


class App:
    def __init__(self, root):
        self.root = root
        self.discs = {}
        self.events = queue.Queue()
        self.busy = False
        root.title(f'FF9 3DS Builder {__version__}')
        root.minsize(640, 520)
        f = ttk.Frame(root, padding=12)
        f.pack(fill='both', expand=True)
        f.columnconfigure(1, weight=1)

        ttk.Label(f, text='Point this at your four Final Fantasy IX (NTSC-U) discs: BIN/CUE, IMG/CCD or raw ISO.',
                  wraplength=600).grid(row=0, column=0, columnspan=3, sticky='w')
        ttk.Button(f, text='Add discs…', command=self.add_many).grid(row=1, column=0, columnspan=3, sticky='w',
                                                                     pady=(8, 4))
        self.rows = {}
        for n in range(1, 5):
            ttk.Label(f, text=f'Disc {n}').grid(row=1 + n, column=0, sticky='w', padx=(0, 8))
            var = tk.StringVar(value='(none)')
            ttk.Label(f, textvariable=var, foreground='grey').grid(row=1 + n, column=1, sticky='we')
            ttk.Button(f, text='Browse…', command=lambda n=n: self.add_one(n)).grid(row=1 + n, column=2, pady=2)
            self.rows[n] = var

        ttk.Separator(f).grid(row=6, column=0, columnspan=3, sticky='we', pady=10)
        ttk.Label(f, text='Save to').grid(row=7, column=0, sticky='w')
        self.out = tk.StringVar(value=os.path.expanduser('~'))
        ttk.Entry(f, textvariable=self.out).grid(row=7, column=1, sticky='we')
        ttk.Button(f, text='Browse…', command=self.pick_out).grid(row=7, column=2)
        opts = ttk.Frame(f)
        opts.grid(row=8, column=0, columnspan=3, sticky='w', pady=6)
        self.format = tk.StringVar(value='cia')
        ttk.Radiobutton(opts, text='ff9-full.cia (install with FBI)', value='cia', variable=self.format).pack(side='left')
        ttk.Radiobutton(opts, text='ff9-full.3dsx (Homebrew Launcher)', value='3dsx',
                        variable=self.format).pack(side='left', padx=12)

        self.go = ttk.Button(f, text='Build', command=self.start)
        self.go.grid(row=9, column=0, columnspan=3, sticky='we', pady=(4, 6))
        self.bar = ttk.Progressbar(f, maximum=1000)
        self.bar.grid(row=10, column=0, columnspan=3, sticky='we')
        self.status = tk.StringVar(value='Add the four discs to begin.')
        ttk.Label(f, textvariable=self.status).grid(row=11, column=0, columnspan=3, sticky='w', pady=(2, 6))
        self.log = tk.Text(f, height=10, wrap='word', state='disabled')
        self.log.grid(row=12, column=0, columnspan=3, sticky='nsew')
        f.rowconfigure(12, weight=1)
        ttk.Label(f, text='For your own discs and console only: the files made hold the game\'s data; never share them.',
                  foreground='grey', wraplength=600).grid(row=13, column=0, columnspan=3, sticky='w', pady=(6, 0))
        for name in ('ff9.3dsx', 'ff9.cia'):
            if not pipeline.payload(name):
                self.write(f'Note: this builder has no prebuilt {name}.')
        root.after(100, self.poll)

    # --- discs ---
    def add_paths(self, paths, slot=None):
        for p in paths:
            try:
                d = Disc(p)
            except DiscError as e:
                messagebox.showerror('Not usable', str(e))
                continue
            except OSError as e:
                messagebox.showerror('Can\'t read', f'{p}: {e}')
                continue
            if slot is not None and d.number != slot:
                if not messagebox.askyesno('Different disc', f'{os.path.basename(p)} is disc {d.number}, not disc '
                                           f'{slot}. Use it as disc {d.number}?'):
                    d.close()
                    continue
            old = self.discs.pop(d.number, None)
            if old:
                old.close()
            self.discs[d.number] = d
            self.rows[d.number].set(f'{d.source}  ({d.boot})')
        missing = [str(n) for n in range(1, 5) if n not in self.discs]
        self.status.set('Ready to build.' if not missing else f'Still needed: disc {", ".join(missing)}.')

    def add_many(self):
        self.add_paths(filedialog.askopenfilenames(title='Your FF9 discs', filetypes=DUMPS))

    def add_one(self, n):
        p = filedialog.askopenfilename(title=f'Disc {n}', filetypes=DUMPS)
        if p:
            self.add_paths([p], n)

    def pick_out(self):
        p = filedialog.askdirectory(title='Save to', initialdir=self.out.get())
        if p:
            self.out.set(p)

    # --- build ---
    def write(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def start(self):
        if self.busy:
            return
        if len(self.discs) < 4:
            messagebox.showinfo('Discs', 'Add all four discs first.')
            return
        out = self.out.get()
        self.busy = True
        self.go.configure(state='disabled')
        fmt = self.format.get()
        args = (dict(self.discs), out, fmt == '3dsx', fmt == 'cia')
        threading.Thread(target=self.run, args=args, daemon=True).start()

    def run(self, discs, out, a, c):
        try:
            outs = pipeline.build(discs, out, a, c, log=lambda m: self.events.put(('log', m)),
                                  report=lambda f, m=None: self.events.put(('progress', f, m)))
            self.events.put(('done', outs))
        except DiscError as e:
            self.events.put(('error', str(e)))
        except Exception as e:  # (anything else: show it rather than die silently)
            self.events.put(('error', f'{e}\n\n{traceback.format_exc()}'))

    def poll(self):
        try:
            while True:
                ev = self.events.get_nowait()
                if ev[0] == 'log':
                    self.write(ev[1])
                elif ev[0] == 'progress':
                    self.bar['value'] = int(ev[1] * 1000)
                    if ev[2]:
                        self.status.set(ev[2])
                elif ev[0] == 'done':
                    self.busy = False
                    self.go.configure(state='normal')
                    self.status.set('Done.')
                    made = ev[1][0]
                    how = ('Copy it to the SD card and install it with FBI.' if made.endswith('.cia') else
                           'Copy it to /3ds/ on the SD card and start it from the Homebrew Launcher.')
                    messagebox.showinfo('Done', f'Made:\n{made}\n\n{how}')
                elif ev[0] == 'error':
                    self.busy = False
                    self.go.configure(state='normal')
                    self.status.set('Stopped.')
                    self.write('Error: ' + ev[1].split('\n')[0])
                    messagebox.showerror('Build stopped', ev[1])
        except queue.Empty:
            pass
        self.root.after(100, self.poll)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()
