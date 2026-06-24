/*
 * main.c – GTK 4 GUI for the Quectel LG290P / LG580P Configurator
 */

#include "quectel.h"
#include "nmea_status.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <poll.h>

#include "app_data.h"
#include "dialogs.h"

static AppData *G = NULL;

/* ═══════════════════════════════════════════════════════════
 * BAUD-RATE HELPERS
 * ═══════════════════════════════════════════════════════════ */

static const int BAUD_TABLE[] = { 9600, 115200, 230400, 460800, 921600 };
#define NUM_BAUDS ((int)(sizeof BAUD_TABLE / sizeof BAUD_TABLE[0]))

static int baud_to_idx(int b)
{
    for (int i = 0; i < NUM_BAUDS; i++)
        if (BAUD_TABLE[i] == b) return i;
    return 3;
}

static int get_selected_baud(AppData *a)
{
    guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->baud_dd));
    return (i < (guint)NUM_BAUDS) ? BAUD_TABLE[i] : 460800;
}

/* ═══════════════════════════════════════════════════════════
 * FORWARD DECLARATIONS
 * ═══════════════════════════════════════════════════════════ */

static void rebuild_tabs(AppData *a);
static void stop_nmea_poll(AppData *a);

/* ═══════════════════════════════════════════════════════════
 * DARK-MODE DETECTION
 * ═══════════════════════════════════════════════════════════ */

static bool detect_dark_mode(void)
{
    GtkSettings *s = gtk_settings_get_default();
    if (!s) return false;

    gboolean d = FALSE;
    g_object_get(s, "gtk-application-prefer-dark-theme", &d, NULL);
    if (d) return true;

    char *t = NULL;
    g_object_get(s, "gtk-theme-name", &t, NULL);
    if (t) {
        char *lo = g_ascii_strdown(t, -1);
        bool r = strstr(lo, "dark") != NULL;
        g_free(lo);
        g_free(t);
        if (r) return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════
 * CSS
 * ═══════════════════════════════════════════════════════════ */

static const char *APP_CSS_TEMPLATE =
    "textview.log-view text { background-color: #1e1e1e; }"
    "textview.log-view {"
    "  font-family: 'Source Code Pro','Consolas',monospace;"
    "  font-size: 9pt;"
    "}"
    ".doc-label-light { color: #004080; font-size: 10pt; }"
    ".doc-label-dark  { color: #5da9e9; font-size: 10pt; }"
    ".bm-label-light  { color: #0050a0; font-size: small; }"
    ".bm-label-dark   { color: #4ba3e3; font-size: small; }"
    ".hint-label      { color: gray;    font-size: small; }";

static void setup_css(void)
{
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, APP_CSS_TEMPLATE);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static const char *css_doc(const AppData *a)
{
    return a->dark_mode ? "doc-label-dark" : "doc-label-light";
}

static const char *css_bm(const AppData *a)
{
    return a->dark_mode ? "bm-label-dark" : "bm-label-light";
}

/* ═══════════════════════════════════════════════════════════
 * LOGGING → GtkTextView
 * ═══════════════════════════════════════════════════════════ */

static void gui_log(int level, const char *msg)
{
    if (!G || !G->log_tv) return;
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(G->log_tv));

    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_date_time_format(now, "%H:%M:%S");
    char full[1024];
    snprintf(full, sizeof full, "%s | %s\n", ts, msg);
    g_free(ts);
    g_date_time_unref(now);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);

    const char *tag;
    if (level == LOG_ERROR)        tag = "error";
    else if (level == LOG_WARNING) tag = "warning";
    else                           tag = "normal";

    gtk_text_buffer_insert_with_tags_by_name(
        buf, &end, full, -1, tag, NULL);

    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark *m = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(G->log_tv), m);
    gtk_text_buffer_delete_mark(buf, m);
}

/* ═══════════════════════════════════════════════════════════
 * APP SETTINGS
 * ═══════════════════════════════════════════════════════════ */

static void load_settings(AppData *a)
{
    strcpy(a->saved_port, "/dev/ttyUSB0");
    a->saved_baud = 460800;

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, "app_config.json", NULL)) {
        JsonObject *o = json_node_get_object(json_parser_get_root(p));
        if (json_object_has_member(o, "port"))
            g_strlcpy(a->saved_port,
                      json_object_get_string_member(o, "port"),
                      sizeof a->saved_port);
        if (json_object_has_member(o, "baudrate"))
            a->saved_baud =
                (int)json_object_get_int_member(o, "baudrate");
    }
    g_object_unref(p);
}

static void save_settings(AppData *a, const char *port, int baud)
{
    g_strlcpy(a->saved_port, port, sizeof a->saved_port);
    a->saved_baud = baud;

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "port");
    json_builder_add_string_value(b, port);
    json_builder_set_member_name(b, "baudrate");
    json_builder_add_int_value(b, baud);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, json_builder_get_root(b));
    json_generator_to_file(gen, "app_config.json", NULL);
    g_object_unref(gen);
    g_object_unref(b);
}

/* ═══════════════════════════════════════════════════════════
 * DIALOG HELPERS
 * ═══════════════════════════════════════════════════════════ */

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

G_GNUC_END_IGNORE_DEPRECATIONS

static bool require_conn(AppData *a)
{
    if (quectel_is_connected(&a->dev)) return true;
    show_warning(a, "Please connect to the module first.");
    return false;
}

static void flush_events(void)
{
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
}

static bool model_excluded(const ConfigEntry *e, const char *model)
{
    for (int i = 0; i < e->num_exclude; i++)
        if (e->exclude_models[i] &&
            strcmp(e->exclude_models[i], model) == 0)
            return true;
    return false;
}

/* ═══════════════════════════════════════════════════════════
 * NMEA POLL CALLBACK
 * ═══════════════════════════════════════════════════════════ */

