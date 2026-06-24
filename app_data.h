#ifndef APP_DATA_H
#define APP_DATA_H

#include "quectel.h"
#include "nmea_status.h"
#include <gtk/gtk.h>
#include <stdbool.h>

#define NMEA_LINE_BUF_SIZE 512

typedef struct {
    GtkApplication *gtkapp;
    GtkWidget      *window;
    GtkWidget      *notebook;
    GtkWidget      *log_tv;
    GtkWidget      *log_sw;
    GtkWidget      *port_entry;
    GtkWidget      *baud_dd;
    GtkWidget      *connect_btn;

    QuectelDevice   dev;
    GPtrArray      *forms;

    GtkWidget      *nmea_ent[3][MAX_NMEA_MSGS];
    GtkWidget      *nmea_chk[3][MAX_NMEA_MSGS];
    int             nmea_cnt;

    char  saved_port[256];
    int   saved_baud;
    bool  dark_mode;

    /* Receiver Status */
    NmeaStatus      nmea_status;
    StatusWidgets   status_widgets;
    guint           nmea_poll_id;
    char            nmea_line_buf[NMEA_LINE_BUF_SIZE];
    int             nmea_line_pos;
    bool            show_nmea_in_console;
} AppData;

typedef struct {
    int           cfg_idx;
    GtkWidget    *entries[MAX_CFG_FIELDS];
    GtkWidget    *bm_labels[MAX_CFG_FIELDS];
    gpointer      app;
} FormData;

typedef struct {
    GtkWidget          *label;
    const SignalBitmask *bm;
} BmCbData;

#endif
