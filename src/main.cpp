#include <gtk/gtk.h>
#include <gst/gst.h>
#include <vte/vte.h>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

constexpr const char* kConfigGroup = "appearance";
constexpr const char* kBackgroundKey = "background";

struct App {
    GtkWidget* window = nullptr;
    GtkWidget* terminal = nullptr;
    GtkWidget* background = nullptr;
    GdkPixbuf* background_pixbuf = nullptr;
    GdkPixbufAnimation* background_animation = nullptr;
    GdkPixbufAnimationIter* animation_iter = nullptr;
    GdkPixbuf* rendered_background = nullptr;
    GstElement* video_player = nullptr;
    GtkWidget* video_widget = nullptr;
    int rendered_width = 0;
    int rendered_height = 0;
    guint background_request = 0;
    guint animation_timeout = 0;
    guint video_bus_watch = 0;
    GKeyFile* settings = nullptr;
    std::string config_path;
    std::string background_path;
    std::string background_mode = "image";
    std::string background_fit = "cover";
    std::string font_family = "monospace";
    int font_size = 12;
    int background_blur = 0;
    double background_opacity = 0.85;
    bool glass = false;
    bool monochrome_text = false;
    GdkRGBA background_color{0.035, 0.045, 0.065, 1.0};
    GdkRGBA text_color{0.91, 0.93, 0.97, 1.0};
};

struct BackgroundLoad {
    std::string path;
    guint request;
};

void clear_rendered_background(App* app) {
    if (app->rendered_background) {
        g_object_unref(app->rendered_background);
        app->rendered_background = nullptr;
    }
    app->rendered_width = 0;
    app->rendered_height = 0;
}

void stop_background_animation(App* app) {
    if (app->animation_timeout) {
        g_source_remove(app->animation_timeout);
        app->animation_timeout = 0;
    }
    if (app->animation_iter) {
        g_object_unref(app->animation_iter);
        app->animation_iter = nullptr;
    }
    if (app->background_animation) {
        g_object_unref(app->background_animation);
        app->background_animation = nullptr;
    }
}

void show_animation_frame(App* app) {
    GdkPixbuf* frame = gdk_pixbuf_animation_iter_get_pixbuf(app->animation_iter);
    if (!frame) return;

    const int frame_width = gdk_pixbuf_get_width(frame);
    const int frame_height = gdk_pixbuf_get_height(frame);
    const double factor = std::min({1.0, 1600.0 / frame_width, 1000.0 / frame_height});
    GdkPixbuf* scaled = frame;
    if (factor < 1.0) {
        scaled = gdk_pixbuf_scale_simple(frame,
            std::max(1, static_cast<int>(frame_width * factor)),
            std::max(1, static_cast<int>(frame_height * factor)), GDK_INTERP_BILINEAR);
    } else {
        g_object_ref(scaled);
    }
    if (!scaled) return;

    if (app->background_pixbuf) g_object_unref(app->background_pixbuf);
    app->background_pixbuf = scaled;
    clear_rendered_background(app);
    gtk_widget_queue_draw(app->background);
}

gboolean advance_animation(gpointer data) {
    auto* app = static_cast<App*>(data);
    gdk_pixbuf_animation_iter_advance(app->animation_iter, nullptr);
    show_animation_frame(app);
    const int delay = gdk_pixbuf_animation_iter_get_delay_time(app->animation_iter);
    app->animation_timeout = g_timeout_add(std::max(delay, 20), advance_animation, app);
    return G_SOURCE_REMOVE;
}