static gboolean nmea_poll_cb(gpointer data)
{
    AppData *a = data;

    if (!quectel_is_connected(&a->dev)) {
        a->nmea_poll_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (a->dev.command_busy)
        return G_SOURCE_CONTINUE;

    struct pollfd pfd = { .fd = a->dev.fd, .events = POLLIN };
    bool updated = false;

    while (poll(&pfd, 1, 0) > 0) {
        char chunk[256];
        ssize_t n = read(a->dev.fd, chunk, sizeof chunk);
        if (n <= 0) break;

        for (ssize_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n') {
                a->nmea_line_buf[a->nmea_line_pos] = '\0';
                if (a->nmea_line_pos > 0 &&
                    a->nmea_line_buf[a->nmea_line_pos - 1] == '\r')
                    a->nmea_line_buf[--a->nmea_line_pos] = '\0';

                if (a->nmea_line_pos > 0 &&
                    a->nmea_line_buf[0] == '$' &&
                    strncmp(a->nmea_line_buf, "$PQTM", 5) != 0)
                {
                    nmea_process_line(&a->nmea_status,
                                      a->nmea_line_buf);
                    updated = true;
                    if (a->show_nmea_in_console)
                        gui_log(LOG_INFO, a->nmea_line_buf);
                }
                a->nmea_line_pos = 0;
            } else if (c != '\r') {
                if (a->nmea_line_pos < NMEA_LINE_BUF_SIZE - 1)
                    a->nmea_line_buf[a->nmea_line_pos++] = c;
            }
        }
    }

    if (updated)
        nmea_status_update_display(&a->nmea_status,
                                    &a->status_widgets);
    return G_SOURCE_CONTINUE;
}

static void start_nmea_poll(AppData *a)
{
    nmea_status_init(&a->nmea_status);
    a->nmea_line_pos = 0;
    if (a->nmea_poll_id == 0)
        a->nmea_poll_id = g_timeout_add(100, nmea_poll_cb, a);
}

static void stop_nmea_poll(AppData *a)
{
    if (a->nmea_poll_id) {
        g_source_remove(a->nmea_poll_id);
        a->nmea_poll_id = 0;
    }
}

/* ═══════════════════════════════════════════════════════════
 * CONNECTION TOGGLE
 * ═══════════════════════════════════════════════════════════ */

static void on_connect(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;

    if (quectel_is_connected(&a->dev)) {
        stop_nmea_poll(a);
        quectel_disconnect(&a->dev);
        gtk_button_set_label(GTK_BUTTON(a->connect_btn), "Connect");
    } else {
        const char *port = gtk_editable_get_text(
                                GTK_EDITABLE(a->port_entry));
        int baud = get_selected_baud(a);
        if (quectel_connect(&a->dev, port, baud)) {
            gtk_button_set_label(GTK_BUTTON(a->connect_btn),
                                 "Disconnect");
            save_settings(a, port, baud);
            rebuild_tabs(a);
            start_nmea_poll(a);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * BITMASK DECODER
 * ═══════════════════════════════════════════════════════════ */

static void on_bm_changed(GtkEditable *ed, gpointer ud)
{
    BmCbData *cb = ud;
    const char *txt = gtk_editable_get_text(ed);
    if (!txt || !*txt) {
        gtk_label_set_text(GTK_LABEL(cb->label), "");
        return;
    }
    char *end;
    unsigned long v = strtoul(txt, &end, 16);
    if (*end) {
        gtk_label_set_text(GTK_LABEL(cb->label), "[Invalid Hex]");
        return;
    }
    GString *s = g_string_new("[");
    bool first = true;
    for (int i = 0; i < cb->bm->num_bits; i++) {
        if (v & (1u << cb->bm->bits[i].bit)) {
            if (!first) g_string_append(s, ", ");
            g_string_append(s, cb->bm->bits[i].name);
            first = false;
        }
    }
    g_string_append_c(s, ']');
    gtk_label_set_text(GTK_LABEL(cb->label),
                       first ? "[Disabled]" : s->str);
    g_string_free(s, TRUE);
}

/* ═══════════════════════════════════════════════════════════
 * CONFIG FORM – READ / WRITE
 * ═══════════════════════════════════════════════════════════ */

static const char *get_widget_val(GtkWidget *w, const FieldDef *fdef) {
    static char ret_buf[16][32];
    static int rb_idx = 0;
    rb_idx = (rb_idx + 1) % 16;
    char *out = ret_buf[rb_idx];

    if (fdef && fdef->type == FIELD_DROPDOWN) {
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(w));
        if (sel < (guint)fdef->num_opts) return fdef->opts[sel].val;
        return "";
    }
    else if (fdef && fdef->type == FIELD_CHECKBOX) {
        return gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? "1" : "0";
    }
    else if (fdef && fdef->type == FIELD_RADIO) {
        GtkWidget *child = gtk_widget_get_first_child(w);
        while (child) {
            if (GTK_IS_CHECK_BUTTON(child) && gtk_check_button_get_active(GTK_CHECK_BUTTON(child))) {
                return (const char *)g_object_get_data(G_OBJECT(child), "val");
            }
            child = gtk_widget_get_next_sibling(child);
        }
        return "";
    }
    else if (fdef && fdef->type == FIELD_BITMASK) {
        unsigned int val = 0;
        GtkWidget *child = gtk_widget_get_first_child(w);
        while (child) {
            if (GTK_IS_CHECK_BUTTON(child)) {
                if (gtk_check_button_get_active(GTK_CHECK_BUTTON(child))) {
                    int bit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "bit"));
                    val |= (1U << bit);
                }
            }
            child = gtk_widget_get_next_sibling(child);
        }
        snprintf(out, 32, "%08X", val);
        return out;
    }
    if (GTK_IS_EDITABLE(w)) {
        return gtk_editable_get_text(GTK_EDITABLE(w));
    }
    return "";
}

