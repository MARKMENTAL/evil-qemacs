#include "qe.h"

/*
 * Evil/Vi modal editing layer for QEmacs.
 *
 * This is intentionally small and self-contained: it intercepts keys in
 * normal mode, passes everything through in insert mode, and provides a
 * minimal :ex command line via the existing minibuffer machinery.
 */

extern void do_indent_rigidly_to_tab_stop(EditState *s, int start, int end, int dir);

/* Track whether each yank-ring slot holds linewise text, so that `p`
 * can paste whole lines below the current line like vim.  qemacs kill
 * registers carry no type information, so vi keeps its own map keyed
 * by qs->yank_current (updated by every do_kill). */
static int vi_yank_linewise[NB_YANK_BUFFERS];

/* Some terminals encode shifted keys as KEY_SHIFT(c) instead of the ASCII
 * character.  Map those back to the ASCII a user expects for vi commands. */
static int vi_normalize_key(int key)
{
    if (KEY_IS_SHIFT(key)) {
        int c = key & 0xFF;
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 'A';
        switch (c) {
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '`': return '~';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '"';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        }
    }
    return key;
}

static void vi_set_pending(EditState *s, int pending)
{
    s->vi_pending = pending;
    if (pending >= ' ' && pending <= '~')
        put_status(s, "-- NORMAL -- %c-", pending);
    else if (pending)
        put_status(s, "-- NORMAL -- ^%c-", pending + '@');
    else
        put_status(s, "-- NORMAL --");
}

/* ---- Visual mode helpers ---- */

/* Extend END past a newline character at END (used for linewise ops so
 * whole lines including their trailing newline are grabbed). */
static int vi_include_newline(EditBuffer *b, int end)
{
    int ch, next;
    if (end >= b->total_size)
        return end;
    ch = eb_nextc(b, end, &next);
    if (ch == '\n')
        end = next;
    return end;
}

/* Extend END to include the character under the cursor (vim charwise
 * selection is inclusive) unless that character is a newline. */
static int vi_include_cursor_char(EditBuffer *b, int end)
{
    int ch, next;
    if (end >= b->total_size)
        return end;
    ch = eb_nextc(b, end, &next);
    if (ch != '\n')
        end = next;
    return end;
}

/* Compute the visual region from the anchor (vi_visual_start) and the
 * cursor (s->offset).  Charwise selections include the character under
 * the cursor on the moving side; linewise selections cover whole lines
 * including the trailing newline of the last line. */
static void vi_visual_region(EditState *s, int *p1, int *p2)
{
    int a = s->vi_visual_start;
    int c = s->offset;
    int save, tmp;
    if (a > c) { tmp = a; a = c; c = tmp; }
    if (s->vi_visual_linewise) {
        save = s->offset;
        s->offset = a; do_bol(s); a = s->offset;
        s->offset = save; do_eol(s); c = s->offset;
        s->offset = save;
        c = vi_include_newline(s->b, c);
    } else {
        if (s->offset >= s->vi_visual_start) {
            a = s->vi_visual_start;
            c = vi_include_cursor_char(s->b, s->offset);
        } else {
            a = s->offset;
            c = vi_include_cursor_char(s->b, s->vi_visual_start);
        }
    }
    *p1 = a;
    *p2 = c;
}

/* Update the visual selection highlight after a motion.
 * The cursor (s->offset) is the moving end of the selection; only the
 * mark is adjusted so motions keep their natural arithmetic and the
 * yank/delete region can always be recomputed from anchor + cursor.
 * Linewise snaps the cursor to EOL of the last selected line. */
static void vi_visual_update(EditState *s)
{
    int a = s->vi_visual_start;
    int c = s->offset;
    int save, tmp;
    if (a > c) { tmp = a; a = c; c = tmp; }
    if (s->vi_visual_linewise) {
        save = s->offset;
        s->offset = a; do_bol(s); a = s->offset;
        s->offset = save; do_eol(s); c = s->offset;
        s->b->mark = a;
        s->offset = c;
    } else {
        s->b->mark = a;
    }
    s->region_style = QE_STYLE_REGION_HILITE;
}

