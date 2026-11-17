#pragma once

#include <gtk/gtk.h>

#include <string>

// Creates LumaTerm's own GTK terminal view backed by libvterm and a PTY.
// The returned widget owns the terminal engine and its child process.
GtkWidget* terminal_widget_new(const std::string& shell,
                               const std::string& font_family,
                               int font_size,
                               const GdkRGBA& foreground,
                               const GdkRGBA& background,
                               bool monochrome_palette);

// Changes the terminal font scale without rebuilding the terminal state.
void terminal_widget_set_font_scale(GtkWidget* widget, double scale);
