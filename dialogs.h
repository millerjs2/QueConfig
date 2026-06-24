#ifndef DIALOGS_H
#define DIALOGS_H

#include "app_data.h"

void dialog_response_destroy(GtkDialog *d, int resp, gpointer u);
void show_warning(AppData *a, const char *msg);
void show_info(AppData *a, const char *msg);

#endif