static void set_widget_val(GtkWidget *w, const FieldDef *fdef, const char *val) {
    if (fdef && fdef->type == FIELD_DROPDOWN) {
        for (int j = 0; j < fdef->num_opts; j++) {
            if (strcmp(fdef->opts[j].val, val) == 0) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(w), j);
                return;
            }
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w), 0);
    }
    else if (fdef && fdef->type == FIELD_CHECKBOX) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(w), (val && val[0] == '1'));
    }
    else if (fdef && fdef->type == FIELD_RADIO) {
        GtkWidget *child = gtk_widget_get_first_child(w);
        while (child) {
            if (GTK_IS_CHECK_BUTTON(child)) {
                const char *cval = (const char *)g_object_get_data(G_OBJECT(child), "val");
                if (cval && val && strcmp(cval, val) == 0) {
                    gtk_check_button_set_active(GTK_CHECK_BUTTON(child), TRUE);
                }
            }
            child = gtk_widget_get_next_sibling(child);
        }
    }
    else if (fdef && fdef->type == FIELD_BITMASK) {
        unsigned int num = val ? strtoul(val, NULL, 16) : 0;
        GtkWidget *child = gtk_widget_get_first_child(w);
        while (child) {
            if (GTK_IS_CHECK_BUTTON(child)) {
                int bit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "bit"));
                gtk_check_button_set_active(GTK_CHECK_BUTTON(child), (num & (1U << bit)) != 0);
            }
            child = gtk_widget_get_next_sibling(child);
        }
    }
    else {
        gtk_editable_set_text(GTK_EDITABLE(w), val ? val : "");
    }
}

static void on_form_read(GtkButton *btn, gpointer ud)
{
    (void)btn;
    FormData *fd = ud;
    AppData  *a  = fd->app;
    if (!require_conn(a)) return;

    int ncfg;
    const ConfigEntry *entries = quectel_get_config_entries(&ncfg);
    const ConfigEntry *e = &entries[fd->cfg_idx];
    int idx_len = (e->num_queries > 0) ? e->queries[0].num_args : 0;

    const char *args[MAX_QUERY_ARGS];
    for (int i = 0; i < idx_len; i++)
        args[i] = get_widget_val(fd->entries[i], &e->field_defs[i]);

    int n = 0;
    char **vals = quectel_read_config(&a->dev, e->cmd_name,
                                       idx_len, args, &n);
    if (vals) {
        for (int i = 0; i < n && i < e->num_fields; i++) {
            set_widget_val(fd->entries[i], &e->field_defs[i], vals[i]);
            g_free(vals[i]);
        }
        g_free(vals);
    }
}

static void on_form_write(GtkButton *btn, gpointer ud)
{
    (void)btn;
    FormData *fd = ud;
    AppData  *a  = fd->app;
    if (!require_conn(a)) return;

    int ncfg;
    const ConfigEntry *entries = quectel_get_config_entries(&ncfg);
    const ConfigEntry *e = &entries[fd->cfg_idx];

    const char *args[MAX_CFG_FIELDS];
    int argc = 0;
    for (int i = 0; i < e->num_fields; i++) {
        const char *t = get_widget_val(fd->entries[i], &e->field_defs[i]);
        if (t && *t)
            args[argc++] = t;
    }
    if (argc > 0)
        quectel_write_config(&a->dev, e->cmd_name, argc, args);
}

/* ═══════════════════════════════════════════════════════════
 * CONFIG FORM – BUILD
 * ═══════════════════════════════════════════════════════════ */

#define FORM_LEFT_WIDTH 400



