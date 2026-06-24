/*
 * nmea_status.c – NMEA sentence parser, sky-chart renderer, and
 *                 receiver-status tab builder for GTK 4.
 *
 * Parses GGA, GSA, GSV, GST sentences to extract position, fix info,
 * DOP, position error estimates, and satellite data.
 * Draws a polar sky chart with Cairo.
 */

#define _USE_MATH_DEFINES
#include "nmea_status.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ═══════════════════════════════════════════════════════════
 * 1. CONSTANTS
 * ═══════════════════════════════════════════════════════════ */

static const char *CONST_NAMES[NUM_CONSTELLATIONS] = {
    "GPS", "GLONASS", "Galileo", "BDS", "QZSS", "NavIC"
};

static const double CONST_COLORS[NUM_CONSTELLATIONS][3] = {
    { 0.12, 0.46, 1.00 },   /* GPS      – blue   */
    { 1.00, 0.20, 0.20 },   /* GLONASS  – red    */
    { 0.20, 0.78, 0.20 },   /* Galileo  – green  */
    { 1.00, 0.65, 0.00 },   /* BDS      – orange */
    { 0.65, 0.20, 0.90 },   /* QZSS     – purple */
    { 0.00, 0.75, 0.75 },   /* NavIC    – cyan   */
};

const char *nmea_const_name(int c)
{
    return (c >= 0 && c < NUM_CONSTELLATIONS) ? CONST_NAMES[c] : "?";
}

/* ═══════════════════════════════════════════════════════════
 * 2. INIT
 * ═══════════════════════════════════════════════════════════ */

void nmea_status_init(NmeaStatus *st)
{
    memset(st, 0, sizeof *st);
    st->fix_mode = 1;            /* 1 = no fix */
}

/* ═══════════════════════════════════════════════════════════
 * 3. PARSING HELPERS
 * ═══════════════════════════════════════════════════════════ */

#define MAX_FIELDS 32

static int split_nmea(char *buf, char **f, int max)
{
    int n = 0;
    char *p = buf;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p  = c + 1;
    }
    return n;
}

static double parse_lat(const char *v, const char *d)
{
    if (!v[0] || !d[0]) return 0.0;
    double raw = atof(v);
    int deg    = (int)(raw / 100.0);
    double r   = deg + (raw - deg * 100.0) / 60.0;
    return (*d == 'S') ? -r : r;
}

static double parse_lon(const char *v, const char *d)
{
    if (!v[0] || !d[0]) return 0.0;
    double raw = atof(v);
    int deg    = (int)(raw / 100.0);
    double r   = deg + (raw - deg * 100.0) / 60.0;
    return (*d == 'W') ? -r : r;
}

static int talker_to_const(const char *t)
{
    if (t[0] == 'G' && t[1] == 'P') return CONST_GPS;
    if (t[0] == 'G' && t[1] == 'L') return CONST_GLONASS;
    if (t[0] == 'G' && t[1] == 'A') return CONST_GALILEO;
    if (t[0] == 'G' && t[1] == 'B') return CONST_BDS;
    if (t[0] == 'B' && t[1] == 'D') return CONST_BDS;
    if (t[0] == 'G' && t[1] == 'Q') return CONST_QZSS;
    if (t[0] == 'Q' && t[1] == 'Z') return CONST_QZSS;
    if (t[0] == 'G' && t[1] == 'I') return CONST_NAVIC;
    return -1;                     /* GN or unknown */
}

/* ═══════════════════════════════════════════════════════════
 * 4. SENTENCE PARSERS
 * ═══════════════════════════════════════════════════════════ */

/* ── GGA ─────────────────────────────────────────────────
 * $xxGGA,time,lat,N/S,lon,E/W,qual,nSV,hdop,alt,M,…     */
static void parse_gga(NmeaStatus *st, char **f, int nf)
{
    if (nf < 10) return;

    /* New epoch → clear used-PRN list and GSV-clear flags */
    st->num_used_prns = 0;
    memset(st->gsv_cleared, 0, sizeof st->gsv_cleared);

    st->latitude  = parse_lat(f[2], f[3]);
    st->longitude = parse_lon(f[4], f[5]);
    if (f[6][0]) st->fix_quality   = atoi(f[6]);
    if (f[7][0]) st->num_sats_used = atoi(f[7]);
    if (f[8][0]) st->hdop          = atof(f[8]);
    if (f[9][0]) st->altitude      = atof(f[9]);
}

