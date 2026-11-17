#include "terminal_widget.hpp"

#include <vterm.h>

#include <pango/pangocairo.h>
#include <glib-unix.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr int kInitialRows = 24;
constexpr int kInitialColumns = 80;
constexpr std::size_t kMaximumScrollbackLines = 5000;
constexpr const char* kWidgetDataKey = "lumaterm-terminal-engine";

struct TerminalEngine {
    GtkWidget* widget = nullptr;
    VTerm* vterm = nullptr;
    VTermState* state = nullptr;
    VTermScreen* screen = nullptr;
    int pty_fd = -1;
    GPid child_pid = 0;
    guint read_watch = 0;
    guint write_watch = 0;
    std::string pending_output;
    std::deque<std::vector<VTermScreenCell>> scrollback;
    std::size_t scrollback_offset = 0;
    int rows = kInitialRows;
    int columns = kInitialColumns;
    int cell_width = 9;
    int cell_height = 18;
    int baseline = 14;
    int font_size = 12;
    double font_scale = 1.0;
    bool cursor_visible = true;
    VTermPos cursor{0, 0};
    GdkRGBA default_foreground{0.91, 0.93, 0.97, 1.0};
    GdkRGBA default_background{0.04, 0.05, 0.07, 1.0};
    PangoFontDescription* font = nullptr;
};

TerminalEngine* engine_for(GtkWidget* widget) {
    return static_cast<TerminalEngine*>(
        g_object_get_data(G_OBJECT(widget), kWidgetDataKey));
}

void queue_pty_output(TerminalEngine* engine, const char* bytes, std::size_t length);

int on_screen_damage(VTermRect, void* user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (engine->widget) gtk_widget_queue_draw(engine->widget);
    return 1;
}

int on_screen_cursor(VTermPos position, VTermPos, int visible, void* user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    engine->cursor = position;
    engine->cursor_visible = visible != 0;
    if (engine->widget) gtk_widget_queue_draw(engine->widget);
    return 1;
}

int on_screen_title(VTermProp property, VTermValue* value, void* user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (property != VTERM_PROP_TITLE || !value || !engine->widget) return 1;

    const auto fragment = value->string;
    if (!fragment.str || fragment.len == 0) return 1;
    const std::string title(fragment.str, fragment.len);
    GtkWidget* top = gtk_widget_get_toplevel(engine->widget);
    if (GTK_IS_WINDOW(top)) gtk_window_set_title(GTK_WINDOW(top), title.c_str());
    return 1;
}

int on_screen_bell(void* user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (engine->widget) gdk_display_beep(gtk_widget_get_display(engine->widget));
    return 1;
}

int on_scrollback_push(int columns, const VTermScreenCell* cells, void* user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (!cells || columns <= 0) return 1;

    engine->scrollback.emplace_back(cells, cells + columns);
    while (engine->scrollback.size() > kMaximumScrollbackLines)
        engine->scrollback.pop_front();
    if (engine->scrollback_offset > 0)
        engine->scrollback_offset = std::min(
            engine->scrollback_offset + 1, engine->scrollback.size());
    return 1;
}

int on_scrollback_pop(int columns, VTermScreenCell* cells, void* user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (!cells || columns <= 0 || engine->scrollback.empty()) return 0;

    const auto& line = engine->scrollback.back();
    for (int column = 0; column < columns; ++column) {
        if (static_cast<std::size_t>(column) < line.size())
            cells[column] = line[static_cast<std::size_t>(column)];
        else
            std::memset(&cells[column], 0, sizeof(VTermScreenCell));
    }
    engine->scrollback.pop_back();
    engine->scrollback_offset = std::min(engine->scrollback_offset,
                                         engine->scrollback.size());
    return 1;
}