/* Begin visual mode. linewise: 0=char, 1=line. */
static void vi_visual_begin(EditState *s, int linewise)
{
    s->vi_visual_active = 1;
    s->vi_visual_linewise = linewise;
    s->vi_visual_start = s->offset;
    s->region_style = QE_STYLE_REGION_HILITE;
    put_status(s, linewise ? "-- VISUAL LINE --" : "-- VISUAL --");
    vi_visual_update(s);
}

/* End visual mode. */
static void vi_visual_end(EditState *s)
{
    s->vi_visual_active = 0;
    s->vi_visual_linewise = 0;
    s->region_style = 0;
    put_status(s, "-- NORMAL --");
}

/* Toggle visual mode. linewise: 0=char, 1=line. */
static void vi_visual_toggle(EditState *s, int linewise)
{
    if (s->vi_visual_active) {
        vi_visual_end(s);
    } else {
        vi_visual_begin(s, linewise);
    }
}

/* Yank the visual region and exit. */
static void vi_visual_yank(EditState *s)
{
    int p1, p2;
    int linewise = s->vi_visual_linewise;
    vi_visual_region(s, &p1, &p2);
    do_kill(s, p1, p2, 0, 1);
    vi_yank_linewise[s->qs->yank_current] = linewise;
    s->qs->last_cmd_func = NULL;
    vi_visual_end(s);
}

/* Delete the visual region and exit. */
static void vi_visual_delete(EditState *s)
{
    int p1, p2;
    int linewise = s->vi_visual_linewise;
    vi_visual_region(s, &p1, &p2);
    do_kill(s, p1, p2, 1, 0);
    vi_yank_linewise[s->qs->yank_current] = linewise;
    s->qs->last_cmd_func = NULL;
    vi_visual_end(s);
}

/* Put the yanked text, replacing the visual region, then exit.
 * The region is removed directly (not via do_kill) so the yank
 * register that is being pasted is not overwritten. */
static void vi_visual_put(EditState *s)
{
    QEmacsState *qs = s->qs;
    int p1, p2, size;
    EditBuffer *yb;

    if (s->b->flags & BF_READONLY) {
        put_status(s, "Buffer is read-only");
        return;
    }
    yb = qs->yank_buffers[qs->yank_current];
    size = yb ? yb->total_size : 0;
    if (size <= 0) {
        put_status(s, "Nothing to put");
        return;
    }
    vi_visual_region(s, &p1, &p2);
    if (p1 != p2) {
        if (s->mode->delete_bytes)
            s->mode->delete_bytes(s, p1, p2 - p1);
        else
            eb_delete_range(s->b, p1, p2);
        s->offset = p1;
    } else if (s->vi_visual_linewise) {
        /* empty linewise selection: paste on a new line below */
        do_eol(s);
        do_char(s, '\n', 1);
    }
    s->b->last_log = LOGOP_FREE;
    s->offset += eb_insert_buffer_convert(s->b, s->offset, yb, 0, size);
    if (s->vi_visual_linewise) {
        /* a charwise register pasted linewise still needs its newline */
        int pch, pnext;
        pch = eb_nextc(s->b, eb_prev(s->b, s->offset), &pnext);
        if (pch != '\n') {
            s->offset += eb_insert_str(s->b, s->offset, "\n");
        }
    }
    vi_visual_end(s);
}

/* Search forward with wrap-around (vim-style wrapscan) */
static void vi_search_forward(EditState *s, const char *str)
{
    int start = s->offset;
    do_search_string(s, str, 1);
    if (s->offset == start) {
        /* no match after cursor; wrap to beginning of buffer */
        do_bof(s);
        do_search_string(s, str, 1);
    }
}

/* Search backward with wrap-around (vim-style wrapscan) */
static void vi_search_backward(EditState *s, const char *str)
{
    int start = s->offset;
    do_search_string(s, str, -1);
    if (s->offset == start) {
        /* no match before cursor; wrap to end of buffer */
        do_eof(s);
        do_search_string(s, str, -1);
    }
}

