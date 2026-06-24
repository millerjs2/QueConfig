#ifndef NMEA_STATUS_H
#define NMEA_STATUS_H
/*
 * nmea_status.h – NMEA sentence parsing, receiver status display,
 *                 and polar sky-chart for GTK 4.
 *
 * Parses: GGA, GSA, GSV, GST
 */

#include <gtk/gtk.h>
#include <stdbool.h>

#define MAX_SVS        128
#define MAX_USED_PRNS   64

enum {
    CONST_GPS = 0,
    CONST_GLONASS,
    CONST_GALILEO,
    CONST_BDS,
    CONST_QZSS,
    CONST_NAVIC,
    NUM_CONSTELLATIONS
};

/* ── Per-satellite data ──────────────────────────────── */
typedef struct {
    int  prn;
    int  elevation;         /* 0-90  (-1 = unknown)  */
    int  azimuth;           /* 0-359 (-1 = unknown)  */
    int  snr;               /* dBHz, or -1           */
    int  constellation;     /* CONST_GPS …           */
    bool in_use;            /* matched against GSA   */
} SvInfo;

/* ── Accumulated receiver state ──────────────────────── */
typedef struct {
    /* GGA */
    double  latitude, longitude, altitude;
    int     fix_quality;        /* GGA field 6           */
    int     num_sats_used;      /* GGA field 7           */

    /* GSA */
    int     fix_mode;           /* GSA field 2 (1/2/3)   */
    double  pdop, hdop, vdop;

    /* GST – position error estimates (1-sigma, metres) */
    double  rms_residual;       /* RMS of pseudorange residuals */
    double  lat_err_m;          /* latitude error  (m)   */
    double  lon_err_m;          /* longitude error (m)   */
    double  alt_err_m;          /* altitude error  (m)   */
    double  smajor_err_m;       /* semi-major axis (m)   */
    double  sminor_err_m;       /* semi-minor axis (m)   */
    double  orient_deg;         /* ellipse orientation (°)*/

    /* GSV */
    SvInfo  svs[MAX_SVS];
    int     num_svs;

    /* GSA used-PRN accumulator */
    int     used_prns[MAX_USED_PRNS];
    int     num_used_prns;

    bool    gsv_cleared[NUM_CONSTELLATIONS];   /* internal */
} NmeaStatus;

/* ── Widget handles for the status tab ───────────────── */
typedef struct {
    /* Position (GGA) */
    GtkWidget *lat_val, *lon_val, *alt_val;

    /* Fix (GGA / GSA) */
    GtkWidget *fix_val, *mode_val, *sats_val;

    /* DOP (GSA) */
    GtkWidget *pdop_val, *hdop_val, *vdop_val;

    /* Position Error (GST) */
    GtkWidget *rms_val;
    GtkWidget *lat_err_val, *lon_err_val, *alt_err_val;
    GtkWidget *smajor_val, *sminor_val, *orient_val;

    /* Satellites (GSV) */
    GtkWidget *const_labels[NUM_CONSTELLATIONS];

    /* Sky chart */
    GtkWidget *sky_chart;           /* GtkDrawingArea */
} StatusWidgets;

/* ── Public API ──────────────────────────────────────── */

void         nmea_status_init  (NmeaStatus *st);
void         nmea_process_line (NmeaStatus *st, const char *line);

GtkWidget   *nmea_status_build_tab(StatusWidgets *w,
                                    NmeaStatus    *st,
                                    GtkWidget    **out_check);

void         nmea_status_update_display(NmeaStatus    *st,
                                        StatusWidgets *w);

const char  *nmea_const_name(int constellation);

#endif /* NMEA_STATUS_H */