static void on_svin_mode_toggled(GtkCheckButton *btn, gpointer ud) {
    (void)ud;
    if (!gtk_check_button_get_active(btn)) return;
    FormData *fd = (FormData *)g_object_get_data(G_OBJECT(btn), "form_data");
    if (!fd) return;
    
    const char *val = (const char *)g_object_get_data(G_OBJECT(btn), "val");
    if (!val) return;
    
    int mode = atoi(val);
    
    for (int i = 1; i <= 6; i++) {
        if (!fd->entries[i]) continue;
        gboolean sensitive = FALSE;
        if (mode == 2) sensitive = TRUE;
        else if (mode == 1 && (i == 1 || i == 2)) sensitive = TRUE;
        
        gtk_widget_set_sensitive(fd->entries[i], sensitive);
    }
}
static void build_form(AppData *a, GtkWidget *parent, int ci)
{
    int ncfg;
    const ConfigEntry *entries = quectel_get_config_entries(&ncfg);
    const ConfigEntry *e = &entries[ci];
    int idx_len = (e->num_queries > 0) ? e->queries[0].num_args : 0;

    FormData *fd = g_new0(FormData, 1);
    fd->cfg_idx = ci;
    fd->app     = a;
    g_ptr_array_add(a->forms, fd);

    char title[128];
    snprintf(title, sizeof title, "%s Configuration", e->cmd_name);
    GtkWidget *frame = gtk_frame_new(title);
    gtk_widget_set_margin_start(frame, 10);
    gtk_widget_set_margin_end(frame, 10);
    gtk_widget_set_margin_top(frame, 5);
    gtk_widget_set_margin_bottom(frame, 5);
    gtk_box_append(GTK_BOX(parent), frame);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 10);
    gtk_widget_set_margin_bottom(vbox, 10);
    gtk_frame_set_child(GTK_FRAME(frame), vbox);

    /* documentation at the top */
    GtkWidget *doc = gtk_label_new(e->doc ? e->doc : "No documentation available.");
    gtk_label_set_wrap(GTK_LABEL(doc), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(doc), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(doc), 0.0f);
    gtk_label_set_yalign(GTK_LABEL(doc), 0.0f);
    gtk_widget_set_hexpand(doc, TRUE);
    gtk_widget_add_css_class(doc, css_doc(a));
    gtk_box_append(GTK_BOX(vbox), doc);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_hexpand(grid, FALSE);
    gtk_box_append(GTK_BOX(vbox), grid);

    int cols = 2; // Keep 2 columns of inputs
    for (int i = 0; i < e->num_fields; i++) {
        int row = i / cols;
        int col = (i % cols) * 3;

        char lbl[48];
        snprintf(lbl, sizeof lbl, "%s:", e->fields[i]);
        GtkWidget *l = gtk_label_new(lbl);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), l, col, row, 1, 1);

        GtkWidget *en = NULL;
        const FieldDef *fdef = &e->field_defs[i];
        if (fdef->type == FIELD_DROPDOWN) {
            GtkStringList *sl = gtk_string_list_new(NULL);
            for (int j = 0; j < fdef->num_opts; j++) {
                gtk_string_list_append(sl, fdef->opts[j].label);
            }
            en = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
            if (fdef->num_opts <= 1) {
                gtk_widget_set_sensitive(en, FALSE);
            }
            fd->entries[i] = en;
            
            if (i < idx_len && e->num_queries > 0) {
                char buf[16];
                snprintf(buf, sizeof buf, "%d", e->queries[0].args[i]);
                set_widget_val(en, fdef, buf);
            }
        } else if (fdef->type == FIELD_CHECKBOX) {
            en = gtk_check_button_new();
            fd->entries[i] = en;
            if (i < idx_len && e->num_queries > 0) {
                char buf[16];
                snprintf(buf, sizeof buf, "%d", e->queries[0].args[i]);
                set_widget_val(en, fdef, buf);
            }
        } else if (fdef->type == FIELD_RADIO) {
            en = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            fd->entries[i] = en;
            GtkWidget *group = NULL;
            for (int j = 0; j < fdef->num_opts; j++) {
                GtkWidget *rb = gtk_check_button_new_with_label(fdef->opts[j].label);
                g_object_set_data(G_OBJECT(rb), "val", (gpointer)fdef->opts[j].val);
                if (group) gtk_check_button_set_group(GTK_CHECK_BUTTON(rb), GTK_CHECK_BUTTON(group));
                else group = rb;
                
                if (strcmp(e->cmd_name, "CFGSVIN") == 0 && i == 0) {
                    g_object_set_data(G_OBJECT(rb), "form_data", fd);
                    g_signal_connect(rb, "toggled", G_CALLBACK(on_svin_mode_toggled), NULL);
                }
                
                gtk_box_append(GTK_BOX(en), rb);
            }
            if (i < idx_len && e->num_queries > 0) {
                char buf[16];
                snprintf(buf, sizeof buf, "%d", e->queries[0].args[i]);
                set_widget_val(en, fdef, buf);
            }
        } else if (fdef->type == FIELD_BITMASK) {
            const SignalBitmask *bm = quectel_find_bitmask(e->fields[i]);
            if (bm && bm->num_bits > 6) {
                en = gtk_grid_new();
                gtk_grid_set_column_spacing(GTK_GRID(en), 10);
                gtk_grid_set_row_spacing(GTK_GRID(en), 2);
                fd->entries[i] = en;
                for (int k = 0; k < bm->num_bits; k++) {
                    GtkWidget *cb = gtk_check_button_new_with_label(bm->bits[k].name);
                    g_object_set_data(G_OBJECT(cb), "bit", GINT_TO_POINTER(bm->bits[k].bit));
                    gtk_grid_attach(GTK_GRID(en), cb, k % 2, k / 2, 1, 1);
                }
            } else {
                en = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                fd->entries[i] = en;
                if (bm) {
                    for (int k = 0; k < bm->num_bits; k++) {
                        GtkWidget *cb = gtk_check_button_new_with_label(bm->bits[k].name);
                        g_object_set_data(G_OBJECT(cb), "bit", GINT_TO_POINTER(bm->bits[k].bit));
                        gtk_box_append(GTK_BOX(en), cb);
                    }
                }
            }
            if (i < idx_len && e->num_queries > 0) {
                char buf[16];
                snprintf(buf, sizeof buf, "%d", e->queries[0].args[i]);
                set_widget_val(en, fdef, buf);
            }
            gtk_widget_set_valign(l, GTK_ALIGN_START);
        } else {
            en = gtk_entry_new();
            gtk_editable_set_width_chars(GTK_EDITABLE(en), 12);
            fd->entries[i] = en;

            if (i < idx_len && e->num_queries > 0) {
                char buf[16];
                snprintf(buf, sizeof buf, "%d", e->queries[0].args[i]);
                gtk_editable_set_text(GTK_EDITABLE(en), buf);
            }
        }
        
        gtk_grid_attach(GTK_GRID(grid), en, col + 1, row, 1, 1);

        /* bitmask decoder */
        if (e->has_signal_bitmasks && fdef->type == FIELD_TEXT) {
            const SignalBitmask *bm = quectel_find_bitmask(e->fields[i]);
            if (bm) {
                GtkWidget *bml = gtk_label_new("");
                gtk_widget_add_css_class(bml, css_bm(a));
                gtk_label_set_wrap(GTK_LABEL(bml), TRUE);
                gtk_label_set_max_width_chars(GTK_LABEL(bml), 40);
                gtk_widget_set_halign(bml, GTK_ALIGN_START);
                gtk_widget_set_valign(bml, GTK_ALIGN_CENTER);
                gtk_grid_attach(GTK_GRID(grid), bml, col + 2, row, 1, 1);
                fd->bm_labels[i] = bml;

                BmCbData *cbd = g_new0(BmCbData, 1);
                cbd->label = bml;
                cbd->bm    = bm;
                g_object_set_data_full(G_OBJECT(en), "bm", cbd, g_free);
                g_signal_connect(en, "changed", G_CALLBACK(on_bm_changed), cbd);
                on_bm_changed(GTK_EDITABLE(en), cbd);
            }
        }
    }

    /* buttons at the bottom */
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(vbox), bb);

    GtkWidget *rb = gtk_button_new_with_label("Read");
    g_signal_connect(rb, "clicked", G_CALLBACK(on_form_read), fd);
    gtk_box_append(GTK_BOX(bb), rb);

    GtkWidget *wb = gtk_button_new_with_label("Write");
    g_signal_connect(wb, "clicked", G_CALLBACK(on_form_write), fd);
    gtk_box_append(GTK_BOX(bb), wb);


}

/* ═══════════════════════════════════════════════════════════
 * NMEA MESSAGE-RATE TAB
 * ═══════════════════════════════════════════════════════════ */

