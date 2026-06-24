#ifndef QUECTEL_H
#define QUECTEL_H

#include <stdbool.h>

#define MAX_CFG_FIELDS       12
#define MAX_CFG_QUERIES       4
#define MAX_QUERY_ARGS        3
#define MAX_BITMASK_BITS      12
#define MAX_EXCLUDE_MODELS    2
#define MAX_NMEA_MSGS        12
#define LINE_BUF_SIZE       512
#define SERIAL_TIMEOUT_SEC    1.2

enum { LOG_INFO = 0, LOG_WARNING = 1, LOG_ERROR = 2 };

typedef struct {
    int         bit;
    const char *name;
} BitmaskBit;

typedef struct {
    const char       *field_name;
    BitmaskBit        bits[MAX_BITMASK_BITS];
    int               num_bits;
} SignalBitmask;

typedef struct {
    int  args[MAX_QUERY_ARGS];
    int  num_args;
} QueryTuple;


#define MAX_DROPDOWN_OPTS 12

typedef enum {
    FIELD_TEXT = 0,
    FIELD_DROPDOWN,
    FIELD_CHECKBOX,
    FIELD_BITMASK,
    FIELD_RADIO
} FieldType;

typedef struct {
    const char *val;
    const char *label;
} DropOption;

typedef struct {
    FieldType type;
    DropOption opts[MAX_DROPDOWN_OPTS];
    int num_opts;
} FieldDef;

typedef struct {
    const char  *cmd_name;
    const char  *category;
    const char  *doc;
    const char  *fields[MAX_CFG_FIELDS];
    int          num_fields;
    QueryTuple   queries[MAX_CFG_QUERIES];
    int          num_queries;
    const char  *exclude_models[MAX_EXCLUDE_MODELS];
    int          num_exclude;
    bool         has_signal_bitmasks;
    FieldDef     field_defs[MAX_CFG_FIELDS];
} ConfigEntry;

typedef struct {
    int     fd;
    char    model[32];
    char  **supported_msgs;
    int     num_supported_msgs;
    double  timeout_sec;
    bool    command_busy;
} QuectelDevice;

typedef void (*AppLogFunc)(int level, const char *message);

void quectel_set_log_callback(AppLogFunc func);

const ConfigEntry   *quectel_get_config_entries(int *out_count);
const SignalBitmask *quectel_get_signal_bitmasks(int *out_count);
const SignalBitmask *quectel_find_bitmask(const char *field_name);

void  quectel_init   (QuectelDevice *dev);
void  quectel_cleanup(QuectelDevice *dev);
bool  quectel_connect   (QuectelDevice *dev, const char *port, int baud);
void  quectel_disconnect(QuectelDevice *dev);
bool  quectel_is_connected(const QuectelDevice *dev);

char  *quectel_send_raw   (QuectelDevice *dev, const char *cmd,
                            const char *wait_for);
char **quectel_read_config (QuectelDevice *dev, const char *cfg,
                            int argc, const char **argv, int *out_n);
bool   quectel_write_config(QuectelDevice *dev, const char *cfg,
                            int argc, const char **argv);
bool   quectel_save_nvm    (QuectelDevice *dev);
void   quectel_reboot      (QuectelDevice *dev);
void   quectel_gnss_start  (QuectelDevice *dev);
void   quectel_gnss_stop   (QuectelDevice *dev);

#endif /* QUECTEL_H */
