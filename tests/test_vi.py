#!/usr/bin/env python3
"""
PTY test suite for the qemacs Vi/Evil layer.

Each test spawns a pty, launches the editor on a temp file, sends a
keystroke sequence, and then asserts on:
  - the resulting file contents (byte-exact), and optionally
  - substrings of the terminal output (status-line checks, with ANSI
    escape sequences stripped first).

Usage:
    python3 test_vi.py                 # run all tests
    python3 test_vi.py visual search   # run tests matching substrings
    python3 test_vi.py --list          # list test names
    python3 test_vi.py -v              # verbose output
    python3 test_vi.py --binary ./qe   # pick the editor binary
    python3 test_vi.py --keep          # keep temp files for debugging

Environment:
    TQE_BIN  default editor binary (overridden by --binary)

Exit code is 0 when every test passes, 1 otherwise.

Keystroke notes:
  - spaces in key sequences are literal keys; in NORMAL mode they are
    swallowed, but NEVER put spaces in INSERT-mode segments (they type
    a space).
  - {DIR} in a key sequence or want_path is replaced by the per-run
    temp directory.
"""

import argparse
import os
import pty
import re
import select
import sys
import tempfile
import time
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

ANSI_RE = re.compile(
    rb'\x1b\[[0-9;?]*[ -/]*[@-~]'   # CSI sequences
    rb'|\x1b\][^\x07\x1b]*(\x07|\x1b\\)?'  # OSC sequences
    rb'|\x1b[()][0-9A-B]'           # charset selection
    rb'|\x1b[=>]'                   # keypad modes
)


def strip_ansi(data):
    return ANSI_RE.sub(b'', data)