static void on_nmea_read(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;

    for (int p = 0; p < 3; p++) {
        char pid[16];
        snprintf(pid, sizeof pid, "%d", p + 1);
        for (int m = 0; m < a->nmea_cnt; m++) {
            const char *args[] = { "1", pid, a->dev.supported_msgs[m] };
            int n = 0;
            char **v = quectel_read_config(&a->dev, "CFGMSGRATE", 3, args, &n);
            if (v && n >= 4) {
                int val = atoi(v[3]);
                if (val > 0) {
                    gtk_check_button_set_active(GTK_CHECK_BUTTON(a->nmea_chk[p][m]), TRUE);
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->nmea_ent[p][m]), val);
                } else {
                    gtk_check_button_set_active(GTK_CHECK_BUTTON(a->nmea_chk[p][m]), FALSE);
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->nmea_ent[p][m]), 1);
                }
            } else {
                gtk_check_button_set_active(GTK_CHECK_BUTTON(a->nmea_chk[p][m]), FALSE);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->nmea_ent[p][m]), 1);
            }
            if (v) {
                for (int i = 0; i < n; i++) g_free(v[i]);
                g_free(v);
            }
        }
        flush_events();
    }
}
static void on_nmea_write(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;

    for (int p = 0; p < 3; p++) {
        char pid[16];
        snprintf(pid, sizeof pid, "%d", p + 1);
        for (int m = 0; m < a->nmea_cnt; m++) {
            char rate_buf[16];
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(a->nmea_chk[p][m]))) {
                snprintf(rate_buf, sizeof rate_buf, "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(a->nmea_ent[p][m])));
            } else {
                snprintf(rate_buf, sizeof rate_buf, "0");
            }
            const char *args[] = { "1", pid, a->dev.supported_msgs[m], rate_buf };
            quectel_write_config(&a->dev, "CFGMSGRATE", 4, args);
        }
        flush_events();
    }
}
static void build_nmea_tab(AppData *a)
{
    GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(tab, 10);
    gtk_widget_set_margin_end(tab, 10);
    gtk_widget_set_margin_top(tab, 10);
    gtk_widget_set_margin_bottom(tab, 10);

    GtkWidget *h1 = gtk_label_new("Set output rate for each UART Port.");
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(h1), bold);
    pango_attr_list_unref(bold);
    gtk_label_set_xalign(GTK_LABEL(h1), 0);
    gtk_box_append(GTK_BOX(tab), h1);

    GtkWidget *h2 = gtk_label_new("Check to enable sentence, and set rate value (1-255 fixes per output).");
    gtk_widget_add_css_class(h2, css_doc(a));
    gtk_label_set_xalign(GTK_LABEL(h2), 0);
    gtk_box_append(GTK_BOX(tab), h2);

    a->nmea_cnt = a->dev.num_supported_msgs;
    if (a->nmea_cnt > MAX_NMEA_MSGS)
        a->nmea_cnt = MAX_NMEA_MSGS;

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(tab), sw);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);

    /* Headers */
    GtkWidget *l_msg = gtk_label_new("Message");
    gtk_label_set_attributes(GTK_LABEL(l_msg), bold);
    gtk_grid_attach(GTK_GRID(grid), l_msg, 0, 0, 1, 1);

    for (int p = 0; p < 3; p++) {
        char buf[16];
        snprintf(buf, sizeof buf, "UART%d", p + 1);
        GtkWidget *l_uart = gtk_label_new(buf);
        gtk_label_set_attributes(GTK_LABEL(l_uart), bold);
        gtk_grid_attach(GTK_GRID(grid), l_uart, p + 1, 0, 1, 1);
    }

    for (int m = 0; m < a->nmea_cnt; m++) {
        GtkWidget *l = gtk_label_new(a->dev.supported_msgs[m]);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), l, 0, m + 1, 1, 1);

        for (int p = 0; p < 3; p++) {
            GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            
            GtkWidget *chk = gtk_check_button_new();
            a->nmea_chk[p][m] = chk;
            gtk_box_append(GTK_BOX(hbox), chk);

            GtkWidget *en = gtk_spin_button_new_with_range(1, 255, 1);
            gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(en), TRUE);
            gtk_editable_set_width_chars(GTK_EDITABLE(en), 3);
            a->nmea_ent[p][m] = en;
            gtk_box_append(GTK_BOX(hbox), en);

            /* Bind checkbox state to spin button sensitivity */
            g_object_bind_property(chk, "active", en, "sensitive", G_BINDING_SYNC_CREATE);

            gtk_grid_attach(GTK_GRID(grid), hbox, p + 1, m + 1, 1, 1);
        }
    }

    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_top(bb, 10);
    gtk_box_append(GTK_BOX(tab), bb);

    GtkWidget *rb = gtk_button_new_with_label("Read All");
    g_signal_connect(rb, "clicked", G_CALLBACK(on_nmea_read), a);
    gtk_box_append(GTK_BOX(bb), rb);

    GtkWidget *wb = gtk_button_new_with_label("Write All");
    g_signal_connect(wb, "clicked", G_CALLBACK(on_nmea_write), a);
    gtk_box_append(GTK_BOX(bb), wb);

    gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook), tab,
                             gtk_label_new("NMEA Messages"));
}
static void on_show_nmea_toggled(GtkCheckButton *btn, gpointer ud)
{
    AppData *a = ud;
    a->show_nmea_in_console = gtk_check_button_get_active(btn);
}

static void build_status_tab(AppData *a)
{
    GtkWidget *check = NULL;
    GtkWidget *tab = nmea_status_build_tab(
        &a->status_widgets, &a->nmea_status, &check);
    g_signal_connect(check, "toggled",
                     G_CALLBACK(on_show_nmea_toggled), a);
    gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook), tab,
                              gtk_label_new("Receiver Status"));
}

/* ═══════════════════════════════════════════════════════════
 * JSON EXPORT / IMPORT
 * ═══════════════════════════════════════════════════════════ */

