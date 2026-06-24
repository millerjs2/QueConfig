/*
 * quectel.c – Quectel LG290P / LG580P GNSS protocol implementation
 */

#include "quectel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>

#include <glib.h>

/* ═══════════════════════════════════════════════════════════
 * 1. LOGGING
 * ═══════════════════════════════════════════════════════════ */

static AppLogFunc s_log_cb = NULL;

void quectel_set_log_callback(AppLogFunc f) { s_log_cb = f; }

static void plog(int lvl, const char *fmt, ...)
{
    if (!s_log_cb) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s_log_cb(lvl, buf);
}

/* ═══════════════════════════════════════════════════════════
 * 2. NMEA MESSAGE LISTS
 * ═══════════════════════════════════════════════════════════ */

static const char *BASE_NMEA[] = {
    "RMC", "GGA", "GSV", "GSA", "VTG",
    "GLL", "GBS", "GNS", "GST", "ZDA", NULL
};
static const char *EXTRA_NMEA[] = { "HDT", "PQTMTAR", NULL };

/* ═══════════════════════════════════════════════════════════
 * 3. SIGNAL BITMASK TABLE
 * ═══════════════════════════════════════════════════════════ */

static const SignalBitmask SIG_BM[] = {
    { "GPS_Sig", { {0,"L1 C/A"}, {1,"L2C"}, {2,"L5-Q"} }, 3 },
    { "GLO_Sig", { {0,"G1 C/A"}, {1,"G2 C/A"} }, 2 },
    { "GAL_Sig", { {0,"E1"}, {1,"E5a"}, {2,"E5b"}, {3,"E6"} }, 4 },
    { "BDS_Sig", { {0,"B1I"}, {1,"B2I"}, {2,"B3I"}, {3,"B1C"}, {4,"B2a"}, {5,"B2b"} }, 6 },
    { "QZS_Sig", { {0,"L1 C/A"}, {1,"L2C"}, {2,"L5-Q"}, {3,"L6"} }, 4 },
    { "NAC_Sig", { {0,"L5"} }, 1 },
    
    { "InputProt", { {0,"NMEA"}, {1,"Quectel Binary"}, {2,"RTCM3"} }, 3 },
    { "OutputProt", { {0,"NMEA"}, {1,"Quectel Binary"}, {2,"RTCM3"} }, 3 },
    { "Value", { {0,"WAAS"}, {1,"SDCM"}, {2,"EGNOS"}, {3,"BDSBAS"}, {4,"MSAS"}, {5,"GAGAN"}, {6,"KASS"}, {7,"ASECNA"}, {8,"SouthPAN"} }, 9 },
};
#define NUM_SIG_BM ((int)(sizeof SIG_BM / sizeof SIG_BM[0]))

const SignalBitmask *quectel_get_signal_bitmasks(int *n)
{
    *n = NUM_SIG_BM;
    return SIG_BM;
}