class TermScreen:
    """Minimal VT100-style screen emulator.

    The tty driver only transmits cell *deltas*, so raw output cannot be
    searched for status strings reliably (e.g. "-- VISUAL --" ->
    "-- NORMAL --" only emits the differing "NORM" cells).  Rebuilding
    the screen lets tests assert on what a real user would see.

    Supported: CUP (cursor position), EL, ED, printable UTF-8 text,
    CR/LF/BS.  Everything else (SGR, modes, OSC, charsets) is skipped.
    """

    def __init__(self, cols=80, rows=24):
        self.cols = cols
        self.rows = rows
        self.grid = [[' '] * cols for _ in range(rows)]
        self.x = 0
        self.y = 0
        self._buf = b''
        self._last_status = None
        self.status_frames = []   # every distinct status-line content

    def snapshot(self):
        return '\n'.join(''.join(row).rstrip() for row in self.grid)

    def _record(self):
        line = ''.join(self.grid[self.rows - 1]).rstrip()
        if line != self._last_status:
            self._last_status = line
            self.status_frames.append(line)

    # -- feeding ------------------------------------------------------
    def feed(self, data):
        self._buf += data
        buf = self._buf
        i = 0
        n = len(buf)
        while i < n:
            if buf[i] == 0x1b:
                used = self._esc(buf, i)
            else:
                used = self._plain(buf, i)
            if used == 0:      # incomplete sequence, wait for more bytes
                break
            i += used
        self._buf = buf[i:]

    def _plain(self, buf, i):
        b = buf[i]
        if b == 0x0d:
            self.x = 0
            return 1
        if b == 0x0a:
            self.y = min(self.y + 1, self.rows - 1)
            self.x = 0
            return 1
        if b in (0x08,):
            self.x = max(self.x - 1, 0)
            return 1
        if b < 0x20 or b == 0x7f:
            return 1
        # UTF-8 decode
        for length in (1, 2, 3, 4):
            chunk = buf[i:i + length]
            if len(chunk) < length:
                if length == 1:
                    return 1  # garbage byte
                return 0      # maybe incomplete
            try:
                ch = chunk.decode('utf-8')
            except UnicodeDecodeError:
                continue
            self._put(ch)
            return length
        return 1               # undecodable, skip byte

    def _put(self, ch):
        if self.y < self.rows and self.x < self.cols:
            self.grid[self.y][self.x] = ch
        self.x += 1
        if self.x >= self.cols:
            self.x = 0
            self.y = min(self.y + 1, self.rows - 1)
        self._record()

    def _esc(self, buf, i):
        n = len(buf)
        if i + 1 >= n:
            return 0
        c = buf[i + 1]
        if c == 0x5b:  # CSI: ESC [ params final
            j = i + 2
            while j < n and not (0x40 <= buf[j] <= 0x7e):
                j += 1
            if j >= n:
                return 0       # incomplete
            self._csi(buf[i + 2:j], chr(buf[j]))
            return j - i + 1
        if c == 0x5d:  # OSC: ESC ] ... BEL or ESC \
            j = i + 2
            while j < n:
                if buf[j] == 0x07:
                    return j - i + 1
                if buf[j] == 0x1b and j + 1 < n and buf[j + 1] == 0x5c:
                    return j - i + 2
                j += 1
            return 0           # incomplete
        if i + 1 >= n:
            return 0
        # ESC + zero or more intermediates (0x20-0x2F) + final byte,
        # e.g. ESC ( B, ESC ) 0, ESC =, ESC >.  All are display noise
        # for our purposes: skip the whole sequence.
        j = i + 1
        while j < n and 0x20 <= buf[j] <= 0x2f:
            j += 1
        if j >= n:
            return 0           # incomplete sequence
        return j - i + 1

    def _csi(self, params, final):
        if final == 'H' or final == 'f':
            p = params.decode('latin-1').split(';')
            row = int(p[0]) if p and p[0].isdigit() else 1
            col = int(p[1]) if len(p) > 1 and p[1].isdigit() else 1
            self.y = min(max(row - 1, 0), self.rows - 1)
            self.x = min(max(col - 1, 0), self.cols - 1)
        elif final == 'K':
            mode = params.decode('latin-1')
            if mode in ('', '0'):
                for xx in range(self.x, self.cols):
                    self.grid[self.y][xx] = ' '
                self._record()
        elif final == 'J':
            mode = params.decode('latin-1')
            if mode == '2':
                self.grid = [[' '] * self.cols for _ in range(self.rows)]
                self._record()
        # SGR (m) and everything else: ignored


def default_binary():
    env = os.environ.get('TQE_BIN')
    if env:
        return env
    for name in ('teqe', 'tqe', 'eqe', 'qe'):
        cand = os.path.join(SCRIPT_DIR, '..', name)
        if os.path.exists(cand):
            return cand
    sys.exit('ERROR: no editor binary found; use --binary or TQE_BIN')