static void do_export(AppData *a, const char *path)
{
    gui_log(LOG_INFO,
        "Starting Full Export (this will take several seconds)...");

    int ncfg;
    const ConfigEntry *entries = quectel_get_config_entries(&ncfg);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    for (int ci = 0; ci < ncfg; ci++) {
        const ConfigEntry *e = &entries[ci];
        if (model_excluded(e, a->dev.model)) continue;

        json_builder_set_member_name(b, e->cmd_name);
        json_builder_begin_array(b);

        for (int qi = 0; qi < e->num_queries; qi++) {
            const QueryTuple *qt = &e->queries[qi];
            const char *args[MAX_QUERY_ARGS];
            char bufs[MAX_QUERY_ARGS][16];
            for (int k = 0; k < qt->num_args; k++) {
                snprintf(bufs[k], sizeof bufs[k],
                         "%d", qt->args[k]);
                args[k] = bufs[k];
            }
            int n = 0;
            char **vals = quectel_read_config(
                &a->dev, e->cmd_name, qt->num_args, args, &n);
            if (vals) {
                json_builder_begin_object(b);
                for (int f = 0; f < n && f < e->num_fields; f++) {
                    json_builder_set_member_name(b, e->fields[f]);
                    json_builder_add_string_value(b, vals[f]);
                    g_free(vals[f]);
                }
                json_builder_end_object(b);
                g_free(vals);
            }
            flush_events();
        }
        json_builder_end_array(b);
    }

    /* CFGMSGRATE */
    static const char *MR_FIELDS[] = {
        "PortType", "PortID", "MsgName", "Rate", "MsgVer_Offset"
    };
    json_builder_set_member_name(b, "CFGMSGRATE");
    json_builder_begin_array(b);
    for (int p = 1; p <= 3; p++) {
        char pid[4];
        snprintf(pid, sizeof pid, "%d", p);
        for (int m = 0; m < a->dev.num_supported_msgs; m++) {
            const char *args[] = {
                "1", pid, a->dev.supported_msgs[m]
            };
            int n = 0;
            char **v = quectel_read_config(
                &a->dev, "CFGMSGRATE", 3, args, &n);
            if (v) {
                json_builder_begin_object(b);
                for (int f = 0; f < n && f < 5; f++) {
                    json_builder_set_member_name(b, MR_FIELDS[f]);
                    json_builder_add_string_value(b, v[f]);
                    g_free(v[f]);
                }
                json_builder_end_object(b);
                g_free(v);
            }
        }
        flush_events();
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, json_builder_get_root(b));
    GError *err = NULL;
    json_generator_to_file(gen, path, &err);
    if (err) {
        gui_log(LOG_ERROR, err->message);
        g_error_free(err);
    } else {
        char msg[512];
        snprintf(msg, sizeof msg, "Exported to %s.", path);
        gui_log(LOG_INFO, msg);
    }
    g_object_unref(gen);
    g_object_unref(b);
}

static void do_import(AppData *a, const char *path)
{
    JsonParser *p = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_file(p, path, &err)) {
        gui_log(LOG_ERROR, err->message);
        g_error_free(err);
        g_object_unref(p);
        return;
    }

    JsonObject *root =
        json_node_get_object(json_parser_get_root(p));
    int ncfg;
    const ConfigEntry *entries = quectel_get_config_entries(&ncfg);

    JsonObjectIter iter;
    const char *name;
    JsonNode *node;
    json_object_iter_init(&iter, root);
    while (json_object_iter_next(&iter, &name, &node)) {
        const ConfigEntry *e = NULL;
        bool is_msgrate = (strcmp(name, "CFGMSGRATE") == 0);

        if (!is_msgrate) {
            for (int i = 0; i < ncfg; i++) {
                if (strcmp(entries[i].cmd_name, name) == 0) {
                    e = &entries[i];
                    break;
                }
            }
            if (!e) continue;
            if (model_excluded(e, a->dev.model)) continue;
        }

        JsonArray *arr = json_node_get_array(node);
        guint len = json_array_get_length(arr);

        if (is_msgrate) {
            static const char *MR_F[] = {
                "PortType", "PortID", "MsgName",
                "Rate", "MsgVer_Offset"
            };
            for (guint i = 0; i < len; i++) {
                JsonObject *o =
                    json_array_get_object_element(arr, i);
                const char *args[5];
                int ac = 0;
                for (int f = 0; f < 5; f++) {
                    if (json_object_has_member(o, MR_F[f]))
                        args[ac++] =
                            json_object_get_string_member(
                                o, MR_F[f]);
                }
                if (ac > 0)
                    quectel_write_config(
                        &a->dev, "CFGMSGRATE", ac, args);
                flush_events();
            }
        } else {
            for (guint i = 0; i < len; i++) {
                JsonObject *o =
                    json_array_get_object_element(arr, i);
                const char *args[MAX_CFG_FIELDS];
                int ac = 0;
                for (int f = 0; f < e->num_fields; f++) {
                    if (json_object_has_member(o, e->fields[f]))
                        args[ac++] =
                            json_object_get_string_member(
                                o, e->fields[f]);
                }
                if (ac > 0)
                    quectel_write_config(
                        &a->dev, e->cmd_name, ac, args);
                flush_events();
            }
        }
    }
    g_object_unref(p);
    gui_log(LOG_INFO,
        "Import complete. Remember to Save to NVM and Reboot!");
}

/* ── File dialog callbacks ─────────────────────────────── */

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static void on_export_resp(GtkDialog *d, int resp, gpointer ud)
{
    AppData *a = ud;
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
        char *path = g_file_get_path(f);
        do_export(a, path);
        g_free(path);
        g_object_unref(f);
    }
    gtk_window_destroy(GTK_WINDOW(d));
}

static void on_export_btn(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;

    GtkWidget *d = gtk_file_chooser_dialog_new(
        "Export Configuration", GTK_WINDOW(a->window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_current_name(
        GTK_FILE_CHOOSER(d), "lg290p_config.json");
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "JSON Files");
    gtk_file_filter_add_pattern(ff, "*.json");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), ff);

    g_signal_connect(d, "response",
                     G_CALLBACK(on_export_resp), a);
    gtk_window_present(GTK_WINDOW(d));
}

