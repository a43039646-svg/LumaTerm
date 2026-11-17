#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

constexpr const char* kGroup = "appearance";

struct SettingsWindow {
    GtkWidget* window = nullptr;
    GtkWidget* mode = nullptr;
    GtkWidget* path = nullptr;
    GtkWidget* browse = nullptr;
    GtkWidget* fit = nullptr;
    GtkWidget* background_color = nullptr;
    GtkWidget* opacity = nullptr;
    GtkWidget* blur = nullptr;
    GtkWidget* glass = nullptr;
    GtkWidget* font = nullptr;
    GtkWidget* text_color = nullptr;
    GtkWidget* monochrome = nullptr;
    GtkWidget* status = nullptr;
    std::string config_path;
    GKeyFile* config = nullptr;
};

std::string get_string(GKeyFile* config, const char* key, const char* fallback) {
    gchar* value = g_key_file_get_string(config, kGroup, key, nullptr);
    if (!value) return fallback;
    std::string result = value;
    g_free(value);
    return result;
}

int get_integer(GKeyFile* config, const char* key, int fallback) {
    GError* error = nullptr;
    const int value = g_key_file_get_integer(config, kGroup, key, &error);
    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

double get_double(GKeyFile* config, const char* key, double fallback) {
    GError* error = nullptr;
    const double value = g_key_file_get_double(config, kGroup, key, &error);
    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

bool get_boolean(GKeyFile* config, const char* key, bool fallback) {
    GError* error = nullptr;
    const gboolean value = g_key_file_get_boolean(config, kGroup, key, &error);
    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

GdkRGBA get_color(GKeyFile* config, const char* key, const char* fallback) {
    GdkRGBA color{};
    const std::string value = get_string(config, key, fallback);
    if (!gdk_rgba_parse(&color, value.c_str())) gdk_rgba_parse(&color, fallback);
    return color;
}

GtkWidget* make_scale(double lower, double upper, double step, double value) {
    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                lower, upper, step);
    gtk_range_set_value(GTK_RANGE(scale), value);
    gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
    gtk_widget_set_hexpand(scale, TRUE);
    return scale;
}

void add_row(GtkGrid* grid, int row, const char* title, GtkWidget* control) {
    GtkWidget* label = gtk_label_new(title);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    gtk_grid_attach(grid, control, 1, row, 1, 1);
}

void update_mode_sensitivity(SettingsWindow* ui) {
    const gchar* mode = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->mode));
    const bool image_mode = mode && g_str_equal(mode, "image");
    const bool has_file = mode && !g_str_equal(mode, "color");
    gtk_widget_set_sensitive(ui->path, has_file);
    gtk_widget_set_sensitive(ui->browse, has_file);
    gtk_widget_set_sensitive(ui->fit, image_mode);
}

void choose_background(GtkButton*, gpointer data) {
    auto* ui = static_cast<SettingsWindow*>(data);
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Choose a background", GTK_WINDOW(ui->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL, "Choose", GTK_RESPONSE_ACCEPT, nullptr);
    const gchar* old_path = gtk_entry_get_text(GTK_ENTRY(ui->path));
    if (old_path && *old_path) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), old_path);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gtk_entry_set_text(GTK_ENTRY(ui->path), filename);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}

std::string color_to_hex(const GdkRGBA& color) {
    gchar hex[8];
    g_snprintf(hex, sizeof(hex), "#%02x%02x%02x",
        static_cast<unsigned>(std::lround(color.red * 255.0)),
        static_cast<unsigned>(std::lround(color.green * 255.0)),
        static_cast<unsigned>(std::lround(color.blue * 255.0)));
    return hex;
}