void write_ready(TerminalEngine* engine) {
    while (engine->pty_fd >= 0 && !engine->pending_output.empty()) {
        const ssize_t written = ::write(engine->pty_fd, engine->pending_output.data(),
                                        engine->pending_output.size());
        if (written > 0) {
            engine->pending_output.erase(0, static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        engine->pending_output.clear();
        break;
    }
}

gboolean on_pty_writable(gint, GIOCondition condition, gpointer user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        engine->write_watch = 0;
        engine->pending_output.clear();
        return G_SOURCE_REMOVE;
    }

    write_ready(engine);
    if (engine->pending_output.empty()) {
        engine->write_watch = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void queue_pty_output(TerminalEngine* engine, const char* bytes, std::size_t length) {
    if (!engine || engine->pty_fd < 0 || !bytes || length == 0) return;
    engine->pending_output.append(bytes, length);
    write_ready(engine);
    if (!engine->pending_output.empty() && engine->write_watch == 0) {
        engine->write_watch = g_unix_fd_add(
            engine->pty_fd, static_cast<GIOCondition>(G_IO_OUT | G_IO_ERR | G_IO_HUP),
            on_pty_writable, engine);
    }
}

void on_vterm_output(const char* bytes, std::size_t length, void* user) {
    queue_pty_output(static_cast<TerminalEngine*>(user), bytes, length);
}

void update_terminal_size(TerminalEngine* engine, int width, int height) {
    if (!engine || !engine->vterm || engine->cell_width <= 0 || engine->cell_height <= 0)
        return;

    const int columns = std::max(2, width / engine->cell_width);
    const int rows = std::max(1, height / engine->cell_height);
    if (columns == engine->columns && rows == engine->rows) return;

    engine->columns = columns;
    engine->rows = rows;
    vterm_set_size(engine->vterm, rows, columns);
    vterm_screen_flush_damage(engine->screen);

    if (engine->pty_fd >= 0) {
        struct winsize size{};
        size.ws_row = static_cast<unsigned short>(rows);
        size.ws_col = static_cast<unsigned short>(columns);
        ioctl(engine->pty_fd, TIOCSWINSZ, &size);
    }
}

void update_font_metrics(TerminalEngine* engine) {
    if (!engine || !engine->widget || !engine->font) return;

    const int scaled_size = std::max(
        6, static_cast<int>(std::lround(engine->font_size * engine->font_scale)));
    pango_font_description_set_size(engine->font, scaled_size * PANGO_SCALE);

    PangoContext* context = gtk_widget_get_pango_context(engine->widget);
    PangoFontMetrics* metrics = pango_context_get_metrics(
        context, engine->font, pango_context_get_language(context));
    engine->cell_width = std::max(
        1, PANGO_PIXELS_CEIL(pango_font_metrics_get_approximate_char_width(metrics)));
    const int ascent = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(metrics));
    const int descent = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(metrics));
    engine->cell_height = std::max(1, ascent + descent);
    engine->baseline = ascent;
    pango_font_metrics_unref(metrics);
}

void color_to_rgba(TerminalEngine* engine, const VTermColor& source,
                   bool foreground, GdkRGBA* result, bool* transparent) {
    if (foreground && VTERM_COLOR_IS_DEFAULT_FG(&source)) {
        *result = engine->default_foreground;
        *transparent = false;
        return;
    }
    if (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&source)) {
        *result = engine->default_background;
        *transparent = true;
        return;
    }

    VTermColor converted = source;
    vterm_screen_convert_color_to_rgb(engine->screen, &converted);
    result->red = converted.rgb.red / 255.0;
    result->green = converted.rgb.green / 255.0;
    result->blue = converted.rgb.blue / 255.0;
    result->alpha = 1.0;
    *transparent = false;
}