/* Perform a vi search and remember it for n/N repeats */
static void vi_do_search(EditState *s, const char *str, int dir)
{
    if (!str || !*str)
        return;
    s->vi_search_dir = dir;
    pstrcpy(s->vi_search_str, sizeof(s->vi_search_str), str);
    if (dir == 1)
        vi_search_forward(s, str);
    else
        vi_search_backward(s, str);
}

static void vi_search_callback(void *opaque, char *buf, CompletionDef *completion)
{
    EditState *s = opaque;
    if (buf && *buf)
        vi_do_search(s, buf, s->vi_search_dir);
    qe_free(&buf);
}

/* Replace only the first occurrence of OLD with NEW in the buffer */
static void vi_replace_first(EditState *s, const char *old, const char *new_)
{
    int end;
    if (!old || !*old) {
        put_error(s, "No search string");
        return;
    }
    do_bof(s);
    do_search_string(s, old, 1);  /* forward to end of first match */
    end = s->offset;
    if (end == 0) {
        /* not found; do_search_string already printed an error */
        return;
    }
    do_search_string(s, old, -1); /* backward to start of the same match */
    eb_delete_range(s->b, s->offset, end);
    eb_insert_str(s->b, s->offset, new_ ? new_ : "");
}

/* Parse :[range]s/old/new/[flags].  Only %s and s are supported. */
static int vi_parse_substitute(EditState *s, const char *cmd)
{
    const char *p;
    char delim;
    char old[512], new_[512];
    int global = 0;
    int i;

    p = cmd;
    if (*p == '%')
        p++;
    if (*p != 's')
        return -1;
    p++;
    delim = *p++;
    if (!delim)
        return -1;

    i = 0;
    while (*p && *p != delim && i < (int)sizeof(old) - 1)
        old[i++] = *p++;
    old[i] = '\0';
    if (*p != delim)
        return -1;
    p++;

    i = 0;
    while (*p && *p != delim && i < (int)sizeof(new_) - 1)
        new_[i++] = *p++;
    new_[i] = '\0';
    if (*p == delim)
        p++;

    if (*p == 'g')
        global = 1;

    if (global) {
        do_bof(s);
        do_replace_string(s, old, new_, 1);
    } else {
        vi_replace_first(s, old, new_);
    }
    return 0;
}

/* Wisdom popup: static table of open-source / philosophy quotes.
 * Display format is always "\"text\" - author" (double quotes outside,
 * single quotes for any quotation inside the text).  Index N is 0-based. */
typedef struct {
    const char *text;
    const char *author;
} ViWisdom;

static const ViWisdom vi_wisdom[] = {
    { "Talk is cheap. Show me the code.", "Linus Torvalds" },
    { "Every program attempts to expand until it can read mail. Those that can't do this, are replaced by ones that can.", "Jamie Zawinski" },
    { "Programs must be written for people to read, and only incidentally for machines to execute.", "Harold Abelson" },
    { "Simplicity is prerequisite for reliability.", "Edsger W. Dijkstra" },
    { "The best way to predict the future is to invent it.", "Alan Kay" },
    { "Free software is a matter of liberty, not price; think of 'free speech', not 'free beer'.", "Richard M. Stallman" },
    { "Given enough eyeballs, all bugs are shallow.", "Eric S. Raymond" },
    { "Abstraction is for complexity; silicon is for speed.", "MARKMENTAL" },
    { "Stay hungry, stay foolish.", "Stewart Brand" },
    { "The question of whether a computer can think is no more interesting than the question of whether a submarine can swim.", "Edsger W. Dijkstra" },
    { "First, solve the problem. Then, write the code.", "John Johnson" },
    { "Perfection is achieved not when there is nothing more to add, but when there is nothing left to take away.", "Antoine de Saint-Exupery" },
    { "The best programs are the ones written when the programmer is supposed to be working on something else.", "Melinda Varian" },
    { "A primary cause of complexity is that software vendors uncritically adopt almost any feature that users want.", "Niklaus Wirth" },
};

static int vi_wisdom_seeded = 0;

/* Show one quote in a help-style popup (q to close, buffer navigation).
 * arg: "" for random, otherwise a 0-based index into vi_wisdom. */