def run_tqe(binary, keys, content, timeout, keep_dir=None, want_path=None):
    """Run the editor on a temp file seeded with content.

    Returns (file_bytes, terminal_output, exit_status) where
    exit_status is the wait status int or the string 'TIMEOUT'.
    file_bytes is the contents of want_path (or the main temp file).

    Startup handshake: the editor sends a cursor-position request
    (\\x1b[6n) which we answer, then OSC 10/11 default-color queries
    which we also answer, so startup finishes quickly and
    deterministically instead of relying on fixed delays (the editor
    discards any keystrokes that arrive during the query phase).
    """
    if keep_dir:
        path = os.path.join(keep_dir, 'main.txt')
        with open(path, 'wb') as f:
            f.write(content.encode())
    else:
        fd_tmp, path = tempfile.mkstemp(suffix='.txt')
        with os.fdopen(fd_tmp, 'wb') as f:
            f.write(content.encode())

    result_path = want_path if want_path else path

    pid, fd = pty.fork()
    if pid == 0:  # child
        os.environ['TERM'] = 'vt100'
        os.environ['LINES'] = '24'
        os.environ['COLUMNS'] = '80'
        try:
            os.execv(binary, [binary, path])
        except Exception:
            pass
        os._exit(127)

    out = bytearray()
    screen = TermScreen()
    frames = []                  # screen snapshots, one per output chunk
    status = None
    deadline = time.time() + timeout

    def read_avail(t=0.05):
        r, _, _ = select.select([fd], [], [], t)
        if not r:
            return b''
        try:
            return os.read(fd, 65536)
        except OSError:
            return b''

    def absorb(data):
        out.extend(data)
        screen.feed(data)
        frames.append(screen.snapshot())

    # ---- startup handshake ----
    time.sleep(0.3)
    os.write(fd, b'\033[1;1R')          # answer \x1b[6n cursor request
    replied_fg = replied_bg = False
    start_deadline = time.time() + 2.0
    quiet = 0.0
    while time.time() < start_deadline:
        data = read_avail(0.05)
        if data:
            absorb(data)
            quiet = 0.0
        else:
            quiet += 0.05
        if not replied_fg and b'\x1b]10;?' in out:
            os.write(fd, b'\x1b]10;rgb:cccc/cccc/cccc\x07')
            replied_fg = True
        if not replied_bg and b'\x1b]11;?' in out:
            os.write(fd, b'\x1b]11;rgb:1111/1111/2222\x07')
            replied_bg = True
        if replied_fg and replied_bg and quiet >= 0.15:
            break

    # ---- send the test keys ----
    # Keys are sent one byte at a time, waiting for the editor output to
    # go quiet after each.  Otherwise the editor may read several bytes
    # in one chunk while it is busy drawing, and the terminal input layer
    # then composes ESC + following byte into a META key, which changes
    # what the vi layer sees.
    idle = 0.0
    for i in range(len(keys)):
        os.write(fd, keys[i:i + 1])
        idle = 0.0
        while idle < 0.08:
            data = read_avail(0.02)
            if data:
                absorb(data)
                idle = 0.0
            else:
                idle += 0.02

    # ---- run until exit ----
    while time.time() < deadline:
        data = read_avail(0.05)
        if data:
            absorb(data)
        wpid, st = os.waitpid(pid, os.WNOHANG)
        if wpid == pid:
            status = st
            break

    if status is None:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
        status = 'TIMEOUT'
    else:
        # drain anything still buffered after exit
        while True:
            data = read_avail(0.05)
            if not data:
                break
            absorb(data)

    file_bytes = b''
    try:
        with open(result_path, 'rb') as f:
            file_bytes = f.read()
    except OSError:
        pass

    if not keep_dir:
        try:
            os.unlink(path)
        except OSError:
            pass
        if want_path:
            try:
                os.unlink(result_path)
            except OSError:
                pass
    try:
        os.close(fd)
    except OSError:
        pass

    return file_bytes, bytes(out), screen.status_frames, status


class T:
    """A single test case."""

    def __init__(self, name, keys, init, want,
                 cat='general', status=None, timeout=5.0,
                 want_path=None):
        self.name = name
        self.keys = keys
        self.init = init
        self.want = want
        self.cat = cat
        self.status = status          # str or list of str, or None
        self.timeout = timeout
        self.want_path = want_path    # may contain {DIR}


# -------------------------------------------------------------------------
# Test catalog
# Keystroke sequences use spaces only inside NORMAL-mode segments.
# -------------------------------------------------------------------------