/* ── GSA ─────────────────────────────────────────────────
 * $xxGSA,mode,fix,sv1…sv12,pdop,hdop,vdop[,sysId]        */
static void parse_gsa(NmeaStatus *st, char **f, int nf)
{
    if (nf < 18) return;

    if (f[2][0]) st->fix_mode = atoi(f[2]);

    for (int i = 3; i <= 14 && i < nf; i++) {
        if (!f[i][0]) continue;
        int prn = atoi(f[i]);
        if (prn <= 0 || st->num_used_prns >= MAX_USED_PRNS) continue;
        bool dup = false;
        for (int j = 0; j < st->num_used_prns; j++)
            if (st->used_prns[j] == prn) { dup = true; break; }
        if (!dup) st->used_prns[st->num_used_prns++] = prn;
    }

    if (f[15][0]) st->pdop = atof(f[15]);
    if (f[16][0]) st->hdop = atof(f[16]);
    if (f[17][0]) st->vdop = atof(f[17]);
}

/* ── GST ─────────────────────────────────────────────────
 * $xxGST,time,rms,smjr,smnr,orient,lat_err,lon_err,alt_err
 *
 * All error values are 1-sigma in metres.                  */
static void parse_gst(NmeaStatus *st, char **f, int nf)
{
    if (nf < 9) return;

    if (f[2][0]) st->rms_residual  = atof(f[2]);
    if (f[3][0]) st->smajor_err_m  = atof(f[3]);
    if (f[4][0]) st->sminor_err_m  = atof(f[4]);
    if (f[5][0]) st->orient_deg    = atof(f[5]);
    if (f[6][0]) st->lat_err_m     = atof(f[6]);
    if (f[7][0]) st->lon_err_m     = atof(f[7]);
    if (f[8][0]) st->alt_err_m     = atof(f[8]);
}

/* Remove all SVs of a given constellation from the list.  */
static void remove_const_svs(NmeaStatus *st, int c)
{
    int w = 0;
    for (int r = 0; r < st->num_svs; r++)
        if (st->svs[r].constellation != c)
            st->svs[w++] = st->svs[r];
    st->num_svs = w;
}

/* ── GSV ─────────────────────────────────────────────────
 * $xxGSV,nMsg,msgN,totSV, prn,el,az,snr, … [,sigId]      */