void save_settings(GtkButton*, gpointer data) {
    auto* ui = static_cast<SettingsWindow*>(data);
    const gchar* mode = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->mode));
    const gchar* fit = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->fit));
    const gchar* path = gtk_entry_get_text(GTK_ENTRY(ui->path));
    GdkRGBA background{}, foreground{};
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(ui->background_color), &background);
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(ui->text_color), &foreground);

    g_key_file_set_string(ui->config, kGroup, "background_mode", mode ? mode : "image");
    g_key_file_set_string(ui->config, kGroup, "background", path ? path : "");
    g_key_file_set_string(ui->config, kGroup, "background_fit", fit ? fit : "cover");
    const std::string bg_hex = color_to_hex(background);
    const std::string fg_hex = color_to_hex(foreground);
    g_key_file_set_string(ui->config, kGroup, "background_color", bg_hex.c_str());
    g_key_file_set_string(ui->config, kGroup, "text_color", fg_hex.c_str());
    g_key_file_set_double(ui->config, kGroup, "background_opacity",
                          gtk_range_get_value(GTK_RANGE(ui->opacity)));
    g_key_file_set_integer(ui->config, kGroup, "background_blur",
                           static_cast<int>(std::lround(
                               gtk_range_get_value(GTK_RANGE(ui->blur)))));
    g_key_file_set_boolean(ui->config, kGroup, "glass",
                           gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->glass)));
    g_key_file_set_boolean(ui->config, kGroup, "monochrome_text",
                           gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->monochrome)));

    gchar* font_name = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(ui->font));
    PangoFontDescription* font = pango_font_description_from_string(
        font_name ? font_name : "Monospace 12");
    const gchar* family = pango_font_description_get_family(font);
    const int font_size = std::clamp(
        PANGO_PIXELS(pango_font_description_get_size(font)), 6, 72);
    g_key_file_set_string(ui->config, kGroup, "font_family",
                          family ? family : "monospace");
    g_key_file_set_integer(ui->config, kGroup, "font_size", font_size);
    pango_font_description_free(font);
    g_free(font_name);

    gsize length = 0;
    GError* error = nullptr;
    gchar* contents = g_key_file_to_data(ui->config, &length, &error);
    if (contents) {
        gchar* directory = g_path_get_dirname(ui->config_path.c_str());
        g_mkdir_with_parents(directory, 0700);
        const gboolean saved = g_file_set_contents(ui->config_path.c_str(), contents,
            static_cast<gssize>(length), &error);
        g_free(directory);
        g_free(contents);
        if (saved) {
            const std::string status = "Saved to " + ui->config_path +
                ". Restart LumaTerm to use the new appearance.";
            gtk_label_set_text(GTK_LABEL(ui->status), status.c_str());
        }
    }
    if (error) {
        gtk_label_set_text(GTK_LABEL(ui->status), error->message);
        g_error_free(error);
    }
}

void load_settings(SettingsWindow* ui) {
    ui->config = g_key_file_new();
    const gchar* config_dir = g_get_user_config_dir();
    ui->config_path = std::string(config_dir) + "/lumaterm/settings.ini";
    g_key_file_load_from_file(ui->config, ui->config_path.c_str(),
        static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS),
        nullptr);

    const std::string mode = get_string(ui->config, "background_mode", "image");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->mode), mode.c_str());
    gtk_entry_set_text(GTK_ENTRY(ui->path),
                       get_string(ui->config, "background", "").c_str());
    const std::string fit = get_string(ui->config, "background_fit", "cover");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->fit), fit.c_str());
    GdkRGBA bg = get_color(ui->config, "background_color", "#10131b");
    GdkRGBA fg = get_color(ui->config, "text_color", "#e8eaf0");
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ui->background_color), &bg);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ui->text_color), &fg);
    gtk_range_set_value(GTK_RANGE(ui->opacity), std::clamp(
        get_double(ui->config, "background_opacity", 0.85), 0.0, 1.0));
    gtk_range_set_value(GTK_RANGE(ui->blur), std::clamp(
        get_integer(ui->config, "background_blur", 0), 0, 10));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->glass),
                                 get_boolean(ui->config, "glass", false));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->monochrome),
                                 get_boolean(ui->config, "monochrome_text", false));

    const std::string family = get_string(ui->config, "font_family", "Monospace");
    const int size = std::clamp(get_integer(ui->config, "font_size", 12), 6, 72);
    const std::string font_name = family + " " + std::to_string(size);
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(ui->font), font_name.c_str());
    update_mode_sensitivity(ui);
}