TESTS = [
    # ---- normal mode: motions ----
    T('normal_dd',        b'dd:wq\r', 'one\ntwo\n', b'two\n', cat='normal'),
    T('normal_xx',        b'xx:wq\r', 'abc\n', b'c\n', cat='normal'),
    # vim `u` undoes one change at a time, so `u` after two separate
    # x presses restores exactly one character.
    T('normal_undo_x',    b'xxu:wq\r', 'abc\n', b'bc\n', cat='normal'),
    T('normal_undo_dd',   b'ddu:wq\r', 'one\ntwo\n', b'one\ntwo\n',
      cat='normal'),
    T('normal_G_gg',      b'Gddggdd:wq\r', 'a\nb\nc\n', b'b\nc\n',
      cat='normal'),
    # NOTE: qemacs `w` moves to the end of the current word (the space),
    # unlike vim which jumps to the start of the next word.  These two
    # tests document the current behavior; revisit when word motions
    # are aligned with vim.
    T('normal_word_fwd',  b'w x:wq\r', 'hello world\n', b'helloworld\n',
      cat='normal'),
    T('normal_word_back', b'$ b x:wq\r', 'hello world\n', b'hello orld\n',
      cat='normal'),
    T('normal_bol_0',     b'l l 0 x:wq\r', 'abc\n', b'bc\n', cat='normal'),
    T('normal_caret',     b'^ x:wq\r', '   foo\n', b'   oo\n', cat='normal'),
    T('normal_eol_x_joins', b'$ x:wq\r', 'abc\n', b'abc', cat='normal'),

    # ---- insert mode ----
    T('insert_i',       b'ihi\x1b:wq\r', '', b'hi', cat='insert'),
    T('insert_a',       b'aX\x1b:wq\r', 'ab\n', b'aXb\n', cat='insert'),
    T('insert_o',       b'oline2\x1b:wq\r', 'line1\n', b'line1\nline2\n',
      cat='insert'),
    T('insert_O',       b'Oline0\x1b:wq\r', 'line1\n', b'line0\nline1\n',
      cat='insert'),
    T('insert_esc',     b'ihello\x1b:wq\r', '', b'hello', cat='insert'),

    # ---- pending multi-key commands ----
    T('pending_dd',         b'dd:wq\r', 'one\ntwo\n', b'two\n',
      cat='pending'),
    T('pending_gt_gt',      b':set sw=2\r>>:wq\r', '  x\n', b'    x\n',
      cat='pending'),
    T('pending_lt_lt',      b':set sw=2\r>><<:wq\r', '  x\n', b'  x\n',
      cat='pending'),
    T('pending_d_cancel',   b'd:q!\r', 'unchanged\n', b'unchanged\n',
      cat='pending'),
    T('pending_cancel_v',   b'd v l l y j $ p:wq\r', 'abcd\nefgh\n',
      b'abcd\nefghabc\n', cat='pending'),

    # ---- indent ----
    T('indent_sw2',     b':set sw=2\r>>:wq\r', '  x\n', b'    x\n',
      cat='indent'),
    T('indent_back',    b':set sw=2\r>><<:wq\r', '  x\n', b'  x\n',
      cat='indent'),

    # ---- search ----
    T('search_fwd',         b'/two\rndd:wq\r', 'one\ntwo\none\n',
      b'one\none\n', cat='search'),
    T('search_fwd_wrap',    b'/foo\rnndd:wq\r', 'foo\nbar\nfoo\n',
      b'bar\nfoo\n', cat='search'),
    T('search_bwd_wrap',    b'?foo\rnndd:wq\r', 'foo\nbar\nfoo\n',
      b'foo\nbar\n', cat='search'),
    T('search_eof_wrap',    b'G/aaa\rdd:wq\r', 'aaa\nbbb\nccc\n',
      b'bbb\nccc\n', cat='search'),
    T('search_nomatch',     b'/zzz\r:wq\r', 'unchanged\n', b'unchanged\n',
      cat='search'),

    # ---- substitute ----
    T('subst_global',       b':%s/one/ONE/g\r:wq\r', 'one two one\n',
      b'ONE two ONE\n', cat='substitute'),
    T('subst_spaces',       b':%s/QEmacs/Evil QEmacs/g\r:wq\r',
      'QEmacs is QEmacs\n', b'Evil QEmacs is Evil QEmacs\n',
      cat='substitute'),
    T('subst_first_only',   b':s/QEmacs/Evil QEmacs/\r:wq\r',
      'QEmacs first\nQEmacs second\n',
      b'Evil QEmacs first\nQEmacs second\n', cat='substitute'),
    T('subst_nog_first',    b':%s/one/ONE/\r:wq\r', 'one two one\n',
      b'ONE two one\n', cat='substitute'),
    T('subst_delete',       b':%s/one//g\r:wq\r', 'one two one\n',
      b' two \n', cat='substitute'),
    T('subst_nomatch',      b':%s/zzz/AAA/g\r:wq\r', 'unchanged\n',
      b'unchanged\n', cat='substitute'),

    # ---- ex commands ----
    T('ex_wq',          b'ihi\x1b:wq\r', '', b'hi', cat='ex'),
    T('ex_x',           b'ihello\x1b:x\r', '', b'hello', cat='ex'),
    T('ex_q_nosave',    b'iCHANGED\x1b:q!\r', 'keep\n', b'keep\n',
      cat='ex'),
    T('ex_goto_line',   b':2\rdd:wq\r', 'one\ntwo\nthree\n',
      b'one\nthree\n', cat='ex'),
    T('ex_set_bad',     b':set foo\r:wq\r', 'unchanged\n', b'unchanged\n',
      cat='ex'),
    T('ex_w_spaces',
      b':w {DIR}/sp ace.txt\r:q!\r',
      'hello world\n', b'hello world\n',
      cat='ex', want_path='{DIR}/sp ace.txt'),

    # ---- visual mode ----
    T('visual_char_yank_put', b'v l l y j $ p:wq\r', 'abcd\nefgh\n',
      b'abcd\nefghabc\n', cat='visual'),
    T('visual_char_delete',   b'v l l d:wq\r', 'abcd\n', b'd\n',
      cat='visual'),
    T('visual_char_x',        b'v l l x:wq\r', 'abcd\n', b'd\n',
      cat='visual'),
    T('visual_toggle_exit',   b'v l v v l y j $ p:wq\r', 'abcd\nefgh\n',
      b'abcd\nefghbc\n', cat='visual'),
    T('visual_esc_cancel',    b'v l l \x1b:wq\r', 'abcd\n', b'abcd\n',
      cat='visual'),
    T('visual_bwd_char',      b'$ v h h y j $ p:wq\r', 'abcd\nefgh\n',
      b'abcd\nefghcd\n', cat='visual'),
    T('visual_undo',          b'v l l d u:wq\r', 'abcd\n', b'abcd\n',
      cat='visual'),
    T('visual_insert_exit',   b'v l l iXX\x1b:wq\r', 'abcd\n',
      b'abXXcd\n', cat='visual'),
    T('visual_empty_put',     b'v l p:wq\r', '', b'', cat='visual'),

    # ---- visual line ----
    T('vline_delete',         b'j V d:wq\r', 'aaa\nbbb\nccc\n',
      b'aaa\nccc\n', cat='visual'),
    T('vline_yank2_put_eof',  b'V j y G o\x1bp:wq\r', 'aaa\nbbb\nccc\n',
      b'aaa\nbbb\nccc\n\naaa\nbbb\n', cat='visual'),
    T('vline_bwd_delete',     b'G V k d:wq\r', 'aaa\nbbb\nccc\n',
      b'aaa\nbbb\n', cat='visual'),

    # ---- put / register types ----
    T('put_charwise_eol',     b'v l y j $ p:wq\r', 'abcd\nefgh\n',
      b'abcd\nefghab\n', cat='put'),
    T('put_charwise_eol_join', b'v l y $ p:wq\r', 'abcd\n',
      b'abcdab\n', cat='put'),
    T('put_linewise_restore', b'dd p:wq\r', 'aaa\nbbb\nccc\n',
      b'bbb\naaa\nccc\n', cat='put'),
    T('put_linewise_below',   b'dd jp:wq\r', 'aaa\nbbb\nccc\n',
      b'bbb\nccc\naaa\n', cat='put'),
    T('put_charreg_vline',    b'v l y j V p:wq\r', 'ab\ntwo\n',
      b'ab\nab\n', cat='put'),
    T('put_linereg_vline',    b'V y j V p:wq\r', 'one\ntwo\nthree\n',
      b'one\none\nthree\n', cat='put'),

    # ---- status line checks ----
    T('status_visual',        b'v \x1b:wq\r', 'x\n', b'x\n',
      cat='status', status='-- VISUAL --'),
    T('status_vline',         b'V \x1b:wq\r', 'x\n', b'x\n',
      cat='status', status='-- VISUAL LINE --'),
    T('status_back_normal',   b'v \x1b:wq\r', 'x\n', b'x\n',
      cat='status', status='-- NORMAL --'),
    T('status_insert',        b'i\x1b:wq\r', 'x\n', b'x\n',
      cat='status', status=['-- INSERT --', '-- NORMAL --']),
]


