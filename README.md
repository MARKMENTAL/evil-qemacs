# Evil QEmacs (`eqe`)

A delightfully unhinged fork of Fabrice Bellard's QEmacs that hacks a native Vim modal state machine directly into a sub-megabyte C codebase.

I don't expect anybody to actually use this. I made it because QEmacs is impossibly tiny, fast, and has incredible zero-copy buffer handling, but my muscle memory is 100% hardwired to modern Vim. Instead of doing the sane thing and using standard tools, I spent an afternoon along with various LLM models jamming a modal text editing engine into QEmacs' internal key dispatch loops.

The implementation is messy, full of hacky C pointer arithmetic, and probably offends both Emacs purists and Vim devotees alike, but whatever, I like it (: 

**This is an alpha, in progress.** It compiles in seconds, boots in under 3ms, uses a couple of MB of RAM, and it just works, it lets you run a awesome hybrid vim/emacs setup on a potato — but it's also a moving target, and the roadmap below is honest about what's missing.

---

### What Actually Works

* **Modal state machine:** Normal, Insert, Visual, and Visual Line modes wired directly into QEmacs window contexts, with live selection highlighting.
* **Motions:** `h`/`j`/`k`/`l`, `w`/`b`, `0`/`^`/`$`, `G`, `gg` — plus `ESC`-cancellation of pending commands, because typing `d:q!` should do the right thing.
* **Operators:** `x`/`xx`, `dd`, and the full Visual trio: `d`, `y`, `x` over any selection. `u` undoes.
* **Registers with vim semantics:** `p` knows charwise from linewise. `dd` followed by `p` puts the line back below the cursor, `dd j p` appends it after the next line, and a charwise yank pastes after the cursor character, landing on the last pasted char. Visual `p` replaces the selection without clobbering the register being pasted.
* **Search:** `/` forward, `?` backward, `n`/`N` to repeat, with vim-style `wrapscan` so searches wrap around the buffer instead of silently giving up.
* **Substitute:** `:%s/old/new/[g]` and `:s/old/new/`, including patterns and replacements that contain spaces.
* **Indenting:** `>>` and `<<` with `:set sw=N`.
* **Window chords:** `C-w w`/`C-w C-w` cycle focus, `C-w h/j/k/l` jump directionally — vim-style, with `C-w ESC` cancelling the chord. They work from text buffers *and* from the Dired pane (which isn't vi-modal; the binding table covers it), so `C-x C-d` followed by `C-w w` ping-pongs focus between directory and buffer.
* **Ex command line:** `:w [file]`, `:q`, `:q!`, `:wq`, `:x`, `:<N>` (goto line), `:set sw=N`, `:%s/.../.../[g]`.
* **Terminal theme auto-detect:** queries your terminal's foreground/background via OSC 10/11 at startup and adopts the palette, falling back to peach-on-black if the terminal doesn't answer.

### Not Yet (the roadmap)

* Numeric counts (`5dd`, `3w`), the `e` motion, the `c` operator, operator+motion combos (`dw`, `d$`), and text objects.
* `:e`, splits (`:sp`/`:vsp`), buffer cycling.
* Vi navigation in hex mode (wandering firmware images with `hjkl` and `r`-replacing bytes is the dream).
* Vim-aligned `w`/`b` word semantics (currently `w` stops at the end of the word, not the start of the next one).
* Visual block (`Ctrl+v`).

---

### The Numbers

Measured on x86-64 (Debian, gcc-14/clang-19, this repo's default `clang` config). Your mileage may vary, but probably not by much:

| | `teqe` (tiny) | `eqe` (full) |
|---|---|---|
| binary size, x86-64 stripped | 319 KB | 1.38 MB |
| binary size, ARM static (Miyoo) | 838 KB | 1.55 MB |
| boot to first frame (pty, median of 15) | 1.6 ms | 2.0 ms |
| resident RAM while idle (VmRSS) | 2.2 MB | 3.3 MB |
| peak memory (VmPeak) | 3.8 MB | 4.9 MB |
| clean compile | 2.7 s | 12.5 s |

---

### Native Build (x86_64 / amd64)

Requires nothing but a standard C toolchain and libc:

```bash
./configure
make
sudo make install
```

This builds two terminal binaries:

* **`eqe`** — the full build (all modes, scripting, languages, docs).
* **`teqe`** — the tiny build (`CONFIG_TINY`, `-Os`): the one that fits on a handheld.

Old muscle memory still works: `make qe` and `make tqe` are aliases for `eqe` and `teqe`. `make install` installs the binary as `qemacs` with `eqe` and `qe` symlinks.

---

### Miyoo Mini Plus (armv7l)

The whole point. The Miyoo ships an old glibc, so the binaries are cross-compiled **statically linked** — one file, no libc drama on device:

```bash
./armv7l-build.sh teqe    # tiny build (838 KB static ARM)
./armv7l-build.sh eqe     # full build (1.55 MB static ARM)
```

Requires the `gcc-arm-linux-gnueabihf` cross toolchain (`apt install gcc-arm-linux-gnueabihf`).

You will see linker warnings like:

```
warning: Using 'getpwent' in statically linked applications requires
at runtime the shared libraries from the glibc version used for linking
```

These come from glibc's NSS-based user lookups (`getpwent`/`getpwnam` in the home-directory code) and are benign for this use — the binary runs fine on the device. Ignore them.

Then copy the binary over and run it (`scp teqe mini:/media/sdcard/`, or however you shuffle files onto yours). Vi mode is on by default: normal mode at startup, `i` to insert, `ESC` to get back.

---

### Testing

There's a real test suite, because "trust me bro it works on my machines" is not a regression strategy:

```bash
make test
```

61 pty-driven tests drive the actual binary through a virtual terminal — motions, pending commands, search wrap, substitute, visual yank/delete/put, register types, ex commands, even status-line contents. The harness answers the terminal's OSC color queries and reconstructs the screen, so tests are deterministic. See [tests/test_vi.py](tests/test_vi.py) for the catalog (`--list`, name/category filters, `--binary`).

---

### Heritage

Everything upstream QEmacs does still works underneath: huge-file editing via mmap, full Unicode with bidi and Indic/Arabic scripts, in-place hex editing, shell/compile mode with a real VT100 terminal, X11 build (`xqe`), and more. Read the upstream manual at [qe-doc.html](qe-doc.html) for the Emacs half of the family tree.

QEmacs is MIT licensed — see the [LICENSE](LICENSE) file — and was started in 2000 by Fabrice Bellard and Charlie Gordon. All the modal crimes here are mine. Upstream lives at [github.com/qemacs/qemacs](https://github.com/qemacs/qemacs).