GtkWidget* make_page_grid() {
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_widget_set_margin_start(grid, 18);
    gtk_widget_set_margin_end(grid, 18);
    gtk_widget_set_margin_top(grid, 18);
    gtk_widget_set_margin_bottom(grid, 18);
    return grid;
}

}  // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    SettingsWindow ui;
    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.window), "LumaTerm Settings");
    gtk_window_set_default_size(GTK_WINDOW(ui.window), 620, 460);
    gtk_window_set_position(GTK_WINDOW(ui.window), GTK_WIN_POS_CENTER);
    g_signal_connect(ui.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
#ifdef LUMATERM_ICON_PATH
    gtk_window_set_icon_from_file(GTK_WINDOW(ui.window), LUMATERM_ICON_PATH, nullptr);
#endif

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_container_add(GTK_CONTAINER(ui.window), root);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<big><b>LumaTerm Settings</b></big>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), title, FALSE, FALSE, 0);

    GtkWidget* notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(root), notebook, TRUE, TRUE, 0);

    GtkWidget* appearance = make_page_grid();
    ui.mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.mode), "image", "Image or GIF");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.mode), "video", "Video");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.mode), "color", "Solid color");
    add_row(GTK_GRID(appearance), 0, "Background type", ui.mode);

    GtkWidget* file_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.path = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.path), "Path to image, GIF, or video");
    gtk_widget_set_hexpand(ui.path, TRUE);
    ui.browse = gtk_button_new_with_label("Browse…");
    gtk_box_pack_start(GTK_BOX(file_row), ui.path, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(file_row), ui.browse, FALSE, FALSE, 0);
    add_row(GTK_GRID(appearance), 1, "Background file", file_row);
    g_signal_connect(ui.browse, "clicked", G_CALLBACK(choose_background), &ui);

    ui.fit = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.fit), "cover", "Fill and crop");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.fit), "contain", "Fit inside");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.fit), "stretch", "Stretch");
    add_row(GTK_GRID(appearance), 2, "Image sizing", ui.fit);

    ui.background_color = gtk_color_button_new();
    gtk_color_button_set_title(GTK_COLOR_BUTTON(ui.background_color), "Background color");
    add_row(GTK_GRID(appearance), 3, "Background color", ui.background_color);
    ui.opacity = make_scale(0.0, 1.0, 0.05, 0.85);
    gtk_scale_set_digits(GTK_SCALE(ui.opacity), 2);
    add_row(GTK_GRID(appearance), 4, "Background opacity", ui.opacity);
    gtk_widget_set_tooltip_text(ui.opacity,
        "0 is fully transparent; 1 is fully opaque. Requires a compositor for desktop transparency.");
    ui.blur = make_scale(0.0, 10.0, 1.0, 0.0);
    gtk_scale_set_digits(GTK_SCALE(ui.blur), 0);
    add_row(GTK_GRID(appearance), 5, "Background blur", ui.blur);
    gtk_widget_set_tooltip_text(ui.blur,
        "Three-pass soft blur. Animated backgrounds are blurred frame by frame.");
    ui.glass = gtk_check_button_new_with_label("Glass window (requires a compositor)");
    gtk_grid_attach(GTK_GRID(appearance), ui.glass, 1, 6, 1, 1);
    gtk_widget_set_tooltip_text(ui.glass,
        "Allows the desktop to show through transparent parts of the window; requires compositor support.");
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), appearance,
                             gtk_label_new("Background"));

    GtkWidget* text = make_page_grid();
    ui.font = gtk_font_button_new_with_font("Monospace 12");
    gtk_font_button_set_show_style(GTK_FONT_BUTTON(ui.font), FALSE);
    add_row(GTK_GRID(text), 0, "Terminal font", ui.font);
    ui.text_color = gtk_color_button_new();
    gtk_color_button_set_title(GTK_COLOR_BUTTON(ui.text_color), "Text color");
    add_row(GTK_GRID(text), 1, "Text color", ui.text_color);
    ui.monochrome = gtk_check_button_new_with_label("Use white ANSI colors too");
    gtk_widget_set_tooltip_text(ui.monochrome,
        "Makes standard ANSI-colored output, such as Fastfetch, monochrome.");
    gtk_grid_attach(GTK_GRID(text), ui.monochrome, 1, 2, 1, 1);
    GtkWidget* note = gtk_label_new(
        "Font zoom shortcuts: Ctrl+Shift+=, Ctrl+Shift+- and Ctrl+Shift+0");
    gtk_widget_set_halign(note, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(text), note, 1, 3, 1, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), text, gtk_label_new("Text"));

    ui.status = gtk_label_new("Changes are saved to your LumaTerm config.");
    gtk_widget_set_halign(ui.status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(root), ui.status, FALSE, FALSE, 0);

    GtkWidget* buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(buttons), 8);
    GtkWidget* close = gtk_button_new_with_label("Close");
    GtkWidget* save = gtk_button_new_with_label("Save settings");
    gtk_widget_set_can_default(save, TRUE);
    gtk_container_add(GTK_CONTAINER(buttons), close);
    gtk_container_add(GTK_CONTAINER(buttons), save);
    gtk_box_pack_start(GTK_BOX(root), buttons, FALSE, FALSE, 0);
    g_signal_connect(close, "clicked", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(save, "clicked", G_CALLBACK(save_settings), &ui);
    g_signal_connect(ui.mode, "changed", G_CALLBACK(+[](GtkComboBox*, gpointer data) {
        update_mode_sensitivity(static_cast<SettingsWindow*>(data));
    }), &ui);

    load_settings(&ui);
    gtk_widget_show_all(ui.window);
    gtk_widget_grab_default(save);
    gtk_main();

    g_key_file_unref(ui.config);
    return 0;
}