// Three horizontal/vertical box-blur passes approximate a Gaussian blur.
// The caller downsamples first, keeping animated GIF backgrounds inexpensive.
GdkPixbuf* box_blur_pixbuf(GdkPixbuf* source, int radius) {
    if (radius <= 0) return gdk_pixbuf_copy(source);

    const int width = gdk_pixbuf_get_width(source);
    const int height = gdk_pixbuf_get_height(source);
    const int channels = gdk_pixbuf_get_n_channels(source);
    if (width <= 1 && height <= 1) return gdk_pixbuf_copy(source);

    radius = std::clamp(radius, 1, 12);
    const int window_size = radius * 2 + 1;
    GdkPixbuf* first = gdk_pixbuf_copy(source);
    GdkPixbuf* second = gdk_pixbuf_copy(source);
    if (!first || !second) {
        if (first) g_object_unref(first);
        if (second) g_object_unref(second);
        return nullptr;
    }

    for (int pass = 0; pass < 3; ++pass) {
        const guchar* input = gdk_pixbuf_get_pixels(first);
        guchar* horizontal = gdk_pixbuf_get_pixels(second);
        const int input_stride = gdk_pixbuf_get_rowstride(first);
        const int horizontal_stride = gdk_pixbuf_get_rowstride(second);

        // Horizontal sliding-window average.
        for (int y = 0; y < height; ++y) {
            for (int channel = 0; channel < channels; ++channel) {
                int sum = 0;
                for (int offset = -radius; offset <= radius; ++offset) {
                    const int sample_x = std::clamp(offset, 0, width - 1);
                    sum += input[y * input_stride + sample_x * channels + channel];
                }
                for (int x = 0; x < width; ++x) {
                    horizontal[y * horizontal_stride + x * channels + channel] =
                        static_cast<guchar>((sum + window_size / 2) / window_size);
                    const int leaving_x = std::clamp(x - radius, 0, width - 1);
                    const int entering_x = std::clamp(x + radius + 1, 0, width - 1);
                    sum -= input[y * input_stride + leaving_x * channels + channel];
                    sum += input[y * input_stride + entering_x * channels + channel];
                }
            }
        }

        // Vertical sliding-window average.
        const guchar* vertical_input = gdk_pixbuf_get_pixels(second);
        guchar* output = gdk_pixbuf_get_pixels(first);
        const int vertical_stride = gdk_pixbuf_get_rowstride(second);
        const int output_stride = gdk_pixbuf_get_rowstride(first);
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < channels; ++channel) {
                int sum = 0;
                for (int offset = -radius; offset <= radius; ++offset) {
                    const int sample_y = std::clamp(offset, 0, height - 1);
                    sum += vertical_input[sample_y * vertical_stride +
                                          x * channels + channel];
                }
                for (int y = 0; y < height; ++y) {
                    output[y * output_stride + x * channels + channel] =
                        static_cast<guchar>((sum + window_size / 2) / window_size);
                    const int leaving_y = std::clamp(y - radius, 0, height - 1);
                    const int entering_y = std::clamp(y + radius + 1, 0, height - 1);
                    sum -= vertical_input[leaving_y * vertical_stride +
                                          x * channels + channel];
                    sum += vertical_input[entering_y * vertical_stride +
                                          x * channels + channel];
                }
            }
        }
    }

    g_object_unref(second);
    return first;
}

gboolean draw_background(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto* app = static_cast<App*>(data);
    const int width = gtk_widget_get_allocated_width(widget);
    const int height = gtk_widget_get_allocated_height(widget);
    if (width <= 0 || height <= 0) return FALSE;

    // Keep an opaque base when glass mode is disabled. In glass mode the
    // compositor shows through whatever part of the background is transparent.
    if (!app->glass) {
        gdk_cairo_set_source_rgba(cr, &app->background_color);
        cairo_paint(cr);
    }
    if (app->background_mode == "color" || !app->background_pixbuf) {
        gdk_cairo_set_source_rgba(cr, &app->background_color);
        cairo_paint_with_alpha(cr, app->glass ? app->background_opacity : 1.0);
        return FALSE;
    }

    const int image_width = gdk_pixbuf_get_width(app->background_pixbuf);
    const int image_height = gdk_pixbuf_get_height(app->background_pixbuf);
    if (image_width <= 0 || image_height <= 0) return FALSE;

    if (app->rendered_background &&
        (app->rendered_width != width || app->rendered_height != height)) {
        clear_rendered_background(app);
    }

    // Scale once per window size; VTE redraws often while commands are running.
    if (!app->rendered_background) {
        double scale_x = static_cast<double>(width) / image_width;
        double scale_y = static_cast<double>(height) / image_height;
        if (app->background_fit != "stretch") {
            const double scale = app->background_fit == "contain"
                ? std::min(scale_x, scale_y) : std::max(scale_x, scale_y);
            scale_x = scale_y = scale;
        }
        const int scaled_width = std::max(1, static_cast<int>(image_width * scale_x));
        const int scaled_height = std::max(1, static_cast<int>(image_height * scale_y));
        GdkPixbuf* cover = gdk_pixbuf_scale_simple(
            app->background_pixbuf, scaled_width, scaled_height, GDK_INTERP_BILINEAR);
        if (!cover) return FALSE;
        const int blur = std::clamp(app->background_blur, 0, 10);
        if (blur > 0) {
            // Work on a smaller copy, then scale it back up. This keeps the
            // per-frame cost of a moving GIF background bounded.
            const int divisor = 1 + std::min(3, (blur + 2) / 3);
            GdkPixbuf* small = gdk_pixbuf_scale_simple(
                cover, std::max(1, scaled_width / divisor),
                std::max(1, scaled_height / divisor), GDK_INTERP_BILINEAR);
            if (small) {
                GdkPixbuf* softened = box_blur_pixbuf(
                    small, std::max(1, blur / divisor));
                if (softened) {
                    app->rendered_background = gdk_pixbuf_scale_simple(
                        softened, scaled_width, scaled_height, GDK_INTERP_BILINEAR);
                    g_object_unref(softened);
                }
                g_object_unref(small);
            }
            if (app->rendered_background) {
                g_object_unref(cover);
            } else {
                // Fall back to the unblurred image if allocation fails.
                app->rendered_background = cover;
            }
        } else {
            app->rendered_background = cover;
        }
        if (!app->rendered_background) return FALSE;
        app->rendered_width = width;
        app->rendered_height = height;
    }

    const int scaled_width = gdk_pixbuf_get_width(app->rendered_background);
    const int scaled_height = gdk_pixbuf_get_height(app->rendered_background);
    const int x = (width - scaled_width) / 2;
    const int y = (height - scaled_height) / 2;
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    gdk_cairo_set_source_pixbuf(cr, app->rendered_background, x, y);
    cairo_paint_with_alpha(cr, app->background_opacity);
    cairo_restore(cr);
    return FALSE;
}

