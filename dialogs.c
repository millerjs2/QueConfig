#include "dialogs.h"

void dialog_response_destroy(GtkDialog *d, int resp, gpointer u)
{
    (void)resp;
    (void)u;
    gtk_window_destroy(GTK_WINDOW(d));
}

void show_warning(AppData *a, const char *msg)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->window),
                                          GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_OK,
                                          "%s", msg);
    g_signal_connect(d, "response", G_CALLBACK(dialog_response_destroy), NULL);
    gtk_window_present(GTK_WINDOW(d));
}

void show_info(AppData *a, const char *msg)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(a->window),
                                          GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_INFO,
                                          GTK_BUTTONS_OK,
                                          "%s", msg);
    g_signal_connect(d, "response", G_CALLBACK(dialog_response_destroy), NULL);
    gtk_window_present(GTK_WINDOW(d));
}