static void do_vi_wisdom(EditState *s, const char *arg)
{
    int n = countof(vi_wisdom);
    int idx;
    EditBuffer *b;

    while (*arg == ' ' || *arg == '\t')
        arg++;
    if (*arg == '\0') {
        if (!vi_wisdom_seeded) {
            srand((unsigned)time(NULL));
            vi_wisdom_seeded = 1;
        }
        idx = rand() % n;
    } else {
        char *end;
        long val = strtol(arg, &end, 10);
        while (*end == ' ' || *end == '\t')
            end++;
        if (end == arg || *end != '\0' || val < 0 || val >= n) {
            put_status(s, "Invalid wisdom index '%s' (0..%d)", arg, n - 1);
            return;
        }
        idx = (int)val;
    }

    b = qe_new_buffer(s->qs, "*Wisdom*", BF_SYSTEM | BF_UTF8);
    if (!b)
        return;
    eb_printf(b, "\"%s\" - %s\n\n-- press q to close --\n",
              vi_wisdom[idx].text, vi_wisdom[idx].author);
    {
        EditState *e = show_popup(s, b, "Wisdom");
        if (e)
            e->wrap = WRAP_WORD; /* long quotes word-wrap instead of truncating */
    }
}

/* Find the topmost Wisdom popup, if any.  Used so bare `q` in vi normal
 * mode can dismiss it even after focus moved back to a text window
 * (e.g. via C-w w / C-x o), where the popup's own `q` binding would
 * otherwise never fire because dispatch is active-window-only. */
static EditState *vi_find_wisdom_popup(QEmacsState *qs)
{
    EditState *e, *found = NULL;
    for (e = qs->first_window; e; e = e->next_window) {
        if ((e->flags & WF_POPUP) && e->b &&
            !strcmp(e->b->name, "*Wisdom*")) {
            found = e;
        }
    }
    return found;
}

/* Global `wisdom` command body: show a random quote (F1, M-x wisdom).
 * NOTE: no C-h binding on purpose: on standard terminals the tty layer
 * translates ^H to KEY_DEL (KBS_CONTROL_H), so a "C-h ..." binding would
 * never fire -- the same reason the old C-h help tree felt dead. */
static void do_wisdom(EditState *s)
{
    do_vi_wisdom(s, "");
}

/* Ex command prompt callback */
static void vi_ex_callback(void *opaque, char *buf, CompletionDef *completion)
{
    EditState *s = opaque;
    const char *p, *arg;
    char cmd[128];
    int i;

    if (!buf) {
        /* aborted */
        return;
    }

    p = buf;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == ':')
        p++;

    /* Substitute commands may contain spaces inside the old/new patterns,
     * so when the command looks like a substitute, treat the whole remainder
     * of the line as the command instead of splitting at the first space. */
    if (((*p == '%' && p[1] == 's' && p[2] && !qe_isalnum((unsigned char)p[2])
          && p[2] != ' ' && p[2] != '\t') ||
         (*p == 's' && p[1] && !qe_isalnum((unsigned char)p[1])
          && p[1] != ' ' && p[1] != '\t'))) {
        i = 0;
        while (*p && i < (int)sizeof(cmd) - 1)
            cmd[i++] = *p++;
        cmd[i] = '\0';
        arg = "";
    } else {
        /* extract first word as command, remainder as argument */
        i = 0;
        while (*p && *p != ' ' && *p != '\t' && i < (int)sizeof(cmd) - 1)
            cmd[i++] = *p++;
        cmd[i] = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        arg = p;
    }

    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
        do_exit_qemacs(s, NO_ARG);
    } else if (strcmp(cmd, "q!") == 0 || strcmp(cmd, "quit!") == 0) {
        do_exit_qemacs(s, 1);
    } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
        do_save_buffer(s);
        do_exit_qemacs(s, NO_ARG);
    } else if (strcmp(cmd, "w") == 0 || strcmp(cmd, "write") == 0) {
        if (*arg)
            do_write_file(s, arg);
        else
            do_save_buffer(s);
    } else if (strcmp(cmd, "set") == 0) {
        if (strstart(arg, "sw", &arg)) {
            while (*arg == ' ' || *arg == '\t' || *arg == '=')
                arg++;
            if (qe_isdigit((unsigned char)*arg)) {
                s->indent_width = strtol(arg, NULL, 10);
                put_status(s, "shiftwidth=%d", s->indent_width);
            } else {
                put_status(s, "shiftwidth=%d", s->indent_width);
            }
        } else {
            put_status(s, "Unsupported set option: %s", arg);
        }
    } else if (strstart(cmd, "%s", &arg) ||
               (strstart(cmd, "s", &arg) && *arg == '/')) {
        if (vi_parse_substitute(s, cmd) < 0)
            put_status(s, "Invalid substitute command");
    } else if (strcmp(cmd, "wisdom") == 0 || strcmp(cmd, "quotes") == 0) {
        do_vi_wisdom(s, arg);
    } else if (cmd[0] >= '0' && cmd[0] <= '9') {
        /* :<number> goes to that line */
        do_goto_line(s, strtol(cmd, NULL, 10), 0);
    } else if (*cmd) {
        put_status(s, "Not an ex command: %s", cmd);
    }

    qe_free(&buf);
}

