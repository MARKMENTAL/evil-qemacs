#include "qe.h"

/*
 * Evil/Vi modal editing layer for QEmacs.
 *
 * This is intentionally small and self-contained: it intercepts keys in
 * normal mode, passes everything through in insert mode, and provides a
 * minimal :ex command line via the existing minibuffer machinery.
 */

/* Ex command prompt callback */
static void vi_ex_callback(void *opaque, char *buf, CompletionDef *completion)
{
    EditState *s = opaque;
    const char *p;

    if (!buf) {
        /* aborted */
        return;
    }

    p = buf;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == ':')
        p++;

    if (strcmp(p, "q") == 0 || strcmp(p, "quit") == 0) {
        do_exit_qemacs(s, NO_ARG);
    } else if (strcmp(p, "q!") == 0 || strcmp(p, "quit!") == 0) {
        do_exit_qemacs(s, 1);
    } else if (strcmp(p, "wq") == 0 || strcmp(p, "x") == 0) {
        do_save_buffer(s);
        do_exit_qemacs(s, NO_ARG);
    } else if (strcmp(p, "w") == 0 || strcmp(p, "write") == 0) {
        do_save_buffer(s);
    } else {
        put_status(s, "Not an ex command: %s", p);
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
    /* Pending multi-key commands (dd, gg, ...) */
    if (s->vi_pending == 'd') {
        if (key == 'd') {
            do_kill_whole_line(s, 1);
        } else {
            put_status(s, "Unknown d command");
        }
        s->vi_pending = 0;
        return 1;
    }
    if (s->vi_pending == 'g') {
        if (key == 'g') {
            do_bof(s);
        } else {
            put_status(s, "Unknown g command");
        }
        s->vi_pending = 0;
        return 1;
    }

    switch (key) {
    case 'i':
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
    case 'a':
        do_left_right(s, 1);
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
    case 'o':
        do_eol(s);
        do_char(s, '\n', 1);
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
    case 'O':
        do_bol(s);
        do_open_line(s);
        s->flags &= ~WF_VI_NORMAL;
        s->vi_pending = 0;
        put_status(s, "-- INSERT --");
        return 1;
    case 'h':
        do_left_right(s, -1);
        return 1;
    case 'j':
        do_up_down(s, 1);
        return 1;
    case 'k':
        do_up_down(s, -1);
        return 1;
    case 'l':
        do_left_right(s, 1);
        return 1;
    case 'w':
        do_word_left_right(s, 1);
        return 1;
    case 'b':
        do_word_left_right(s, -1);
        return 1;
    case '0':
        do_bol(s);
        return 1;
    case '^':
        do_bol_nspace(s);
        return 1;
    case '$':
        do_eol(s);
        return 1;
    case 'G':
        do_eof(s);
        return 1;
    case 'x':
        do_delete_char(s, 1);
        return 1;
    case 'u':
        do_undo(s);
        return 1;
    case 'd':
        s->vi_pending = 'd';
        return 1;
    case 'g':
        s->vi_pending = 'g';
        return 1;
    case ':':
        minibuffer_edit(s, "", ":", NULL, NULL, vi_ex_callback, s);
        return 1;
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
        s->vi_pending = 0;
        put_status(s, "-- NORMAL --");
        return 1;
    }
    if (KEY_IS_META(key)) {
        s->vi_pending = 0;
        vi_normal_key(s, key & 0xFF);
        return 1;
    }

    return vi_normal_key(s, key);
}

static const CmdDef vi_commands[] = {
    CMD0("vi-mode", "", "Toggle Vi/Evil modal editing", do_vi_mode)
    CMD0("vi-normal-mode", "", "Enter Vi normal mode", do_vi_normal_mode)
    CMD0("vi-insert-mode", "", "Enter Vi insert mode", do_vi_insert_mode)
};

static int vi_init(QEmacsState *qs)
{
    qe_register_commands(qs, NULL, vi_commands, countof(vi_commands));
    return 0;
}

qe_module_init(vi_init);