static void on_import_resp(GtkDialog *d, int resp, gpointer ud)
{
    AppData *a = ud;
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(d));
        char *path = g_file_get_path(f);
        do_import(a, path);
        g_free(path);
        g_object_unref(f);
    }
    gtk_window_destroy(GTK_WINDOW(d));
}

static void on_import_btn(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;

    GtkWidget *d = gtk_file_chooser_dialog_new(
        "Import Configuration", GTK_WINDOW(a->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "JSON Files");
    gtk_file_filter_add_pattern(ff, "*.json");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), ff);

    g_signal_connect(d, "response",
                     G_CALLBACK(on_import_resp), a);
    gtk_window_present(GTK_WINDOW(d));
}

G_GNUC_END_IGNORE_DEPRECATIONS

/* ═══════════════════════════════════════════════════════════
 * SYSTEM TAB
 * ═══════════════════════════════════════════════════════════ */

static void on_save_nvm(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;
    if (quectel_save_nvm(&a->dev))
        show_info(a, "Configuration saved to NVM.");
}

static void on_reboot(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;
    quectel_reboot(&a->dev);
}

static void on_gnss_start(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;
    quectel_gnss_start(&a->dev);
}

static void on_gnss_stop(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;
    quectel_gnss_stop(&a->dev);
}

static void build_system_tab(AppData *a)
{
    GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(tab, 20);
    gtk_widget_set_margin_top(tab, 20);

    GtkWidget *h = gtk_label_new(
        "Global Operations & JSON Configurations");
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold,
        pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(h), bold);
    pango_attr_list_unref(bold);
    gtk_box_append(GTK_BOX(tab), h);

    GtkWidget *r_gnss = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(tab), r_gnss);

    GtkWidget *start_btn = gtk_button_new_with_label("GNSS START");
    g_signal_connect(start_btn, "clicked", G_CALLBACK(on_gnss_start), a);
    gtk_box_append(GTK_BOX(r_gnss), start_btn);

    GtkWidget *stop_btn = gtk_button_new_with_label("GNSS STOP");
    g_signal_connect(stop_btn, "clicked", G_CALLBACK(on_gnss_stop), a);
    gtk_box_append(GTK_BOX(r_gnss), stop_btn);

    GtkWidget *r1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(r1, 15);
    gtk_box_append(GTK_BOX(tab), r1);

    GtkWidget *eb = gtk_button_new_with_label(
        "Export Device Config to JSON…");
    g_signal_connect(eb, "clicked",
                     G_CALLBACK(on_export_btn), a);
    gtk_box_append(GTK_BOX(r1), eb);

    GtkWidget *ib = gtk_button_new_with_label(
        "Apply Config from JSON…");
    g_signal_connect(ib, "clicked",
                     G_CALLBACK(on_import_btn), a);
    gtk_box_append(GTK_BOX(r1), ib);

    GtkWidget *r2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(r2, 20);
    gtk_box_append(GTK_BOX(tab), r2);

    GtkWidget *sb = gtk_button_new_with_label(
        "Save Settings to NVM");
    g_signal_connect(sb, "clicked",
                     G_CALLBACK(on_save_nvm), a);
    gtk_box_append(GTK_BOX(r2), sb);

    GtkWidget *rbb = gtk_button_new_with_label(
        "Receiver Reboot");
    g_signal_connect(rbb, "clicked",
                     G_CALLBACK(on_reboot), a);
    gtk_box_append(GTK_BOX(r2), rbb);

    gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook), tab,
                              gtk_label_new("System & Backup"));
}

/* ═══════════════════════════════════════════════════════════
 * REBUILD ALL TABS
 * ═══════════════════════════════════════════════════════════ */

static const char *CATEGORIES[] = {
    "Antenna & Formatting",
    "Base & RTK",
    "GNSS & Signal",
    "Interfaces",
    "Navigation & Timing",
    NULL
};

static void rebuild_tabs(AppData *a)
{
    if (a->forms) g_ptr_array_free(a->forms, TRUE);
    a->forms = g_ptr_array_new_with_free_func(g_free);
    memset(a->nmea_ent, 0, sizeof a->nmea_ent);
    a->nmea_cnt = 0;
    memset(&a->status_widgets, 0, sizeof a->status_widgets);

    while (gtk_notebook_get_n_pages(GTK_NOTEBOOK(a->notebook)) > 0)
        gtk_notebook_remove_page(GTK_NOTEBOOK(a->notebook), 0);

    int ncfg;
    const ConfigEntry *entries = quectel_get_config_entries(&ncfg);

    for (int c = 0; CATEGORIES[c]; c++) {
        GtkWidget *sw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(
            GTK_SCROLLED_WINDOW(sw),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        
        GtkWidget *container;
        if (strcmp(CATEGORIES[c], "GNSS & Signal") == 0) {
            container = gtk_grid_new();
            gtk_grid_set_column_spacing(GTK_GRID(container), 10);
            gtk_grid_set_row_spacing(GTK_GRID(container), 10);
            gtk_widget_set_margin_start(container, 5);
            gtk_widget_set_margin_top(container, 5);
        } else {
            container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        }
        
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), container);

        int grid_idx = 0;
        for (int i = 0; i < ncfg; i++) {
            if (strcmp(entries[i].category, CATEGORIES[c]) != 0) continue;
            if (model_excluded(&entries[i], a->dev.model)) continue;
            
            if (strcmp(CATEGORIES[c], "GNSS & Signal") == 0) {
                GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                build_form(a, wrapper, i);
                gtk_grid_attach(GTK_GRID(container), wrapper, grid_idx % 2, grid_idx / 2, 1, 1);
                grid_idx++;
            } else {
                build_form(a, container, i);
            }
        }

        gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook), sw,
                                  gtk_label_new(CATEGORIES[c]));
    }

    build_nmea_tab(a);
    build_status_tab(a);
    build_system_tab(a);

    gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook), 2);
}

/* ═══════════════════════════════════════════════════════════
 * MAIN WINDOW BUILD
 * ═══════════════════════════════════════════════════════════ */