/* Toggle vi mode on/off for the current window */
static void do_vi_mode(EditState *s)
{
    if (s->flags & WF_VI_NORMAL) {
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
    } else {
        s->flags |= WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- NORMAL --");
    }
}

static void do_vi_normal_mode(EditState *s)
{
    s->flags |= WF_VI_NORMAL;
    s->vi_pending = 0;
    put_status(s, "-- NORMAL --");
}

static void do_vi_insert_mode(EditState *s)
{
    s->flags &= ~WF_VI_NORMAL;
    s->vi_pending = 0;
    put_status(s, "-- INSERT --");
}

/* Process a single key in normal mode. Return 1 if consumed. */
static int vi_normal_key(EditState *s, int key)
{
    key = vi_normalize_key(key);

    /* Pending multi-key commands (dd, gg, >>, <<, ...).
     * Mode-switching keys cancel the pending state and are processed
     * normally so e.g. "d:q!" does the right thing. */
    if (s->vi_pending == 'd') {
        if (key == 'd') {
            do_kill_whole_line(s, 1);
            vi_yank_linewise[s->qs->yank_current] = 1;
        } else if (key == ':' || key == 'i' || key == 'a' ||
                    key == 'o' || key == 'O' || key == 'v' ||
                    key == 'V' || key == 'y' || key == 'p') {
             vi_set_pending(s, 0);
             return vi_normal_key(s, key);
         } else {
             put_status(s, "Unknown d command");
         }
         vi_set_pending(s, 0);
         return 1;
     }
     if (s->vi_pending == 'g') {
         if (key == 'g') {
             do_bof(s);
         } else if (key == 'G') {
             do_eof(s);
         } else if (key == ':' || key == 'i' || key == 'a' ||
                    key == 'o' || key == 'O' || key == 'v' ||
                    key == 'V' || key == 'y' || key == 'p') {
             vi_set_pending(s, 0);
             return vi_normal_key(s, key);
         } else {
             put_status(s, "Unknown g command");
         }
         vi_set_pending(s, 0);
         return 1;
     }
     if (s->vi_pending == '>') {
         if (key == '>') {
             do_indent_rigidly_to_tab_stop(s, s->offset, s->offset, +1);
         } else if (key == ':' || key == 'i' || key == 'a' ||
                    key == 'o' || key == 'O' || key == 'v' ||
                    key == 'V' || key == 'y' || key == 'p') {
             vi_set_pending(s, 0);
             return vi_normal_key(s, key);
         } else {
             put_status(s, "Unknown > command");
         }
         vi_set_pending(s, 0);
         return 1;
     }
     if (s->vi_pending == '<') {
         if (key == '<') {
             do_indent_rigidly_to_tab_stop(s, s->offset, s->offset, -1);
         } else if (key == ':' || key == 'i' || key == 'a' ||
                    key == 'o' || key == 'O' || key == 'v' ||
                    key == 'V' || key == 'y' || key == 'p') {
             vi_set_pending(s, 0);
             return vi_normal_key(s, key);
         } else {
             put_status(s, "Unknown < command");
         }
         vi_set_pending(s, 0);
         return 1;
     }
     if (s->vi_pending == KEY_CTRL('w')) {
         if (key == 'w' || key == KEY_CTRL('w')) {
             do_other_window(s, 1);
         }
#ifndef CONFIG_TINY
         else if (key == 'h') {
             do_find_window(s, KEY_LEFT);
         } else if (key == 'j') {
             do_find_window(s, KEY_DOWN);
         } else if (key == 'k') {
             do_find_window(s, KEY_UP);
         } else if (key == 'l') {
             do_find_window(s, KEY_RIGHT);
         }
#endif
         else if (key == ':' || key == 'i' || key == 'a' ||
                    key == 'o' || key == 'O' || key == 'v' ||
                    key == 'V' || key == 'y' || key == 'p') {
             vi_set_pending(s, 0);
             return vi_normal_key(s, key);
         } else {
             put_status(s, "Unknown C-w command");
         }
         vi_set_pending(s, 0);
         return 1;
     }

    switch (key) {
    case 'i':
        if (s->vi_visual_active) vi_visual_end(s);
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
    case 'a':
        if (s->vi_visual_active) vi_visual_end(s);
        do_left_right(s, 1);
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
    case 'o':
        if (s->vi_visual_active) vi_visual_end(s);
        do_eol(s);
        do_char(s, '\n', 1);
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
    case 'O':
        if (s->vi_visual_active) vi_visual_end(s);
        do_bol(s);
        do_open_line(s);
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
     case 'h':
         do_left_right(s, -1);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case 'j':
         do_up_down(s, 1);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case 'k':
         do_up_down(s, -1);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case 'l':
         do_left_right(s, 1);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case 'w':
         do_word_left_right(s, 1);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case 'b':
         do_word_left_right(s, -1);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case '0':
         do_bol(s);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case '^':
         do_bol_nspace(s);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case '$':
         do_eol(s);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
     case 'G':
         do_eof(s);
         if (s->vi_visual_active) vi_visual_update(s);
         return 1;
    case 'x':
        if (s->vi_visual_active) {
            vi_visual_delete(s);
            return 1;
        }
        do_delete_char(s, 1);
        return 1;
    case 'u':
        do_undo(s);
        return 1;
    case 'g':
        vi_set_pending(s, 'g');
        return 1;
    case '>':
        vi_set_pending(s, '>');
        return 1;
    case '<':
        vi_set_pending(s, '<');
        return 1;
    case KEY_CTRL('w'):
        vi_set_pending(s, KEY_CTRL('w'));
        return 1;
     case '/':
         s->vi_pending = 0;
         s->vi_search_dir = 1;
         put_status(s, "-- NORMAL --");
         minibuffer_edit(s, "", "/", NULL, NULL, vi_search_callback, s);
         return 1;
     case '?':
         s->vi_pending = 0;
         s->vi_search_dir = -1;
         put_status(s, "-- NORMAL --");
         minibuffer_edit(s, "", "?", NULL, NULL, vi_search_callback, s);
         return 1;
     case 'n':
         if (s->vi_search_str[0]) {
             if (s->vi_search_dir == 1)
                 vi_search_forward(s, s->vi_search_str);
             else
                 vi_search_backward(s, s->vi_search_str);
         } else {
             put_status(s, "No previous search");
         }
         return 1;
     case 'N':
         if (s->vi_search_str[0]) {
             if (s->vi_search_dir == 1)
                 vi_search_backward(s, s->vi_search_str);
             else
                 vi_search_forward(s, s->vi_search_str);
         } else {
             put_status(s, "No previous search");
         }
         return 1;
    case 'p':
        if (s->vi_visual_active) {
            vi_visual_put(s);
        } else if (vi_yank_linewise[s->qs->yank_current]) {
            /* linewise register: paste whole lines below the current line */
            do_eol(s);
            if (s->offset < s->b->total_size)
                s->offset = eb_next(s->b, s->offset);
            do_yank(s);
        } else {
            /* charwise register: paste after the character under the
             * cursor (before the newline at EOL), cursor on last char */
            int ch, next;
            ch = eb_nextc(s->b, s->offset, &next);
            if (ch != '\n')
                s->offset = next;
            do_yank(s);
            if (s->offset > 0)
                s->offset = eb_prev(s->b, s->offset);
        }
        return 1;
     case 'v':
         vi_visual_toggle(s, 0);
         return 1;
     case 'V':
         vi_visual_toggle(s, 1);
         return 1;
     case 'y':
         if (s->vi_visual_active)
             vi_visual_yank(s);
         else
             put_status(s, "Use v/V then y to yank");
         return 1;
     case 'd':
         if (s->vi_visual_active) {
             vi_visual_delete(s);
         } else {
             vi_set_pending(s, 'd');
         }
         return 1;
     case ':':
         s->vi_pending = 0;
         put_status(s, "-- NORMAL --");
         minibuffer_edit(s, "", ":", NULL, NULL, vi_ex_callback, s);
         return 1;
     case 'q': {
         /* Dismiss a stranded Wisdom popup when focus has moved back to
          * a text window.  Guard on no pending / no visual so `dq` and
          * visual flows keep their existing behavior. */
         EditState *qp;
         if (!s->vi_visual_active && !s->vi_pending) {
             qp = vi_find_wisdom_popup(s->qs);
             if (qp) {
                 do_popup_exit(qp);
                 return 1;
             }
         }
         return 1;
     }
     default:
        /* Swallow unknown printable ASCII so it doesn't self-insert */
        if (key >= ' ' && key <= '~')
            return 1;
        break;
    }

    return 0;
}