static void parse_gsv(NmeaStatus *st, char **f, int nf,
                      const char *talker)
{
    if (nf < 4) return;
    int constellation = talker_to_const(talker);
    if (constellation < 0) return;

    int msg_num = atoi(f[2]);

    if (msg_num == 1 && !st->gsv_cleared[constellation]) {
        remove_const_svs(st, constellation);
        st->gsv_cleared[constellation] = true;
    }

    int remaining = nf - 4;
    int sv_fields = (remaining % 4 == 1) ? remaining - 1 : remaining;
    int sv_cnt    = sv_fields / 4;

    for (int s = 0; s < sv_cnt && st->num_svs < MAX_SVS; s++) {
        int b = 4 + s * 4;
        if (b + 3 >= nf) break;

        int prn  = f[b][0]   ? atoi(f[b])   : 0;
        int elev = f[b+1][0] ? atoi(f[b+1]) : -1;
        int azim = f[b+2][0] ? atoi(f[b+2]) : -1;
        int snr  = f[b+3][0] ? atoi(f[b+3]) : -1;
        if (prn <= 0) continue;

        bool found = false;
        for (int i = 0; i < st->num_svs; i++) {
            SvInfo *x = &st->svs[i];
            if (x->prn == prn && x->constellation == constellation) {
                if (snr  > x->snr)  x->snr  = snr;
                if (elev >= 0)       x->elevation = elev;
                if (azim >= 0)       x->azimuth   = azim;
                found = true;
                break;
            }
        }
        if (!found) {
            SvInfo *nv = &st->svs[st->num_svs++];
            nv->prn           = prn;
            nv->elevation     = elev;
            nv->azimuth       = azim;
            nv->snr           = snr;
            nv->constellation = constellation;
            nv->in_use        = false;
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * 5. MAIN LINE PROCESSOR
 * ═══════════════════════════════════════════════════════════ */

void nmea_process_line(NmeaStatus *st, const char *line)
{
    if (!line || line[0] != '$') return;

    char buf[512];
    g_strlcpy(buf, line, sizeof buf);

    char *star = strchr(buf, '*');
    if (star) *star = '\0';

    char *f[MAX_FIELDS];
    int nf = split_nmea(buf, f, MAX_FIELDS);
    if (nf < 2) return;

    const char *id = f[0] + 1;          /* skip '$' */
    if (strlen(id) < 5) return;

    char talker[3] = { id[0], id[1], '\0' };
    const char *sentence = id + 2;

    if      (strcmp(sentence, "GGA") == 0) parse_gga(st, f, nf);
    else if (strcmp(sentence, "GSA") == 0) parse_gsa(st, f, nf);
    else if (strcmp(sentence, "GSV") == 0) parse_gsv(st, f, nf, talker);
    else if (strcmp(sentence, "GST") == 0) parse_gst(st, f, nf);
}

/* ═══════════════════════════════════════════════════════════
 * 6. FIX-TYPE STRINGS
 * ═══════════════════════════════════════════════════════════ */

static const char *fix_quality_str(int q)
{
    switch (q) {
    case 0: return "No Fix";
    case 1: return "GPS / SPS";
    case 2: return "DGPS";
    case 3: return "PPS";
    case 4: return "RTK Fix";
    case 5: return "RTK Float";
    case 6: return "Dead Reckoning";
    default: return "Unknown";
    }
}

static const char *fix_mode_str(int m)
{
    switch (m) {
    case 1: return "No Fix";
    case 2: return "2D";
    case 3: return "3D";
    default: return "N/A";
    }
}

/* ═══════════════════════════════════════════════════════════
 * 7. DISPLAY UPDATE
 * ═══════════════════════════════════════════════════════════ */

void nmea_status_update_display(NmeaStatus *st, StatusWidgets *w)
{
    if (!w->lat_val) return;

    /* Mark in_use from GSA PRN list */
    for (int i = 0; i < st->num_svs; i++) {
        st->svs[i].in_use = false;
        for (int j = 0; j < st->num_used_prns; j++) {
            if (st->svs[i].prn == st->used_prns[j]) {
                st->svs[i].in_use = true;
                break;
            }
        }
    }

    /* Per-constellation counts (deduplicated by PRN) */
    int view[NUM_CONSTELLATIONS] = {0};
    int used[NUM_CONSTELLATIONS] = {0};

    for (int i = 0; i < st->num_svs; i++) {
        int c = st->svs[i].constellation;
        if (c < 0 || c >= NUM_CONSTELLATIONS) continue;
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (st->svs[j].constellation == c &&
                st->svs[j].prn == st->svs[i].prn) {
                dup = true;
                break;
            }
        if (dup) continue;
        view[c]++;
        if (st->svs[i].in_use) used[c]++;
    }

    char buf[64];

    /* Position (GGA) */
    snprintf(buf, sizeof buf, "%.7f°", st->latitude);
    gtk_label_set_text(GTK_LABEL(w->lat_val), buf);
    snprintf(buf, sizeof buf, "%.7f°", st->longitude);
    gtk_label_set_text(GTK_LABEL(w->lon_val), buf);
    snprintf(buf, sizeof buf, "%.1f m", st->altitude);
    gtk_label_set_text(GTK_LABEL(w->alt_val), buf);

    /* Fix (GGA / GSA) */
    gtk_label_set_text(GTK_LABEL(w->fix_val),
                       fix_quality_str(st->fix_quality));
    gtk_label_set_text(GTK_LABEL(w->mode_val),
                       fix_mode_str(st->fix_mode));
    snprintf(buf, sizeof buf, "%d", st->num_sats_used);
    gtk_label_set_text(GTK_LABEL(w->sats_val), buf);

    /* DOP (GSA) */
    snprintf(buf, sizeof buf, "%.1f", st->pdop);
    gtk_label_set_text(GTK_LABEL(w->pdop_val), buf);
    snprintf(buf, sizeof buf, "%.1f", st->hdop);
    gtk_label_set_text(GTK_LABEL(w->hdop_val), buf);
    snprintf(buf, sizeof buf, "%.1f", st->vdop);
    gtk_label_set_text(GTK_LABEL(w->vdop_val), buf);

    /* Position Error (GST) */
    snprintf(buf, sizeof buf, "%.3f m", st->rms_residual);
    gtk_label_set_text(GTK_LABEL(w->rms_val), buf);
    snprintf(buf, sizeof buf, "%.3f m", st->lat_err_m);
    gtk_label_set_text(GTK_LABEL(w->lat_err_val), buf);
    snprintf(buf, sizeof buf, "%.3f m", st->lon_err_m);
    gtk_label_set_text(GTK_LABEL(w->lon_err_val), buf);
    snprintf(buf, sizeof buf, "%.3f m", st->alt_err_m);
    gtk_label_set_text(GTK_LABEL(w->alt_err_val), buf);
    snprintf(buf, sizeof buf, "%.3f m", st->smajor_err_m);
    gtk_label_set_text(GTK_LABEL(w->smajor_val), buf);
    snprintf(buf, sizeof buf, "%.3f m", st->sminor_err_m);
    gtk_label_set_text(GTK_LABEL(w->sminor_val), buf);
    snprintf(buf, sizeof buf, "%.1f°", st->orient_deg);
    gtk_label_set_text(GTK_LABEL(w->orient_val), buf);

    /* Constellation counts (GSV) */
    for (int c = 0; c < NUM_CONSTELLATIONS; c++) {
        snprintf(buf, sizeof buf, "%d / %d", view[c], used[c]);
        gtk_label_set_text(GTK_LABEL(w->const_labels[c]), buf);
    }

    /* Trigger sky-chart redraw */
    if (w->sky_chart)
        gtk_widget_queue_draw(w->sky_chart);
}

/* ═══════════════════════════════════════════════════════════
 * 8. SKY CHART (Cairo drawing on GtkDrawingArea)
 * ═══════════════════════════════════════════════════════════ */

static void sky_draw(GtkDrawingArea *area, cairo_t *cr,
                     int width, int height, gpointer data)
{
    (void)area;
    NmeaStatus *st = data;

    const int legend_h = 28;
    int chart_h = height - legend_h;
    if (chart_h < 40) chart_h = 40;
    int sz = (width < chart_h) ? width : chart_h;
    double cx = width / 2.0;
    double cy = chart_h / 2.0;
    double R  = (sz - 40) / 2.0;
    if (R < 20) R = 20;

    /* Background */
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.14);
    cairo_paint(cr);

    /* Elevation rings: 0° (outer), 30°, 60° */
    cairo_set_line_width(cr, 0.8);
    cairo_set_source_rgba(cr, 0.35, 0.35, 0.40, 0.8);
    for (int e = 0; e <= 60; e += 30) {
        double rr = (90.0 - e) / 90.0 * R;
        cairo_arc(cr, cx, cy, rr, 0, 2 * M_PI);
        cairo_stroke(cr);
    }
    cairo_arc(cr, cx, cy, 2, 0, 2 * M_PI);
    cairo_fill(cr);

    /* Cross-hair lines */
    cairo_set_source_rgba(cr, 0.30, 0.30, 0.35, 0.6);
    cairo_set_line_width(cr, 0.6);
    cairo_move_to(cr, cx, cy - R);
    cairo_line_to(cr, cx, cy + R);
    cairo_stroke(cr);
    cairo_move_to(cr, cx - R, cy);
    cairo_line_to(cr, cx + R, cy);
    cairo_stroke(cr);

    /* Cardinal labels */
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgb(cr, 0.75, 0.75, 0.80);

    cairo_text_extents_t ext;

    cairo_text_extents(cr, "N", &ext);
    cairo_move_to(cr, cx - ext.width / 2, cy - R - 6);
    cairo_show_text(cr, "N");

    cairo_text_extents(cr, "S", &ext);
    cairo_move_to(cr, cx - ext.width / 2,
                  cy + R + ext.height + 6);
    cairo_show_text(cr, "S");

    cairo_text_extents(cr, "E", &ext);
    cairo_move_to(cr, cx + R + 6, cy + ext.height / 2);
    cairo_show_text(cr, "E");

    cairo_text_extents(cr, "W", &ext);
    cairo_move_to(cr, cx - R - ext.width - 6,
                  cy + ext.height / 2);
    cairo_show_text(cr, "W");

    /* Elevation labels */
    cairo_set_font_size(cr, 9);
    cairo_set_source_rgba(cr, 0.50, 0.50, 0.55, 0.7);
    for (int e = 30; e <= 60; e += 30) {
        double lr = (90.0 - e) / 90.0 * R;
        char lbl[8];
        snprintf(lbl, sizeof lbl, "%d°", e);
        cairo_move_to(cr, cx + 3, cy - lr + 11);
        cairo_show_text(cr, lbl);
    }

    /* Plot satellites */
    const double dot_r = 6.0;
    cairo_set_font_size(cr, 8);

    for (int i = 0; i < st->num_svs; i++) {
        const SvInfo *sv = &st->svs[i];
        if (sv->elevation < 0 || sv->elevation > 90) continue;
        if (sv->azimuth < 0   || sv->azimuth > 360) continue;

        double sr    = (90.0 - sv->elevation) / 90.0 * R;
        double a_rad = sv->azimuth * M_PI / 180.0;
        double sx    = cx + sr * sin(a_rad);
        double sy    = cy - sr * cos(a_rad);

        int c = sv->constellation;
        if (c < 0 || c >= NUM_CONSTELLATIONS) c = 0;
        double alpha = sv->in_use ? 1.0 : 0.35;

        /* filled dot */
        cairo_set_source_rgba(cr,
            CONST_COLORS[c][0], CONST_COLORS[c][1],
            CONST_COLORS[c][2], alpha);
        cairo_arc(cr, sx, sy, dot_r, 0, 2 * M_PI);
        cairo_fill(cr);

        /* outline */
        cairo_set_source_rgba(cr, 1, 1, 1, alpha * 0.4);
        cairo_arc(cr, sx, sy, dot_r, 0, 2 * M_PI);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        /* PRN number */
        char ps[8];
        snprintf(ps, sizeof ps, "%d", sv->prn);
        cairo_set_source_rgba(cr, 1, 1, 1, alpha * 0.9);
        cairo_move_to(cr, sx + dot_r + 2, sy + 3);
        cairo_show_text(cr, ps);
    }

    /* Legend at the bottom */
    double lx = 10;
    double ly = chart_h + legend_h - 6;
    cairo_set_font_size(cr, 9);
    for (int c = 0; c < NUM_CONSTELLATIONS; c++) {
        cairo_set_source_rgb(cr,
            CONST_COLORS[c][0], CONST_COLORS[c][1],
            CONST_COLORS[c][2]);
        cairo_arc(cr, lx + 5, ly - 4, 4, 0, 2 * M_PI);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.75, 0.75, 0.80);
        cairo_move_to(cr, lx + 12, ly);
        cairo_show_text(cr, CONST_NAMES[c]);

        cairo_text_extents(cr, CONST_NAMES[c], &ext);
        lx += 14 + ext.x_advance + 10;
    }
}

/* ═══════════════════════════════════════════════════════════
 * 9. TAB BUILDER
 * ═══════════════════════════════════════════════════════════ */

/* Add a row  "Label:  [value_widget]"  to a grid. */
static GtkWidget *add_row(GtkWidget *grid, int row,
                          const char *label_text)
{
    GtkWidget *l = gtk_label_new(label_text);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);

    GtkWidget *v = gtk_label_new("---");
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_label_set_width_chars(GTK_LABEL(v), 16);
    gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
    return v;
}