static void on_read_all(GtkButton *btn, gpointer ud)
{
    (void)btn;
    AppData *a = ud;
    if (!require_conn(a)) return;

    for (guint i = 0; i < a->forms->len; i++) {
        FormData *fd = g_ptr_array_index(a->forms, i);
        int idx_len = 0;
        int ncfg;
        const ConfigEntry *entries = quectel_get_config_entries(&ncfg);
        const ConfigEntry *e = &entries[fd->cfg_idx];
        if (e->num_queries > 0) idx_len = e->queries[0].num_args;

        bool has_idx = true;
        for (int j = 0; j < idx_len; j++) {
            const char *t = get_widget_val(fd->entries[j], &e->field_defs[j]);
            if (!t || !*t) { has_idx = false; break; }
        }
        if (has_idx) {
            on_form_read(NULL, fd);
        }
    }
}

static void build_ui(AppData *a)
{
    a->window = gtk_application_window_new(a->gtkapp);
    gtk_window_set_title(GTK_WINDOW(a->window),
        "Quectel LG290P/LG580P Configurator");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 890, 800);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(a->window), root);

    /* ── Connection bar ─────────────────────────────── */
    GtkWidget *conn_frame = gtk_frame_new("Connection");
    gtk_widget_set_margin_start(conn_frame, 10);
    gtk_widget_set_margin_end(conn_frame, 10);
    gtk_widget_set_margin_top(conn_frame, 5);
    gtk_box_append(GTK_BOX(root), conn_frame);

    GtkWidget *conn_box = gtk_box_new(
        GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_start(conn_box, 5);
    gtk_widget_set_margin_end(conn_box, 5);
    gtk_widget_set_margin_top(conn_box, 5);
    gtk_widget_set_margin_bottom(conn_box, 5);
    gtk_frame_set_child(GTK_FRAME(conn_frame), conn_box);

    gtk_box_append(GTK_BOX(conn_box), gtk_label_new("Port:"));
    a->port_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(a->port_entry),
                          a->saved_port);
    gtk_editable_set_width_chars(GTK_EDITABLE(a->port_entry), 15);
    gtk_box_append(GTK_BOX(conn_box), a->port_entry);

    gtk_box_append(GTK_BOX(conn_box),
                   gtk_label_new("Baud Rate:"));
    const char *bauds[] = {
        "9600", "115200", "230400", "460800", "921600", NULL
    };
    GtkStringList *bm = gtk_string_list_new(bauds);
    a->baud_dd = gtk_drop_down_new(G_LIST_MODEL(bm), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->baud_dd),
                               baud_to_idx(a->saved_baud));
    gtk_box_append(GTK_BOX(conn_box), a->baud_dd);

    a->connect_btn = gtk_button_new_with_label("Connect");
    g_signal_connect(a->connect_btn, "clicked",
                     G_CALLBACK(on_connect), a);
    gtk_box_append(GTK_BOX(conn_box), a->connect_btn);

    GtkWidget *read_all_btn = gtk_button_new_with_label("Read All Settings");
    g_signal_connect(read_all_btn, "clicked", G_CALLBACK(on_read_all), a);
    gtk_box_append(GTK_BOX(conn_box), read_all_btn);

    /* ── Notebook ───────────────────────────────────── */
    a->notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(a->notebook, TRUE);
    gtk_widget_set_margin_start(a->notebook, 10);
    gtk_widget_set_margin_end(a->notebook, 10);
    gtk_widget_set_margin_top(a->notebook, 5);
    gtk_box_append(GTK_BOX(root), a->notebook);

    /* ── Log panel ──────────────────────────────────── */
    GtkWidget *log_frame = gtk_frame_new("Console Log");
    gtk_widget_set_margin_start(log_frame, 10);
    gtk_widget_set_margin_end(log_frame, 10);
    gtk_widget_set_margin_top(log_frame, 5);
    gtk_widget_set_margin_bottom(log_frame, 5);
    gtk_box_append(GTK_BOX(root), log_frame);

    a->log_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(a->log_sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(a->log_sw, -1, 150);
    gtk_frame_set_child(GTK_FRAME(log_frame), a->log_sw);

    a->log_tv = gtk_text_view_new();
    gtk_text_view_set_editable(
        GTK_TEXT_VIEW(a->log_tv), FALSE);
    gtk_text_view_set_cursor_visible(
        GTK_TEXT_VIEW(a->log_tv), FALSE);
    gtk_text_view_set_wrap_mode(
        GTK_TEXT_VIEW(a->log_tv), GTK_WRAP_WORD);
    gtk_widget_add_css_class(a->log_tv, "log-view");
    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(a->log_sw), a->log_tv);

    /* text tags */
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(a->log_tv));
    gtk_text_buffer_create_tag(buf, "normal",
        "foreground", "#00ff00", NULL);
    gtk_text_buffer_create_tag(buf, "error",
        "foreground", "#ff4d4d", NULL);
    gtk_text_buffer_create_tag(buf, "warning",
        "foreground", "darkorange", NULL);
}

/* ═══════════════════════════════════════════════════════════
 * ACTIVATE & MAIN
 * ═══════════════════════════════════════════════════════════ */

static void activate(GtkApplication *gtkapp, gpointer ud)
{
    (void)gtkapp;
    AppData *a = ud;
    G = a;

    load_settings(a);
    quectel_init(&a->dev);
    quectel_set_log_callback(gui_log);
    nmea_status_init(&a->nmea_status);

    setup_css();
    a->dark_mode = detect_dark_mode();

    build_ui(a);
    rebuild_tabs(a);
    gtk_window_present(GTK_WINDOW(a->window));
}

int main(int argc, char *argv[])
{
    AppData app;
    memset(&app, 0, sizeof app);
    app.dev.fd = -1;

    app.gtkapp = gtk_application_new(
        "com.example.gnss_configurator",
        G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.gtkapp, "activate",
                     G_CALLBACK(activate), &app);

    int status = g_application_run(
        G_APPLICATION(app.gtkapp), argc, argv);

    stop_nmea_poll(&app);
    quectel_cleanup(&app.dev);
    if (app.forms)
        g_ptr_array_free(app.forms, TRUE);
    g_object_unref(app.gtkapp);
    return status;
}