void load_background_worker(GTask* task, gpointer, gpointer task_data,
                            GCancellable*) {
    auto* load = static_cast<BackgroundLoad*>(task_data);
    GError* error = nullptr;
    // Decode image files off the GTK thread so large files don't freeze the window.
    GdkPixbufAnimation* animation = gdk_pixbuf_animation_new_from_file(
        load->path.c_str(), &error);
    if (!animation) {
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, animation, g_object_unref);
}

void background_loaded(GObject*, GAsyncResult* result, gpointer data) {
    auto* app = static_cast<App*>(data);
    auto* task = G_TASK(result);
    auto* load = static_cast<BackgroundLoad*>(g_task_get_task_data(task));
    GError* error = nullptr;
    auto* animation = static_cast<GdkPixbufAnimation*>(
        g_task_propagate_pointer(task, &error));

    if (load->request == app->background_request) {
        if (animation) {
            stop_background_animation(app);
            if (app->background_pixbuf) {
                g_object_unref(app->background_pixbuf);
                app->background_pixbuf = nullptr;
            }
            app->background_animation = animation;
            app->animation_iter = gdk_pixbuf_animation_get_iter(animation, nullptr);
            show_animation_frame(app);
            if (!gdk_pixbuf_animation_is_static_image(animation)) {
                const int delay = gdk_pixbuf_animation_iter_get_delay_time(app->animation_iter);
                app->animation_timeout = g_timeout_add(
                    std::max(delay, 20), advance_animation, app);
            }
        } else if (error) {
            GtkWidget* dialog = gtk_message_dialog_new(
                GTK_WINDOW(app->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                "Could not load the background image: %s", error->message);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
    } else if (animation) {
        g_object_unref(animation);
    }
    if (error) g_error_free(error);
}

void start_background_load(App* app, const char* path) {
    auto* load = new BackgroundLoad{path, ++app->background_request};
    GTask* task = g_task_new(app->window, nullptr, background_loaded, app);
    g_task_set_task_data(task, load, [](gpointer data) {
        delete static_cast<BackgroundLoad*>(data);
    });
    g_task_run_in_thread(task, load_background_worker);
    g_object_unref(task);
}

void show_message(App* app, GtkMessageType type, const char* message) {
    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window), GTK_DIALOG_DESTROY_WITH_PARENT,
        type, GTK_BUTTONS_CLOSE, "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

GstElement* create_video_blur_filter(int strength) {
    if (strength <= 0) return nullptr;

    GstElement* bin = gst_bin_new("lumaterm-video-filter");
    GstElement* to_blur = gst_element_factory_make("videoconvert", nullptr);
    GstElement* blur = gst_element_factory_make("gaussianblur", nullptr);
    GstElement* from_blur = gst_element_factory_make("videoconvert", nullptr);
    if (!bin || !to_blur || !blur || !from_blur) {
        if (bin) gst_object_unref(bin);
        if (to_blur) gst_object_unref(to_blur);
        if (blur) gst_object_unref(blur);
        if (from_blur) gst_object_unref(from_blur);
        return nullptr;
    }

    g_object_set(blur, "sigma", std::max(0.5, strength * 1.2), nullptr);
    gst_bin_add_many(GST_BIN(bin), to_blur, blur, from_blur, nullptr);
    if (!gst_element_link_many(to_blur, blur, from_blur, nullptr)) {
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad* input = gst_element_get_static_pad(to_blur, "sink");
    GstPad* output = gst_element_get_static_pad(from_blur, "src");
    GstPad* ghost_input = input ? gst_ghost_pad_new("sink", input) : nullptr;
    GstPad* ghost_output = output ? gst_ghost_pad_new("src", output) : nullptr;
    if (input) gst_object_unref(input);
    if (output) gst_object_unref(output);
    if (!ghost_input || !ghost_output ||
        !gst_element_add_pad(bin, ghost_input) ||
        !gst_element_add_pad(bin, ghost_output)) {
        if (ghost_input && GST_OBJECT_PARENT(ghost_input) == nullptr) gst_object_unref(ghost_input);
        if (ghost_output && GST_OBJECT_PARENT(ghost_output) == nullptr) gst_object_unref(ghost_output);
        gst_object_unref(bin);
        return nullptr;
    }
    return bin;
}

gboolean on_video_bus_message(GstBus*, GstMessage* message, gpointer data) {
    auto* app = static_cast<App*>(data);
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_EOS:
            gst_element_seek_simple(app->video_player, GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
            break;
        case GST_MESSAGE_ERROR: {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            show_message(app, GTK_MESSAGE_ERROR,
                error ? error->message : "Video background playback failed.");
            if (error) g_error_free(error);
            g_free(debug);
            gst_element_set_state(app->video_player, GST_STATE_NULL);
            app->video_bus_watch = 0;
            return G_SOURCE_REMOVE;
        }
        default:
            break;
    }
    return G_SOURCE_CONTINUE;
}

gboolean on_terminal_key_press(GtkWidget*, GdkEventKey* event, gpointer data) {
    auto* terminal = VTE_TERMINAL(data);
    const GdkModifierType modifiers = static_cast<GdkModifierType>(
        event->state & gtk_accelerator_get_default_mod_mask());
    if ((modifiers & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) !=
        (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) return FALSE;

    double scale = vte_terminal_get_font_scale(terminal);
    switch (event->keyval) {
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:
            scale = std::min(2.5, scale * 1.1);
            break;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract:
            scale = std::max(0.5, scale / 1.1);
            break;
        case GDK_KEY_0:
        case GDK_KEY_KP_0:
            scale = 1.0;
            break;
        default:
            return FALSE;
    }
    vte_terminal_set_font_scale(terminal, scale);
    return TRUE;
}

void start_video_background(App* app, GtkWidget* overlay, const char* path) {
    GError* error = nullptr;
    gchar* uri = g_filename_to_uri(path, nullptr, &error);
    if (!uri) {
        const std::string message = error ? error->message : "Invalid video path.";
        show_message(app, GTK_MESSAGE_ERROR, message.c_str());
        if (error) g_error_free(error);
        return;
    }

    app->video_player = gst_element_factory_make("playbin", "lumaterm-video-player");
    GstElement* gtk_sink = gst_element_factory_make("gtksink", "lumaterm-video-sink");
    GstElement* audio_sink = gst_element_factory_make("fakesink", "lumaterm-silent-audio");
    if (!app->video_player || !gtk_sink || !audio_sink) {
        show_message(app, GTK_MESSAGE_ERROR,
            "Video playback needs GStreamer and its GTK 3 video sink. Install gstreamer1.0-gtk3.");
        if (app->video_player) {
            gst_object_unref(app->video_player);
            app->video_player = nullptr;
        }
        if (gtk_sink) gst_object_unref(gtk_sink);
        if (audio_sink) gst_object_unref(audio_sink);
        g_free(uri);
        return;
    }

    g_object_get(gtk_sink, "widget", &app->video_widget, nullptr);
    if (!app->video_widget) {
        show_message(app, GTK_MESSAGE_ERROR, "GStreamer could not create its video widget.");
        gst_object_unref(app->video_player);
        app->video_player = nullptr;
        gst_object_unref(gtk_sink);
        gst_object_unref(audio_sink);
        g_free(uri);
        return;
    }

    gtk_widget_set_hexpand(app->video_widget, TRUE);
    gtk_widget_set_vexpand(app->video_widget, TRUE);
    gtk_widget_set_halign(app->video_widget, GTK_ALIGN_FILL);
    gtk_widget_set_valign(app->video_widget, GTK_ALIGN_FILL);
    gtk_widget_set_opacity(app->video_widget, app->background_opacity);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app->video_widget);
    g_object_unref(app->video_widget); // GtkOverlay now owns the widget.

    GstElement* filter = create_video_blur_filter(app->background_blur);
    if (app->background_blur > 0 && !filter) {
        show_message(app, GTK_MESSAGE_WARNING,
            "Video blur is unavailable. Install gstreamer1.0-plugins-bad; the video will play without blur.");
    }
    if (filter) {
        g_object_set(app->video_player, "video-filter", filter, nullptr);
        gst_object_unref(filter);
    }
    g_object_set(app->video_player,
        "uri", uri, "video-sink", gtk_sink, "audio-sink", audio_sink, nullptr);
    gst_object_unref(gtk_sink);
    gst_object_unref(audio_sink);
    g_free(uri);

    GstBus* bus = gst_element_get_bus(app->video_player);
    app->video_bus_watch = gst_bus_add_watch(bus, on_video_bus_message, app);
    gst_object_unref(bus);
    gst_element_set_state(app->video_player, GST_STATE_PLAYING);
}

void on_child_exited(VteTerminal*, gint, gpointer data) {
    gtk_main_quit();
    (void)data;
}

double config_double(GKeyFile* file, const char* key, double fallback) {
    GError* error = nullptr;
    const double value = g_key_file_get_double(file, kConfigGroup, key, &error);
    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

int config_integer(GKeyFile* file, const char* key, int fallback) {
    GError* error = nullptr;
    const int value = g_key_file_get_integer(file, kConfigGroup, key, &error);
    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

bool config_boolean(GKeyFile* file, const char* key, bool fallback) {
    GError* error = nullptr;
    const gboolean value = g_key_file_get_boolean(file, kConfigGroup, key, &error);
    if (error) {
        g_error_free(error);
        return fallback;
    }
    return value;
}

std::string config_string(GKeyFile* file, const char* key, const char* fallback) {
    gchar* value = g_key_file_get_string(file, kConfigGroup, key, nullptr);
    if (!value) return fallback;
    std::string result = value;
    g_free(value);
    return result;
}

void load_settings(App* app) {
    app->settings = g_key_file_new();
    const gchar* config_dir = g_get_user_config_dir();
    app->config_path = std::string(config_dir) + "/lumaterm/settings.ini";
    g_key_file_load_from_file(app->settings, app->config_path.c_str(),
                              G_KEY_FILE_NONE, nullptr);
    app->background_path = config_string(app->settings, kBackgroundKey, "");
    app->background_mode = config_string(app->settings, "background_mode", "image");
    app->background_fit = config_string(app->settings, "background_fit", "cover");
    app->font_family = config_string(app->settings, "font_family", "monospace");
    app->font_size = std::clamp(config_integer(app->settings, "font_size", 12), 6, 72);
    app->background_blur = std::clamp(
        config_integer(app->settings, "background_blur", 0), 0, 10);
    app->background_opacity = std::clamp(
        config_double(app->settings, "background_opacity", 0.85), 0.0, 1.0);
    app->glass = config_boolean(app->settings, "glass", false);
    app->monochrome_text = config_boolean(app->settings, "monochrome_text", false);

    const std::string bg = config_string(app->settings, "background_color", "#090b10");
    const std::string fg = config_string(app->settings, "text_color", "#e8eaf0");
    gdk_rgba_parse(&app->background_color, bg.c_str());
    gdk_rgba_parse(&app->text_color, fg.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    gtk_init(&argc, &argv);

    App app;
    load_settings(&app);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "");
    if (app.glass) {
        GdkScreen* screen = gtk_widget_get_screen(app.window);
        GdkVisual* rgba_visual = gdk_screen_get_rgba_visual(screen);
        if (rgba_visual) {
            gtk_widget_set_visual(app.window, rgba_visual);
            gtk_widget_set_app_paintable(app.window, TRUE);
        }
    }
#ifdef LUMATERM_ICON_PATH
    GError* icon_error = nullptr;
    if (!gtk_window_set_icon_from_file(GTK_WINDOW(app.window), LUMATERM_ICON_PATH,
                                       &icon_error)) {
        gtk_window_set_icon_name(GTK_WINDOW(app.window), "lumaterm");
        if (icon_error) g_error_free(icon_error);
    }
#endif
    gtk_window_set_default_size(GTK_WINDOW(app.window), 900, 560);
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget* layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), layout);

    GtkWidget* overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(layout), overlay, TRUE, TRUE, 0);
    app.background = gtk_drawing_area_new();
    gtk_widget_set_hexpand(app.background, TRUE);
    gtk_widget_set_vexpand(app.background, TRUE);
    gtk_container_add(GTK_CONTAINER(overlay), app.background);
    g_signal_connect(app.background, "draw", G_CALLBACK(draw_background), &app);
    if (app.background_mode == "video" && !app.background_path.empty()) {
        start_video_background(&app, overlay, app.background_path.c_str());
    } else if (app.background_mode != "color" && !app.background_path.empty()) {
        start_background_load(&app, app.background_path.c_str());
    }

    app.terminal = vte_terminal_new();
    gtk_widget_set_name(app.terminal, "lumaterm-terminal");
    gtk_widget_set_hexpand(app.terminal, TRUE);
    gtk_widget_set_vexpand(app.terminal, TRUE);
    gtk_widget_set_margin_start(app.terminal, 12);
    gtk_widget_set_margin_end(app.terminal, 12);
    gtk_widget_set_margin_top(app.terminal, 10);
    gtk_widget_set_margin_bottom(app.terminal, 10);
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(app.terminal), 5000);
    vte_terminal_set_cursor_blink_mode(VTE_TERMINAL(app.terminal), VTE_CURSOR_BLINK_ON);
    vte_terminal_set_font_scale(VTE_TERMINAL(app.terminal), 1.0);

    PangoFontDescription* font = pango_font_description_new();
    pango_font_description_set_family(font, app.font_family.c_str());
    pango_font_description_set_size(font, app.font_size * PANGO_SCALE);
    vte_terminal_set_font(VTE_TERMINAL(app.terminal), font);
    pango_font_description_free(font);
    GdkRGBA monochrome_palette[256];
    const GdkRGBA* palette = nullptr;
    gsize palette_size = 0;
    if (app.monochrome_text) {
        for (GdkRGBA& color : monochrome_palette) color = {1.0, 1.0, 1.0, 1.0};
        palette = monochrome_palette;
        palette_size = G_N_ELEMENTS(monochrome_palette);
    }
    vte_terminal_set_colors(VTE_TERMINAL(app.terminal), &app.text_color,
                            &app.background_color, palette, palette_size);
    vte_terminal_set_clear_background(VTE_TERMINAL(app.terminal), FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app.terminal);

    gchar* shell = g_strdup(g_getenv("SHELL") ? g_getenv("SHELL") : "/bin/sh");
    gchar* argv_shell[] = {shell, nullptr};
    vte_terminal_spawn_async(VTE_TERMINAL(app.terminal), VTE_PTY_DEFAULT,
                             nullptr, argv_shell, nullptr, (GSpawnFlags)0,
                             nullptr, nullptr, nullptr, -1, nullptr, nullptr,
                             nullptr);
    g_free(shell);
    g_signal_connect(app.terminal, "child-exited", G_CALLBACK(on_child_exited), &app);
    g_signal_connect(app.terminal, "key-press-event",
                     G_CALLBACK(on_terminal_key_press), app.terminal);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window { background-color: transparent; }"
        "#lumaterm-terminal { background-color: transparent; }",
        -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    gtk_widget_show_all(app.window);
    gtk_widget_grab_focus(app.terminal);
    gtk_main();

    if (app.video_bus_watch) g_source_remove(app.video_bus_watch);
    if (app.video_player) {
        gst_element_set_state(app.video_player, GST_STATE_NULL);
        gst_object_unref(app.video_player);
    }
    stop_background_animation(&app);
    if (app.background_pixbuf) g_object_unref(app.background_pixbuf);
    clear_rendered_background(&app);
    g_key_file_unref(app.settings);
    return 0;
}