/*
 * Intercept a key for vi/evil mode.
 * Return 1 if the key was consumed, 0 to let normal QEmacs dispatch handle it.
 * The caller is responsible for resetting the key context when this returns 1.
 */
int vi_handle_key(EditState *s, int key)
{
    if (!(s->flags & WF_VI_NORMAL)) {
        /* Insert mode: ESC exits to normal. If the terminal layer combined
         * ESC with the next byte into a META key, treat that as ESC followed
         * by the base key. */
        if (key == KEY_ESC || key == 27 || key == KEY_CTRL('[')) {
            s->flags |= WF_VI_NORMAL;
            s->vi_pending = 0;
            put_status(s, "-- NORMAL --");
            return 1;
        }
        if (KEY_IS_META(key)) {
            s->flags |= WF_VI_NORMAL;
            s->vi_pending = 0;
            put_status(s, "-- NORMAL --");
            vi_normal_key(s, key & 0xFF);
            return 1;
        }
        return 0;
    }

     /* Normal mode: ESC / C-[ is a cancel/no-op. META keys are treated as
      * ESC followed by the base key, so ESC-h and similar chords work even
      * when the terminal layer composes them. */
    if (key == KEY_ESC || key == 27 || key == KEY_CTRL('[')) {
        if (s->vi_visual_active)
            vi_visual_end(s);
        else
            vi_set_pending(s, 0);
        return 1;
    }
    if (KEY_IS_META(key)) {
        if (s->vi_visual_active) {
            vi_visual_end(s);
            vi_normal_key(s, key & 0xFF);
        } else {
            vi_set_pending(s, 0);
            vi_normal_key(s, key & 0xFF);
        }
        return 1;
    }

    return vi_normal_key(s, key);
}

static const CmdDef vi_commands[] = {
    CMD0("vi-mode", "", "Toggle Vi/Evil modal editing", do_vi_mode)
    CMD0("vi-normal-mode", "", "Enter Vi normal mode", do_vi_normal_mode)
    CMD0("vi-insert-mode", "", "Enter Vi insert mode", do_vi_insert_mode)
    CMD0("wisdom", "f1", "Show a random wisdom quote", do_wisdom)
};

static int vi_init(QEmacsState *qs)
{
    qe_register_commands(qs, NULL, vi_commands, countof(vi_commands));
    return 0;
}

qe_module_init(vi_init);