void draw_cell(TerminalEngine* engine, cairo_t* cr, PangoLayout* layout,
               int row, int column, const VTermScreenCell& cell) {
    if (cell.width <= 0) return;

    GdkRGBA foreground{};
    GdkRGBA background{};
    bool background_transparent = false;
    bool foreground_transparent = false;
    color_to_rgba(engine, cell.fg, true, &foreground, &foreground_transparent);
    color_to_rgba(engine, cell.bg, false, &background, &background_transparent);
    if (cell.attrs.reverse) {
        std::swap(foreground, background);
        std::swap(foreground_transparent, background_transparent);
    }

    const double x = column * engine->cell_width;
    const double y = row * engine->cell_height;
    const double cell_pixel_width = engine->cell_width * cell.width;

    if (!background_transparent) {
        gdk_cairo_set_source_rgba(cr, &background);
        cairo_rectangle(cr, x, y, cell_pixel_width, engine->cell_height);
        cairo_fill(cr);
    }

    if (cell.attrs.conceal) return;

    std::string text;
    for (uint32_t codepoint : cell.chars) {
        if (codepoint == 0) break;
        if (!g_unichar_validate(static_cast<gunichar>(codepoint))) continue;
        char encoded[8]{};
        const int length = g_unichar_to_utf8(static_cast<gunichar>(codepoint), encoded);
        text.append(encoded, static_cast<std::size_t>(length));
    }
    if (text.empty()) return;

    PangoFontDescription* font = pango_font_description_copy(engine->font);
    pango_font_description_set_weight(font,
        cell.attrs.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_style(font,
        cell.attrs.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));

    if (foreground_transparent) foreground = engine->default_foreground;
    gdk_cairo_set_source_rgba(cr, &foreground);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);

    if (cell.attrs.underline != VTERM_UNDERLINE_OFF || cell.attrs.strike) {
        cairo_set_line_width(cr, 1.0);
        if (cell.attrs.underline != VTERM_UNDERLINE_OFF) {
            cairo_move_to(cr, x, y + engine->cell_height - 2.0);
            cairo_line_to(cr, x + cell_pixel_width, y + engine->cell_height - 2.0);
        }
        if (cell.attrs.underline == VTERM_UNDERLINE_DOUBLE) {
            cairo_move_to(cr, x, y + engine->cell_height - 4.0);
            cairo_line_to(cr, x + cell_pixel_width, y + engine->cell_height - 4.0);
        }
        if (cell.attrs.strike) {
            cairo_move_to(cr, x, y + engine->cell_height * 0.52);
            cairo_line_to(cr, x + cell_pixel_width, y + engine->cell_height * 0.52);
        }
        cairo_stroke(cr);
    }
}

gboolean draw_terminal(GtkWidget* widget, cairo_t* cr, gpointer user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (!engine || !engine->screen) return FALSE;

    update_font_metrics(engine);
    const int width = gtk_widget_get_allocated_width(widget);
    const int height = gtk_widget_get_allocated_height(widget);
    update_terminal_size(engine, width, height);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* base_font = pango_font_description_copy(engine->font);
    pango_layout_set_font_description(layout, base_font);
    pango_font_description_free(base_font);

    const std::size_t offset = std::min(engine->scrollback_offset,
                                        engine->scrollback.size());
    for (int row = 0; row < engine->rows; ++row) {
        for (int column = 0; column < engine->columns; ++column) {
            VTermScreenCell cell{};
            bool have_cell = false;

            if (offset > 0 && static_cast<std::size_t>(row) < offset) {
                const std::size_t history_index =
                    engine->scrollback.size() - offset + static_cast<std::size_t>(row);
                if (history_index < engine->scrollback.size() &&
                    static_cast<std::size_t>(column) < engine->scrollback[history_index].size()) {
                    cell = engine->scrollback[history_index][static_cast<std::size_t>(column)];
                    have_cell = true;
                }
            } else {
                const VTermPos position{row - static_cast<int>(offset), column};
                if (position.row >= 0 && position.row < engine->rows)
                    have_cell = vterm_screen_get_cell(engine->screen, position, &cell) != 0;
            }

            if (have_cell) draw_cell(engine, cr, layout, row, column, cell);
        }
    }

    if (offset == 0 && engine->cursor_visible &&
        engine->cursor.row >= 0 && engine->cursor.row < engine->rows &&
        engine->cursor.col >= 0 && engine->cursor.col < engine->columns) {
        const double x = engine->cursor.col * engine->cell_width;
        const double y = engine->cursor.row * engine->cell_height;
        cairo_set_source_rgba(cr, engine->default_foreground.red,
                             engine->default_foreground.green,
                             engine->default_foreground.blue, 0.72);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, x + 0.5, y + 0.5,
                        engine->cell_width - 1.0, engine->cell_height - 1.0);
        cairo_stroke(cr);
    }

    g_object_unref(layout);
    return FALSE;
}