const SignalBitmask *quectel_find_bitmask(const char *fn)
{
    for (int i = 0; i < NUM_SIG_BM; i++)
        if (strcmp(SIG_BM[i].field_name, fn) == 0)
            return &SIG_BM[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * 4. CONFIGURATION REGISTRY
 * ═══════════════════════════════════════════════════════════ */

static const ConfigEntry CFG[] = {

    /* ── Interfaces ─────────────────────────────────────── */

    {
        "CFGUART", "Interfaces",
        "Configure UART port settings\nData bits and flow control cannot be changed at this firmware revision.",
        { "Index", "BaudRate", "DataBit", "Parity", "StopBit", "FlowCtrl" }, 6,
        { { {1}, 1 }, { {2}, 1 }, { {3}, 1 } }, 3,
        { NULL, NULL }, 0,
        false,
        {
            { FIELD_DROPDOWN, { {"1", "1 (UART1)"}, {"2", "2 (UART2)"}, {"3", "3 (UART3)"} }, 3 },
            { FIELD_DROPDOWN, { {"9600", "9600"}, {"19200", "19200"}, {"38400", "38400"}, {"57600", "57600"}, {"115200", "115200"}, {"230400", "230400"}, {"460800", "460800"}, {"921600", "921600"} }, 8 },
            { FIELD_DROPDOWN, { {"8", "8 bits"} }, 1 },
            { FIELD_DROPDOWN, { {"0", "0 (No)"}, {"1", "1 (Odd)"}, {"2", "2 (Even)"} }, 3 },
            { FIELD_DROPDOWN, { {"1", "1 bit"}, {"2", "2 bits"} }, 2 },
            { FIELD_DROPDOWN, { {"0", "0 (None)"} }, 1 }
        }
    },

    {
        "CFGPROT", "Interfaces",
        "Configure Protocol inputs and outputs for UART ports.",
        { "PortType", "PortID", "InputProt", "OutputProt" }, 4,
        { { {1, 1}, 2 }, { {1, 2}, 2 }, { {1, 3}, 2 } }, 3,
        { NULL, NULL }, 0,
        true,
        {
            { FIELD_DROPDOWN, { {"1", "1 (UART)"} }, 1 },
            { FIELD_DROPDOWN, { {"1", "1 (UART1)"}, {"2", "2 (UART2)"}, {"3", "3 (UART3)"} }, 3 },
            { FIELD_BITMASK, {}, 0 },
            { FIELD_BITMASK, {}, 0 }
        }
    },

    {
        "CFGPINALT", "Interfaces",
        "PinNum: 14\n"
        "Mode: 1=Mode 1 (RTK_STAT), 2=Mode 2 (ANT_ON)",
        { "PinNum", "Mode" }, 2,
        { { {14}, 1 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGEVENT", "Interfaces",
        "Index: 1 (Event 1)\n"
        "Mode: 0=Disable, 1=Enable\n"
        "Edge: 1=Rising, 2=Falling\n"
        "Guard: Time between two interrupts (ms)",
        { "Index", "Mode", "Edge", "Guard" }, 4,
        { { {1}, 1 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    /* ── Base & RTK ─────────────────────────────────────── */

    {
        "CFGRCVRMODE", "Base & RTK",
        "Mode:\n"
        " 0 = Unknown\n"
        " 1 = Rover (Restores default NMEA messages)\n"
        " 2 = Base Station (Disables NMEA, enables RTCM MSM4 & RTCM3-1005)",
        { "Mode" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGSVIN", "Base & RTK",
        "Base Station Survey-In Mode.",
        { "Mode", "CFG_CNT", "3D_AccLimit", "ECEF_X", "ECEF_Y", "ECEF_Z", "Distance" }, 7,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        {
            { FIELD_RADIO, { {"0", "Disable"}, {"1", "Survey-in"}, {"2", "Fixed"} }, 3 },
            { FIELD_TEXT, {}, 0 },
            { FIELD_TEXT, {}, 0 },
            { FIELD_TEXT, {}, 0 },
            { FIELD_TEXT, {}, 0 },
            { FIELD_TEXT, {}, 0 },
            { FIELD_TEXT, {}, 0 }
        }
    },

    {
        "CFGRTK", "Base & RTK",
        "Configure RTK settings.",
        { "DiffMode", "RelMode", "Timeout" }, 3,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        {
            { FIELD_RADIO, { {"0", "Disable"}, {"1", "Auto"}, {"2", "RTD only"} }, 3 },
            { FIELD_RADIO, { {"1", "Absolute"}, {"2", "Relative"} }, 2 },
            { FIELD_TEXT, {}, 0 }
        }
    },

    {
        "CFGRTKSRCTYPE", "Base & RTK",
        "RTK Source Type",
        { "SrcType" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        {
            { FIELD_DROPDOWN, { {"0", "0 (Auto)"}, {"1", "1 (Normal)"}, {"2", "2 (Wide Lane)"} }, 3 }
        }
    },

    {
        "CFGRTKRL", "Base & RTK",
        "RTK Reliability Level",
        { "Reliability" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        {
            { FIELD_DROPDOWN, { {"1", "1 (Very relax)"}, {"2", "2 (Relax)"}, {"3", "3 (Medium)"}, {"4", "4 (Strict)"}, {"5", "5 (Very strict)"} }, 5 }
        }
    },

    {
        "CFGRSID", "Base & RTK",
        "ID: Reference station ID (0-4095)",
        { "ID" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGRTCM", "Base & RTK",
        "Configure RTCM Output Messages",
        { "MSM_Type", "MSM_Mode", "MSM_ElevThd", "EPH_Mode", "EPH_Interval" }, 5,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        {
            { FIELD_DROPDOWN, { {"3", "3 (MSM3)"}, {"4", "4 (MSM4)"}, {"5", "5 (MSM5)"}, {"6", "6 (MSM6)"}, {"7", "7 (MSM7)"} }, 5 },
            { FIELD_DROPDOWN, { {"0", "0 (Not output when no sat searched)"} }, 1 },
            { FIELD_TEXT, {}, 0 },
            { FIELD_DROPDOWN, { {"0", "0 (Disable)"}, {"1", "1 (On update)"}, {"2", "2 (Interval)"}, {"3", "3 (Each epoch)"} }, 4 },
            { FIELD_TEXT, {}, 0 }
        }
    },

    /* ── GNSS & Signal ──────────────────────────────────── */

    {
        "CFGCNST", "GNSS & Signal",
        "Enable/Disable specific constellations.",
        { "GPS", "GLONASS", "Galileo", "BDS", "QZSS", "NavIC" }, 6,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        {
            { FIELD_CHECKBOX, {}, 0 },
            { FIELD_CHECKBOX, {}, 0 },
            { FIELD_CHECKBOX, {}, 0 },
            { FIELD_CHECKBOX, {}, 0 },
            { FIELD_CHECKBOX, {}, 0 },
            { FIELD_CHECKBOX, {}, 0 }
        }
    },

    {
        "CFGSIGNAL", "GNSS & Signal",
        "Enable/Disable specific frequency bands.\nNote: L1 frequency bands on some models cannot be disabled.",
        { "GPS_Sig", "GLO_Sig", "GAL_Sig", "BDS_Sig", "QZS_Sig", "NAC_Sig" }, 6,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        true,
        {
            { FIELD_BITMASK, {}, 0 },
            { FIELD_BITMASK, {}, 0 },
            { FIELD_BITMASK, {}, 0 },
            { FIELD_BITMASK, {}, 0 },
            { FIELD_BITMASK, {}, 0 },
            { FIELD_BITMASK, {}, 0 }
        }
    },

    {
        "CFGSIGNAL2", "GNSS & Signal",
        "Antenna 2 Signal Mask.\n"
        "Enable/Disable specific frequency bands using Hex bitmasks.",
        { "GPS_Sig", "GLO_Sig", "GAL_Sig", "BDS_Sig", "QZS_Sig", "NAC_Sig" }, 6,
        { { {0}, 0 } }, 1,
        { "LG290P", "LG293P" }, 2,
        true,
        { { 0 } }
    },

    {
        "CFGSIGGRP", "GNSS & Signal",
        "SigGrpNum:\n"
        " 0 = Default\n"
        " 1 = Disable Antenna 2 (LG580P)\n"
        " 2 = Dual Antenna (LG580P)",
        { "SigGrpNum" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGSBAS", "GNSS & Signal",
        "Enable or disable specific SBAS systems.",
        { "Value" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        true,
        {
            { FIELD_BITMASK, {}, 0 }
        }
    },

    {
        "CFGELETHD", "GNSS & Signal",
        "Ele: Elevation threshold for position engine\n"
        "Range: -90.0 to 90.0 (-90.0 = no limitation)",
        { "Ele" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGCNRTHD", "GNSS & Signal",
        "CNR: Carrier-to-Noise Ratio threshold (dBHz)\n"
        "Range: 0.0 to 99.0 (0 = no limitation)",
        { "CNR" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    /* ── Navigation & Timing ────────────────────────────── */

    {
        "CFGNAVMODE", "Navigation & Timing",
        "Mode:\n"
        " 0 = Normal (Driving)\n"
        " 5 = Dynamic flight (LG580P Default)\n"
        " 11 = Mower (LG290P Default)\n"
        " 14 = Agriculture",
        { "Mode" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGFIXRATE", "Navigation & Timing",
        "FixInterval: GNSS output fix interval in milliseconds.\n"
        "(e.g., 1000 = 1Hz, 100 = 10Hz)",
        { "FixInterval" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGSTANDALONE", "Navigation & Timing",
        "Mode: 0=Disable, 1=Enable\n"
        "Time: Wait time to automatically enter standalone mode (s)\n"
        "Timeout: Duration of mode (s)\n"
        "Lat/Lon/Alt: Initial position override (opt)",
        { "Mode", "Time", "Timeout", "Latitude", "Longitude", "Altitude" }, 6,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGPPP", "Navigation & Timing",
        "Mode: 0x00=Disable, 0x01=B2b PPP, 0x02=E6 HAS, 0xFF=Auto\n"
        "Datum: 1=WGS84, 2=PPP original, 3=CGCS2000",
        { "Mode", "Datum", "Timeout", "HorStd", "VerStd" }, 5,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGGEOSEP", "Navigation & Timing",
        "Mode: 0=Auto (uses built-in geoidal separation table), 1=Manual\n"
        "GeoSep: Custom value if Mode=1",
        { "Mode", "GeoSep" }, 2,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGODO", "Navigation & Timing",
        "State: 0=Disable, 1=Enable\n"
        "InitDist: Initial accumulated distance (m)",
        { "State", "InitDist" }, 2,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGWN", "Navigation & Timing",
        "WN: Reference start GPS week number (e.g., 2200)",
        { "WN" }, 1,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGPPS", "Navigation & Timing",
        "Index: 1\n"
        "Enable: 0=Disable, 1=Enable\n"
        "Duration: Pulse duration (ms)\n"
        "Mode: 1=Always output, 2=Output only in 2D/3D fix\n"
        "Polarity: 0=Low, 1=High",
        { "Index", "Enable", "Duration", "Mode", "Polarity", "Reserved" }, 6,
        { { {1}, 1 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGPPS2", "Navigation & Timing",
        "Same as CFGPPS, with extended features.\n"
        "Period: Pulse output period (ms)",
        { "Index", "Enable", "Duration", "Mode", "Polarity", "Reserved1",
          "Period", "Userdelay", "Reserved2", "Reserved3", "Reserved4", "Reserved5" }, 12,
        { { {1}, 1 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGGEOFENCE", "Navigation & Timing",
        "Index: 0-3\n"
        "Mode: 0=Disable, 1=Enable\n"
        "Shape: 0=Circle(center/radius), 1=Circle(center/point),\n"
        "       2=Triangle, 3=Quad\n"
        "Lat1/Rad: Serves as Radius (m) if Shape=0",
        { "Index", "Mode", "Reserved", "Shape", "Lat0", "Lon0",
          "Lat1/Rad", "Lon1", "Lat2", "Lon2", "Lat3", "Lon3" }, 12,
        { { {0}, 1 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    /* ── Antenna & Formatting ───────────────────────────── */

    {
        "CFGANTINF", "Antenna & Formatting",
        "Configures RTCM3 1033 information.\n"
        "AntDsc: String (max 31 chars)\n"
        "AntSetupID: 0-255\n"
        "AntSN: String (max 31 chars)",
        { "AntDsc", "AntSetupID", "AntSN" }, 3,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGANTDELTA", "Antenna & Formatting",
        "Deviations from reference point to antenna (m).",
        { "East", "North", "Height" }, 3,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGNMEADP", "Antenna & Formatting",
        "Sets decimal places for NMEA fields.\n"
        "UTC/ALT/SPD/COG: 0-3\n"
        "POS: 0-8\n"
        "DOP: 0-3",
        { "UTC_DP", "POS_DP", "ALT_DP", "DOP_DP", "SPD_DP", "COG_DP" }, 6,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGNMEATID", "Antenna & Formatting",
        "Main_TalkerID: 00=Auto, or specific 2-char string (e.g., GN)\n"
        "GSV_TalkerID: 0=Auto, 1=Same as Main",
        { "Main_TalkerID", "GSV_TalkerID" }, 2,
        { { {0}, 0 } }, 1,
        { NULL, NULL }, 0,
        false,
        { { 0 } }
    },

    {
        "CFGBLD", "Antenna & Formatting",
        "Baseline distance between two antennas (m).\n"
        "0 = Baseline distance will be calculated by software.",
        { "Distance" }, 1,
        { { {0}, 0 } }, 1,
        { "LG290P", "LG293P" }, 2,
        false,
        { { 0 } }
    },

    {
        "CFGANTENNA", "Antenna & Formatting",
        "Antenna power supply.\n"
        "0 = Power off\n"
        "1 = Power on",
        { "Power", "Reserved" }, 2,
        { { {0}, 0 } }, 1,
        { "LG290P", "LG293P" }, 2,
        false,
        { { 0 } }
    },
};

#define NUM_CFG ((int)(sizeof CFG / sizeof CFG[0]))

const ConfigEntry *quectel_get_config_entries(int *n)
{
    *n = NUM_CFG;
    return CFG;
}

/* ═══════════════════════════════════════════════════════════
 * 5. SERIAL HELPERS
 * ═══════════════════════════════════════════════════════════ */

static speed_t baud_to_speed(int b)
{
    switch (b) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:     return B460800;
    }
}

static int serial_readline(int fd, char *buf, int max, int tmo_ms)
{
    int pos = 0;
    gint64 deadline = g_get_monotonic_time() + (gint64)tmo_ms * 1000;

    while (pos < max - 1) {
        gint64 rem = (deadline - g_get_monotonic_time()) / 1000;
        if (rem <= 0) break;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, (int)rem);
        if (r < 0)  { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;

        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0)  { if (errno == EINTR) continue; return -1; }
        if (n == 0) break;

        if (c == '\n') {
            if (pos > 0 && buf[pos - 1] == '\r') pos--;
            buf[pos] = '\0';
            return pos;
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (pos > 0) ? pos : -1;
}

/* ═══════════════════════════════════════════════════════════
 * 6. DEVICE LIFECYCLE
 * ═══════════════════════════════════════════════════════════ */

void quectel_init(QuectelDevice *d)
{
    d->fd = -1;
    strcpy(d->model, "LG290P");
    d->supported_msgs     = NULL;
    d->num_supported_msgs = 0;
    d->timeout_sec        = SERIAL_TIMEOUT_SEC;
    d->command_busy       = false;
}

void quectel_cleanup(QuectelDevice *d)
{
    quectel_disconnect(d);
    if (d->supported_msgs) {
        for (int i = 0; i < d->num_supported_msgs; i++)
            g_free(d->supported_msgs[i]);
        g_free(d->supported_msgs);
        d->supported_msgs = NULL;
        d->num_supported_msgs = 0;
    }
}

bool quectel_is_connected(const QuectelDevice *d)
{
    return d->fd >= 0;
}

void quectel_disconnect(QuectelDevice *d)
{
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    plog(LOG_INFO, "--- Disconnected ---");
}

/* Forward declarations */
static void detect_model(QuectelDevice *d);
static void configure_msgs(QuectelDevice *d);

bool quectel_connect(QuectelDevice *d, const char *port, int baud)
{
    d->fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (d->fd < 0) {
        plog(LOG_ERROR, "Connection Failed: %s", strerror(errno));
        return false;
    }
    fcntl(d->fd, F_SETFL, fcntl(d->fd, F_GETFL, 0) & ~O_NONBLOCK);

    struct termios tty;
    memset(&tty, 0, sizeof tty);
    if (tcgetattr(d->fd, &tty) != 0) {
        plog(LOG_ERROR, "tcgetattr: %s", strerror(errno));
        close(d->fd);
        d->fd = -1;
        return false;
    }

    speed_t sp = baud_to_speed(baud);
    cfsetispeed(&tty, sp);
    cfsetospeed(&tty, sp);

    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY |
                      IGNBRK | BRKINT | PARMRK | ISTRIP |
                      INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    tcflush(d->fd, TCIFLUSH);
    if (tcsetattr(d->fd, TCSANOW, &tty) != 0) {
        plog(LOG_ERROR, "tcsetattr: %s", strerror(errno));
        close(d->fd);
        d->fd = -1;
        return false;
    }

    plog(LOG_INFO, "--- Connected to %s @ %d baud ---", port, baud);
    detect_model(d);
    configure_msgs(d);
    return true;
}

/* ═══════════════════════════════════════════════════════════
 * 7. MODEL DETECTION & DYNAMIC MESSAGES
 * ═══════════════════════════════════════════════════════════ */

static void detect_model(QuectelDevice *d)
{
    char *resp = quectel_send_raw(d, "PQTMVERNO", "$PQTMVERNO,");
    if (resp && !strstr(resp, ",ERROR")) {
        char *cpy = g_strdup(resp);
        char *sv;
        (void)strtok_r(cpy, ",", &sv);
        char *ver = strtok_r(NULL, ",", &sv);
        if (ver && strstr(ver, "P0")) {
            char *z = strchr(ver, '0');
            if (z) {
                int n = (int)(z - ver);
                if (n > 0 && n < 28) {
                    memcpy(d->model, ver, n);
                    d->model[n] = '\0';
                    strcat(d->model, "0P");
                } else {
                    strcpy(d->model, "LG290P");
                }
            } else {
                strcpy(d->model, "LG290P");
            }
        } else {
            strcpy(d->model, "LG290P");
        }
        g_free(cpy);
        plog(LOG_INFO, "Detected Model: %s", d->model);
    } else {
        strcpy(d->model, "LG290P");
        plog(LOG_WARNING,
             "Failed to auto-detect model. Defaulting to LG290P.");
    }
    g_free(resp);
}

static void configure_msgs(QuectelDevice *d)
{
    if (d->supported_msgs) {
        for (int i = 0; i < d->num_supported_msgs; i++)
            g_free(d->supported_msgs[i]);
        g_free(d->supported_msgs);
    }

    int bc = 0;
    while (BASE_NMEA[bc]) bc++;

    bool is290 = (strcmp(d->model, "LG290P") == 0);
    int ec = 0;
    if (!is290) {
        while (EXTRA_NMEA[ec]) ec++;
    }

    d->num_supported_msgs = bc + ec;
    d->supported_msgs = g_malloc(sizeof(char *) * d->num_supported_msgs);
    for (int i = 0; i < bc; i++)
        d->supported_msgs[i] = g_strdup(BASE_NMEA[i]);
    for (int i = 0; i < ec; i++)
        d->supported_msgs[bc + i] = g_strdup(EXTRA_NMEA[i]);
}

/* ═══════════════════════════════════════════════════════════
 * 8. PROTOCOL COMMANDS
 * ═══════════════════════════════════════════════════════════ */

static unsigned char calc_cksum(const char *s)
{
    unsigned char ck = 0;
    for (; *s; s++) ck ^= (unsigned char)*s;
    return ck;
}

char *quectel_send_raw(QuectelDevice *d, const char *cmd,
                       const char *wait_for)
{
    if (d->fd < 0) {
        plog(LOG_ERROR, "Not connected.");
        return NULL;
    }

    d->command_busy = true;

    char sentence[512];
    snprintf(sentence, sizeof sentence, "$%s*%02X\r\n",
             cmd, calc_cksum(cmd));

    tcflush(d->fd, TCIFLUSH);
    if (write(d->fd, sentence, strlen(sentence)) < 0)
        plog(LOG_ERROR, "write: %s", strerror(errno));

    plog(LOG_INFO, "TX: $%s*%02X", cmd, calc_cksum(cmd));

    if (!wait_for) {
        d->command_busy = false;
        return NULL;
    }

    int tmo_ms = (int)(d->timeout_sec * 1000);
    gint64 deadline = g_get_monotonic_time() + (gint64)tmo_ms * 1000;
    char line[LINE_BUF_SIZE];

    while (g_get_monotonic_time() < deadline) {
        int rem = (int)((deadline - g_get_monotonic_time()) / 1000);
        if (rem <= 0) break;
        int n = serial_readline(d->fd, line, sizeof line, rem);
        if (n <= 0) continue;

        if (strncmp(line, "$PQTM", 5) == 0) {
            if (strstr(line, ",ERROR,"))
                plog(LOG_ERROR, "RX: %s", line);
            else
                plog(LOG_INFO, "RX: %s", line);
        }
        if (strncmp(line, wait_for, strlen(wait_for)) == 0) {
            d->command_busy = false;
            return g_strdup(line);
        }
    }

    plog(LOG_WARNING, "Timeout waiting for: %s", wait_for);
    d->command_busy = false;
    return NULL;
}

char **quectel_read_config(QuectelDevice *d, const char *cfg,
                           int argc, const char **argv, int *out_n)
{
    *out_n = 0;
    char cmd[512];
    int p = snprintf(cmd, sizeof cmd, "PQTM%s,R", cfg);
    for (int i = 0; i < argc; i++)
        p += snprintf(cmd + p, sizeof(cmd) - p, ",%s", argv[i]);

    char wf[64];
    snprintf(wf, sizeof wf, "$PQTM%s,", cfg);

    char *resp = quectel_send_raw(d, cmd, wf);
    if (!resp || !strstr(resp, ",OK")) {
        g_free(resp);
        return NULL;
    }

    /* strip checksum */
    char *star = strchr(resp, '*');
    if (star) *star = '\0';

    /* tokenise */
    char **all = NULL;
    int total = 0;
    char *sv, *tok = strtok_r(resp, ",", &sv);
    while (tok) {
        all = g_realloc(all, sizeof(char *) * (total + 1));
        all[total++] = g_strdup(tok);
        tok = strtok_r(NULL, ",", &sv);
    }
    g_free(resp);

    if (total <= 2) {
        for (int i = 0; i < total; i++) g_free(all[i]);
        g_free(all);
        return NULL;
    }

    /* return from index 2 */
    int rn = total - 2;
    char **res = g_malloc(sizeof(char *) * rn);
    for (int i = 0; i < rn; i++)
        res[i] = all[i + 2];
    g_free(all[0]);
    g_free(all[1]);
    g_free(all);
    *out_n = rn;
    return res;
}

bool quectel_write_config(QuectelDevice *d, const char *cfg,
                          int argc, const char **argv)
{
    char cmd[512];
    int p = snprintf(cmd, sizeof cmd, "PQTM%s,W", cfg);
    for (int i = 0; i < argc; i++)
        p += snprintf(cmd + p, sizeof(cmd) - p, ",%s", argv[i]);

    char wf[64];
    snprintf(wf, sizeof wf, "$PQTM%s,", cfg);

    char *resp = quectel_send_raw(d, cmd, wf);
    bool ok = resp && strstr(resp, ",OK");
    g_free(resp);
    return ok;
}

bool quectel_save_nvm(QuectelDevice *d)
{
    char *r = quectel_send_raw(d, "PQTMSAVEPAR", "$PQTMSAVEPAR,");
    bool ok = r && strstr(r, ",OK");
    g_free(r);
    return ok;
}

void quectel_gnss_start(QuectelDevice *d)
{
    char *r = quectel_send_raw(d, "PQTMGNSSSTART", "$PQTMGNSSSTART");
    g_free(r);
    plog(LOG_INFO, "GNSS START command sent.");
}

void quectel_gnss_stop(QuectelDevice *d)
{
    char *r = quectel_send_raw(d, "PQTMGNSSSTOP", "$PQTMGNSSSTOP");
    g_free(r);
    plog(LOG_INFO, "GNSS STOP command sent.");
}

void quectel_reboot(QuectelDevice *d)
{
    char *r = quectel_send_raw(d, "PQTMSRR", "$PQTMSRR");
    g_free(r);
    plog(LOG_INFO, "Receiver Reboot command sent.");
}