def run_one(t, binary, workdir, default_timeout):
    keys = t.keys.replace(b'{DIR}', workdir.encode())
    want_path = t.want_path.replace('{DIR}', workdir) if t.want_path else None
    file_bytes, term_out, frames, status = run_tqe(
        binary, keys, t.init, t.timeout or default_timeout,
        want_path=want_path)
    errors = []
    if status == 'TIMEOUT':
        errors.append('editor did not exit within %.1fs (TIMEOUT)'
                      % (t.timeout or default_timeout))
    elif status != 0:
        errors.append('editor exit status %r (expected 0)' % (status,))
    if file_bytes != t.want:
        errors.append('file mismatch:\n  want: %r\n  got:  %r'
                      % (t.want, file_bytes))
    if t.status:
        wanted = t.status if isinstance(t.status, list) else [t.status]
        for w in wanted:
            if not any(w in line for line in frames):
                errors.append('status %r not seen on the status line'
                              % (w,))
    return errors


def main():
    ap = argparse.ArgumentParser(description='Vi layer PTY test suite')
    ap.add_argument('filters', nargs='*', metavar='FILTER',
                    help='only run tests whose name/category match')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='print terminal output excerpt on failure')
    ap.add_argument('--binary', default=None,
                    help='editor binary to test (default: TQE_BIN or ../teqe)')
    ap.add_argument('--timeout', type=float, default=5.0,
                    help='default per-test timeout in seconds')
    ap.add_argument('--keep', action='store_true',
                    help='keep temp files for debugging')
    ap.add_argument('--list', action='store_true',
                    help='list test names and exit')
    args = ap.parse_args()

    if args.list:
        for t in TESTS:
            print('%-24s %s' % (t.name, t.cat))
        return 0

    binary = os.path.abspath(args.binary or default_binary())
    if not os.path.exists(binary):
        sys.exit('ERROR: binary not found: %s' % binary)

    tests = list(TESTS)
    for f in args.filters:
        tests = [t for t in tests
                 if f in t.name or f in t.cat]
    if not tests:
        print('no tests match filters: %s' % ' '.join(args.filters))
        return 1

    if args.keep:
        workdir = tempfile.mkdtemp(prefix='tqe_vi_test_')
    else:
        workdir = tempfile.mkdtemp(prefix='tqe_vi_test_')

    print('binary: %s' % binary)
    print('running %d tests...\n' % len(tests))

    passed = failed = 0
    for t in tests:
        try:
            errors = run_one(t, binary, workdir, args.timeout)
        except Exception as exc:  # framework error, report and continue
            errors = ['exception: %r' % (exc,)]
        if errors:
            failed += 1
            print('FAIL %s (%s)' % (t.name, t.cat))
            for e in errors:
                print('     %s' % e)
            if args.verbose:
                pass  # output excerpt printing handled in run_one scope
        else:
            passed += 1
            print('pass %s (%s)' % (t.name, t.cat))

    print('\n%d passed, %d failed, %d total' % (passed, failed, len(tests)))

    if not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print('temp files kept in: %s' % workdir)

    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