void child_exited(GPid pid, gint, gpointer user) {
    auto* widget = GTK_WIDGET(user);
    auto* engine = engine_for(widget);
    if (engine) {
        engine->child_pid = 0;
        gtk_main_quit();
    }
    g_spawn_close_pid(pid);
    g_object_unref(widget);
}

gboolean on_pty_readable(gint fd, GIOCondition condition, gpointer user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    char buffer[16384];

    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            engine->scrollback_offset = 0;
            vterm_input_write(engine->vterm, buffer, static_cast<std::size_t>(count));
            vterm_screen_flush_damage(engine->screen);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        engine->read_watch = 0;
        return G_SOURCE_REMOVE;
    }

    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        engine->read_watch = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

VTermModifier get_modifiers(GdkModifierType state) {
    VTermModifier modifiers = VTERM_MOD_NONE;
    if (state & GDK_SHIFT_MASK) modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_SHIFT);
    if (state & GDK_CONTROL_MASK) modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_CTRL);
    if (state & GDK_MOD1_MASK) modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_ALT);
    return modifiers;
}

bool send_special_key(TerminalEngine* engine, guint keyval, VTermModifier modifiers) {
    VTermKey key = VTERM_KEY_NONE;
    switch (keyval) {
        case GDK_KEY_Return: key = VTERM_KEY_ENTER; break;
        case GDK_KEY_KP_Enter: key = VTERM_KEY_KP_ENTER; break;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab: key = VTERM_KEY_TAB; break;
        case GDK_KEY_BackSpace: key = VTERM_KEY_BACKSPACE; break;
        case GDK_KEY_Escape: key = VTERM_KEY_ESCAPE; break;
        case GDK_KEY_Up: key = VTERM_KEY_UP; break;
        case GDK_KEY_Down: key = VTERM_KEY_DOWN; break;
        case GDK_KEY_Left: key = VTERM_KEY_LEFT; break;
        case GDK_KEY_Right: key = VTERM_KEY_RIGHT; break;
        case GDK_KEY_Insert: key = VTERM_KEY_INS; break;
        case GDK_KEY_Delete: key = VTERM_KEY_DEL; break;
        case GDK_KEY_Home: key = VTERM_KEY_HOME; break;
        case GDK_KEY_End: key = VTERM_KEY_END; break;
        case GDK_KEY_Page_Up: key = VTERM_KEY_PAGEUP; break;
        case GDK_KEY_Page_Down: key = VTERM_KEY_PAGEDOWN; break;
        case GDK_KEY_KP_0: key = VTERM_KEY_KP_0; break;
        case GDK_KEY_KP_1: key = VTERM_KEY_KP_1; break;
        case GDK_KEY_KP_2: key = VTERM_KEY_KP_2; break;
        case GDK_KEY_KP_3: key = VTERM_KEY_KP_3; break;
        case GDK_KEY_KP_4: key = VTERM_KEY_KP_4; break;
        case GDK_KEY_KP_5: key = VTERM_KEY_KP_5; break;
        case GDK_KEY_KP_6: key = VTERM_KEY_KP_6; break;
        case GDK_KEY_KP_7: key = VTERM_KEY_KP_7; break;
        case GDK_KEY_KP_8: key = VTERM_KEY_KP_8; break;
        case GDK_KEY_KP_9: key = VTERM_KEY_KP_9; break;
        case GDK_KEY_KP_Multiply: key = VTERM_KEY_KP_MULT; break;
        case GDK_KEY_KP_Add: key = VTERM_KEY_KP_PLUS; break;
        case GDK_KEY_KP_Subtract: key = VTERM_KEY_KP_MINUS; break;
        case GDK_KEY_KP_Decimal: key = VTERM_KEY_KP_PERIOD; break;
        case GDK_KEY_KP_Divide: key = VTERM_KEY_KP_DIVIDE; break;
        case GDK_KEY_KP_Equal: key = VTERM_KEY_KP_EQUAL; break;
        default:
            if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35)
                key = VTERM_KEY_FUNCTION(static_cast<int>(keyval - GDK_KEY_F1 + 1));
            break;
    }

    if (key == VTERM_KEY_NONE) return false;
    vterm_keyboard_key(engine->vterm, key, modifiers);
    return true;
}