/* Build a section frame with a title that includes the sentence source. */
static GtkWidget *make_section(const char *title, GtkWidget **out_grid)
{
    GtkWidget *fr = gtk_frame_new(title);
    GtkWidget *g  = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    gtk_grid_set_row_spacing(GTK_GRID(g), 4);
    gtk_widget_set_margin_start(g, 8);
    gtk_widget_set_margin_end(g, 8);
    gtk_widget_set_margin_top(g, 4);
    gtk_widget_set_margin_bottom(g, 4);
    gtk_frame_set_child(GTK_FRAME(fr), g);
    *out_grid = g;
    return fr;
}

GtkWidget *nmea_status_build_tab(StatusWidgets *w,
                                  NmeaStatus    *st,
                                  GtkWidget    **out_check)
{
    memset(w, 0, sizeof *w);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(vbox, 10);
    gtk_widget_set_margin_end(vbox, 10);
    gtk_widget_set_margin_top(vbox, 5);

    /* Checkbox */
    *out_check = gtk_check_button_new_with_label(
                         "Show NMEA sentences in Console");
    gtk_box_append(GTK_BOX(vbox), *out_check);

    /* Horizontal split:  info | sky chart */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_append(GTK_BOX(vbox), hbox);

    /* --- left column: info inside a scrolled window --- */
    GtkWidget *info_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(info_sw),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(info_sw, 320, -1);
    gtk_box_append(GTK_BOX(hbox), info_sw);

    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(info, 5);
    gtk_widget_set_margin_bottom(info, 5);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(info_sw), info);

    GtkWidget *g;

    /* ── Position (GGA) ──────────────────────────────── */
    gtk_box_append(GTK_BOX(info),
                   make_section("Position  (GGA)", &g));
    w->lat_val = add_row(g, 0, "Latitude:");
    w->lon_val = add_row(g, 1, "Longitude:");
    w->alt_val = add_row(g, 2, "Altitude:");

    /* ── Fix (GGA / GSA) ─────────────────────────────── */
    gtk_box_append(GTK_BOX(info),
                   make_section("Fix  (GGA / GSA)", &g));
    w->fix_val  = add_row(g, 0, "Quality:");
    w->mode_val = add_row(g, 1, "Mode:");
    w->sats_val = add_row(g, 2, "Sats Used:");

    /* ── DOP (GSA) ───────────────────────────────────── */
    gtk_box_append(GTK_BOX(info),
                   make_section("DOP  (GSA)", &g));
    w->pdop_val = add_row(g, 0, "PDOP:");
    w->hdop_val = add_row(g, 1, "HDOP:");
    w->vdop_val = add_row(g, 2, "VDOP:");

    /* ── Position Error (GST) ────────────────────────── */
    gtk_box_append(GTK_BOX(info),
                   make_section("Position Error  (GST)", &g));
    w->rms_val     = add_row(g, 0, "RMS Residual:");
    w->lat_err_val = add_row(g, 1, "Lat σ:");
    w->lon_err_val = add_row(g, 2, "Lon σ:");
    w->alt_err_val = add_row(g, 3, "Alt σ:");
    w->smajor_val  = add_row(g, 4, "Semi-Major:");
    w->sminor_val  = add_row(g, 5, "Semi-Minor:");
    w->orient_val  = add_row(g, 6, "Orientation:");

    /* ── Satellites (GSV) ────────────────────────────── */
    gtk_box_append(GTK_BOX(info),
                   make_section("Satellites  View / Used  (GSV)", &g));
    for (int c = 0; c < NUM_CONSTELLATIONS; c++) {
        char name[24];
        snprintf(name, sizeof name, "%s:", CONST_NAMES[c]);
        GtkWidget *l = gtk_label_new(name);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(g), l, 0, c, 1, 1);

        w->const_labels[c] = gtk_label_new("0 / 0");
        gtk_widget_set_halign(w->const_labels[c], GTK_ALIGN_START);
        gtk_label_set_width_chars(
            GTK_LABEL(w->const_labels[c]), 10);
        gtk_grid_attach(GTK_GRID(g), w->const_labels[c],
                        1, c, 1, 1);
    }

    /* --- right column: sky chart --- */
    w->sky_chart = gtk_drawing_area_new();
    gtk_widget_set_size_request(w->sky_chart, 350, 350);
    gtk_widget_set_hexpand(w->sky_chart, TRUE);
    gtk_widget_set_vexpand(w->sky_chart, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(w->sky_chart),
        sky_draw, st, NULL);
    gtk_box_append(GTK_BOX(hbox), w->sky_chart);

    return vbox;
}