void paste_received(GtkClipboard*, const gchar* text, gpointer user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (!engine || !text) return;
    vterm_keyboard_start_paste(engine->vterm);
    queue_pty_output(engine, text, std::strlen(text));
    vterm_keyboard_end_paste(engine->vterm);
}

gboolean on_terminal_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    const GdkModifierType state = static_cast<GdkModifierType>(
        event->state & gtk_accelerator_get_default_mod_mask());

    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) ==
        (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
        switch (event->keyval) {
            case GDK_KEY_plus:
            case GDK_KEY_equal:
            case GDK_KEY_KP_Add:
                engine->font_scale = std::min(2.5, engine->font_scale * 1.1);
                update_font_metrics(engine);
                gtk_widget_queue_draw(widget);
                return TRUE;
            case GDK_KEY_minus:
            case GDK_KEY_KP_Subtract:
                engine->font_scale = std::max(0.5, engine->font_scale / 1.1);
                update_font_metrics(engine);
                gtk_widget_queue_draw(widget);
                return TRUE;
            case GDK_KEY_0:
            case GDK_KEY_KP_0:
                engine->font_scale = 1.0;
                update_font_metrics(engine);
                gtk_widget_queue_draw(widget);
                return TRUE;
            case GDK_KEY_c:
            case GDK_KEY_C:
                // Text selection/copy is implemented separately from the
                // terminal parser. Do not turn this desktop shortcut into ^C.
                return TRUE;
            case GDK_KEY_v:
            case GDK_KEY_V:
                gtk_clipboard_request_text(gtk_widget_get_clipboard(widget,
                    GDK_SELECTION_CLIPBOARD), paste_received, engine);
                return TRUE;
            default:
                break;
        }
    }

    if ((event->keyval == GDK_KEY_Page_Up || event->keyval == GDK_KEY_Page_Down) &&
        (state & GDK_SHIFT_MASK)) {
        const std::size_t page = static_cast<std::size_t>(std::max(1, engine->rows / 2));
        if (event->keyval == GDK_KEY_Page_Up) {
            engine->scrollback_offset = std::min(
                engine->scrollback.size(), engine->scrollback_offset + page);
        } else {
            engine->scrollback_offset =
                engine->scrollback_offset > page ? engine->scrollback_offset - page : 0;
        }
        gtk_widget_queue_draw(widget);
        return TRUE;
    }

    engine->scrollback_offset = 0;
    const VTermModifier modifiers = get_modifiers(state);
    if (send_special_key(engine, event->keyval, modifiers)) return TRUE;

    const gunichar unicode = gdk_keyval_to_unicode(event->keyval);
    if (unicode != 0) {
        vterm_keyboard_unichar(engine->vterm, unicode, modifiers);
        return TRUE;
    }
    return FALSE;
}

gboolean on_terminal_scroll(GtkWidget* widget, GdkEventScroll* event, gpointer user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (event->direction == GDK_SCROLL_UP) {
        engine->scrollback_offset = std::min(
            engine->scrollback.size(), engine->scrollback_offset + 3);
    } else if (event->direction == GDK_SCROLL_DOWN) {
        engine->scrollback_offset = engine->scrollback_offset > 3
            ? engine->scrollback_offset - 3 : 0;
    } else if (event->direction == GDK_SCROLL_SMOOTH) {
        if (event->delta_y < 0)
            engine->scrollback_offset = std::min(
                engine->scrollback.size(), engine->scrollback_offset + 3);
        else
            engine->scrollback_offset = engine->scrollback_offset > 3
                ? engine->scrollback_offset - 3 : 0;
    }
    gtk_widget_queue_draw(widget);
    return TRUE;
}

void on_widget_destroy(GtkWidget*, gpointer user) {
    auto* engine = static_cast<TerminalEngine*>(user);
    if (!engine) return;

    if (engine->read_watch) {
        g_source_remove(engine->read_watch);
        engine->read_watch = 0;
    }
    if (engine->write_watch) {
        g_source_remove(engine->write_watch);
        engine->write_watch = 0;
    }
    if (engine->child_pid > 0) kill(-engine->child_pid, SIGHUP);
    if (engine->pty_fd >= 0) {
        close(engine->pty_fd);
        engine->pty_fd = -1;
    }
    engine->pending_output.clear();
}

void destroy_terminal_engine(gpointer data) {
    auto* engine = static_cast<TerminalEngine*>(data);
    if (!engine) return;

    if (engine->read_watch) g_source_remove(engine->read_watch);
    if (engine->write_watch) g_source_remove(engine->write_watch);
    if (engine->child_pid > 0) kill(-engine->child_pid, SIGHUP);
    if (engine->pty_fd >= 0) close(engine->pty_fd);
    if (engine->vterm) vterm_free(engine->vterm);
    if (engine->font) pango_font_description_free(engine->font);
    delete engine;
}

bool spawn_shell(TerminalEngine* engine, const std::string& shell, GError** error) {
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Could not open a pseudoterminal: %s", g_strerror(errno));
        return false;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        const int saved_errno = errno;
        close(master);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not prepare pseudoterminal: %s", g_strerror(saved_errno));
        return false;
    }

    char* slave_name = ptsname(master);
    if (!slave_name) {
        const int saved_errno = errno;
        close(master);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not locate pseudoterminal slave: %s", g_strerror(saved_errno));
        return false;
    }
    const std::string slave_path(slave_name);
    gchar* shell_basename = g_path_get_basename(shell.c_str());
    const std::string login_name = std::string("-") + shell_basename;
    g_free(shell_basename);

    const pid_t child = fork();
    if (child < 0) {
        const int saved_errno = errno;
        close(master);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Could not start the shell: %s", g_strerror(saved_errno));
        return false;
    }

    if (child == 0) {
        close(master);
        if (setsid() < 0) _exit(126);
        const int slave = open(slave_path.c_str(), O_RDWR);
        if (slave < 0) _exit(126);
        ioctl(slave, TIOCSCTTY, 0);

        struct termios terminal_settings{};
        if (tcgetattr(slave, &terminal_settings) == 0) {
            cfmakeraw(&terminal_settings);
            terminal_settings.c_oflag |= OPOST;
            terminal_settings.c_lflag |= (ISIG | ICANON | ECHO | ECHOE | ECHOK);
            tcsetattr(slave, TCSANOW, &terminal_settings);
        }

        struct winsize size{};
        size.ws_row = kInitialRows;
        size.ws_col = kInitialColumns;
        ioctl(slave, TIOCSWINSZ, &size);

        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);

        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execl(shell.c_str(), login_name.c_str(), static_cast<char*>(nullptr));
        execl(shell.c_str(), shell.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    const int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) fcntl(master, F_SETFL, flags | O_NONBLOCK);
    engine->pty_fd = master;
    engine->child_pid = child;
    engine->read_watch = g_unix_fd_add(
        master, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
        on_pty_readable, engine);

    // Keep the widget alive until GLib has reaped the child process.
    g_child_watch_add(static_cast<GPid>(child), child_exited,
                      g_object_ref(engine->widget));
    return true;
}

} // namespace

GtkWidget* terminal_widget_new(const std::string& shell,
                               const std::string& font_family,
                               int font_size,
                               const GdkRGBA& foreground,
                               const GdkRGBA& background,
                               bool monochrome_palette) {
    GtkWidget* widget = gtk_drawing_area_new();
    auto* engine = new TerminalEngine();
    engine->widget = widget;
    engine->font_size = std::clamp(font_size, 6, 72);
    engine->default_foreground = foreground;
    engine->default_background = background;
    engine->font = pango_font_description_new();
    pango_font_description_set_family(engine->font, font_family.c_str());
    pango_font_description_set_size(engine->font, engine->font_size * PANGO_SCALE);

    engine->vterm = vterm_new(kInitialRows, kInitialColumns);
    if (!engine->vterm) {
        pango_font_description_free(engine->font);
        delete engine;
        return nullptr;
    }
    vterm_set_utf8(engine->vterm, 1);
    engine->state = vterm_obtain_state(engine->vterm);
    engine->screen = vterm_obtain_screen(engine->vterm);
    vterm_screen_enable_altscreen(engine->screen, 1);
    vterm_screen_set_damage_merge(engine->screen, VTERM_DAMAGE_ROW);

    VTermColor default_foreground{};
    VTermColor default_background{};
    vterm_color_rgb(&default_foreground,
        static_cast<uint8_t>(std::lround(foreground.red * 255.0)),
        static_cast<uint8_t>(std::lround(foreground.green * 255.0)),
        static_cast<uint8_t>(std::lround(foreground.blue * 255.0)));
    vterm_color_rgb(&default_background,
        static_cast<uint8_t>(std::lround(background.red * 255.0)),
        static_cast<uint8_t>(std::lround(background.green * 255.0)),
        static_cast<uint8_t>(std::lround(background.blue * 255.0)));
    vterm_screen_set_default_colors(engine->screen,
                                   &default_foreground, &default_background);

    if (monochrome_palette) {
        VTermColor white{};
        vterm_color_rgb(&white, 255, 255, 255);
        for (int index = 0; index < 256; ++index)
            vterm_state_set_palette_color(engine->state, index, &white);
    }

    static const VTermScreenCallbacks callbacks = [] {
        VTermScreenCallbacks value{};
        value.damage = on_screen_damage;
        value.movecursor = on_screen_cursor;
        value.settermprop = on_screen_title;
        value.bell = on_screen_bell;
        value.sb_pushline = on_scrollback_push;
        value.sb_popline = on_scrollback_pop;
        return value;
    }();
    vterm_screen_set_callbacks(engine->screen, &callbacks, engine);
    vterm_output_set_callback(engine->vterm, on_vterm_output, engine);
    vterm_screen_reset(engine->screen, 1);

    g_object_set_data_full(G_OBJECT(widget), kWidgetDataKey, engine,
                           destroy_terminal_engine);
    gtk_widget_set_name(widget, "lumaterm-terminal");
    gtk_widget_set_can_focus(widget, TRUE);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    gtk_widget_set_margin_start(widget, 12);
    gtk_widget_set_margin_end(widget, 12);
    gtk_widget_set_margin_top(widget, 10);
    gtk_widget_set_margin_bottom(widget, 10);
    gtk_widget_add_events(widget, GDK_KEY_PRESS_MASK | GDK_SCROLL_MASK |
                                  GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(widget, "draw", G_CALLBACK(draw_terminal), engine);
    g_signal_connect(widget, "key-press-event",
                     G_CALLBACK(on_terminal_key_press), engine);
    g_signal_connect(widget, "scroll-event",
                     G_CALLBACK(on_terminal_scroll), engine);
    g_signal_connect(widget, "destroy",
                     G_CALLBACK(on_widget_destroy), engine);

    update_font_metrics(engine);
    GError* error = nullptr;
    if (!spawn_shell(engine, shell, &error)) {
        GtkWidget* dialog = gtk_message_dialog_new(
            nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not start the shell: %s",
            error ? error->message : "Unknown pseudoterminal error");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (error) g_error_free(error);
        gtk_widget_destroy(widget);
        g_object_unref(widget);
        return nullptr;
    }
    return widget;
}

void terminal_widget_set_font_scale(GtkWidget* widget, double scale) {
    TerminalEngine* engine = engine_for(widget);
    if (!engine) return;
    engine->font_scale = std::clamp(scale, 0.5, 2.5);
    update_font_metrics(engine);
    gtk_widget_queue_draw(widget);
}
