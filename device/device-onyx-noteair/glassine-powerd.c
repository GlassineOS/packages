/* glassine-powerd: the Onyx Boox Note Air's sleep policy and front light.
 *
 * Suspend on KEY_POWER (pm8941_pwrkey), on the cover closing (gpio-keys
 * SW_LID = 1) or after idle_sleep_min without any input. Then: front light
 * off, WiFi and Bluetooth off (policy), glassine-epdd-desktop draws its sleep
 * banner, then `mem` > /sys/power/state, which returns after ANY wakeup.
 *
 * Sleep is sticky: only the user ends it. After each wakeup the interrupts that
 * fired are compared (/proc/interrupts, by name): power key, cover (hall) or
 * USB plug-in wake up for real (light + WiFi back, banner replaced); anything
 * else (a WLAN packet pattern, the fuel gauge) goes straight back to sleep. A
 * refused suspend (EBUSY: a wakeup source held for a moment) is retried with a
 * growing pause and logs the active wakeup sources.
 * wifi_off_after_min > 0 keeps WiFi through short sleeps: an RTC alarm wakes us
 * after that long, WiFi goes off, and back to sleep.
 * With USB connected the kernel refuses to suspend: not tried, and no idle
 * sleep then. No idle sleep either while an ALSA playback stream runs (music;
 * PipeWire closes the device ~5 s after the last sound) or while
 * /run/glassine-no-sleep exists (a root job that must finish).
 *
 * Config /etc/glassine/power.conf (key=value, re-read when it changes; written by
 * glassine-settings and glassine-shell as the desktop user, group video):
 *   power_key=suspend|ignore   cover=suspend|ignore
 *   idle_sleep_min=N (0 = never)   wifi_off_after_min=N (0 = at once, -1 = never)
 *   light_br=0..28  light_ct=0..24  (front light, restored at start and on wake;
 *       no light_br: the light is left as it is at start)
 *   light_managed=0|1  (1: brightness and warmth follow the sun, see managed_tick)
 *   light_b_min/light_b_max, light_w_min/light_w_max = 0..100 %  (night/day limits)
 *   light_b_off, light_w_off = -50..50  (the user's shift of the schedule)
 *   a managed brightness never goes below LIGHT_FLOOR (5 %): in the dark the
 *   screen must stay readable, whatever the schedule and the offset say
 * /run/glassine-light (written here, read by glassine-settings and glassine-shell):
 * the zone, today's sunrise/sunset (local hours) and the scheduled
 * brightness/warmth (%).
 * Each sleep logs its length, the battery's capacity and voltage, and the RPM
 * vmin/vlow counts and time (qcom_stats). Sensor hub streams (qcom-smgr-*) are
 * paused for the sleep.
 *
 * SIGUSR1 = sleep now (the shell's Power popup, via glassine-power-root sleep).
 *
 * usage: glassine-powerd [-n]   (-n: log, do not suspend or switch radios)
 *        glassine-powerd -p     (print the configuration and the light it sets, then exit)
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <math.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <glob.h>
#include <sys/ioctl.h>

#define SLEEP_PILL "/run/glassine-sleep-pill.pam"      /* glassine-epdd-desktop PILL_PATH */
#define SLEEP_PILL_BIN "/usr/bin/glassine-sleep-pill"
#define CTL "/run/glassine-epdd-desktop.ctl"
static const char *CONF = "/etc/glassine/power.conf";
/* the LM3630A's two banks, 0..255; a is the warm one. br/ct keep the ranges of
 * Onyx's own light controls in the config (0..28, 0..24). */
#define FL_A "/sys/class/backlight/frontlight-a/brightness"
#define FL_B "/sys/class/backlight/frontlight-b/brightness"
#define FL_WARM FL_A
#define FL_COOL FL_B
#define RTC_ALARM "/sys/class/rtc/rtc0/wakealarm"
#define LIGHT_RUN "/run/glassine-light"
#define ZONEINFO "/usr/share/zoneinfo/"
#define GRACE_MS 1500
#define MAXDEV 16

static struct {
    int power_key, cover;           /* 1 = suspend */
    int idle_min, wifi_after;
    int br, ct;
    int br_set;                     /* power.conf has light_br */
} cf = { 1, 1, 10, 0, 0, 12, 0 };
/* managed light: limits and offsets in %, the schedule's current value, and what is on */
static struct { int on, bmin, bmax, wmin, wmax, boff, woff, b, w, br, ct; } lm = { 0, 20, 80, 10, 90, 0, 0, -1, -1, -1, -1 };
#define LIGHT_FLOOR 5                   /* % (glassine-settings and glassine-shell keep the same) */
static time_t conf_mtime = -1;
static void managed_tick(void);
static int dry, print_only;

/* the /proc/interrupts names of the wakeups that end a sleep */
static const char *wake_irqs[] = { "pm8941_pwrkey", "Hall Effect Sensor", "usb-plugin" };
#define NWAKE (sizeof wake_irqs / sizeof *wake_irqs)

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void stamp(void)
{
    time_t t = time(NULL);
    char b[32];
    strftime(b, sizeof b, "%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "%s glassine-powerd: ", b);
}

static int wr(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    int ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    close(fd);
    return ok ? 0 : -1;
}

static long rd_long(const char *path, long def)
{
    char b[32] = "";
    int fd = open(path, O_RDONLY);
    if (fd < 0) return def;
    ssize_t k = read(fd, b, sizeof b - 1);
    close(fd);
    return k > 0 ? strtol(b, NULL, 10) : def;
}

static void epdd(const char *cmd)
{
    int fd = open(CTL, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return;
    if (write(fd, cmd, strlen(cmd)) < 0) { /* epdd not running: nothing to draw */ }
    close(fd);
}

static int clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
/* the managed brightness (%): the schedule + the offset, never below the floor */
static int managed_b(void) { int b = clamp100(lm.b + lm.boff); return b < LIGHT_FLOOR ? LIGHT_FLOOR : b; }

/* the LM3630A's full-scale current per bank (0..31): the kernel's current_a|b
 * (the driver sets it only at probe otherwise) */
static void fl_current(int a, int b)
{
    glob_t g;
    char p[160], v[16];
    int done = 0;
    if (!glob("/sys/bus/i2c/devices/*-0038/current_a", 0, NULL, &g)) {
        snprintf(p, sizeof p, "%s", g.gl_pathv[0]);
        snprintf(v, sizeof v, "%d\n", a);
        done = !wr(p, v);
        p[strlen(p) - 1] = 'b';
        snprintf(v, sizeof v, "%d\n", b);
        done &= !wr(p, v);
        globfree(&g);
    }
    static int warned;
    if (!done && !warned) { warned = 1; stamp(); fprintf(stderr, "light: cannot set the LM3630A current\n"); }
}

/* br 0..28 -> 0..255 on a square curve, split between the banks by ct 0..24 */
static void light_codes(int br, int ct, int *warm, int *cool)
{
    ct = ct < 0 ? 0 : ct > 24 ? 24 : ct;
    int total = br <= 0 ? 0 : br >= 28 ? 255 : 1 + 254 * br * br / (28 * 28);
    *warm = total * ct / 24;
    *cool = total - *warm;              /* the rest: a dim light split both ways isn't 0 + 0 */
}

static void light(int on)
{
    char b[16];
    if (lm.on && lm.b >= 0) {           /* managed: the schedule + the user's offset */
        cf.br = (int)lround(managed_b() * 28 / 100.0);
        cf.ct = (int)lround(clamp100(lm.w + lm.woff) * 24 / 100.0);
    } else if (on && !cf.br_set) {      /* no light in the config: leave it as it is */
        return;
    }
    lm.br = on ? cf.br : -1; lm.ct = on ? cf.ct : -1;
    int warm, cool;
    light_codes(on ? cf.br : 0, cf.ct, &warm, &cool);
    if (print_only) { printf("light %d %d\n", warm, cool); return; }
    fl_current(31, 31);
    snprintf(b, sizeof b, "%d\n", warm); wr(FL_WARM, b);
    snprintf(b, sizeof b, "%d\n", cool); wr(FL_COOL, b);
}

static void load_conf(int force)
{
    struct stat st;
    if (stat(CONF, &st) < 0) { if (force) light(1); return; }
    if (!force && st.st_mtime == conf_mtime) return;
    conf_mtime = st.st_mtime;
    FILE *f = fopen(CONF, "r");
    if (!f) return;
    char line[128], k[64], v[64];
    cf.br_set = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %63[a-z_] = %63s", k, v) != 2) continue;
        if (!strcmp(k, "power_key")) cf.power_key = strcmp(v, "ignore") != 0;
        else if (!strcmp(k, "cover")) cf.cover = strcmp(v, "ignore") != 0;
        else if (!strcmp(k, "idle_sleep_min")) cf.idle_min = atoi(v);
        else if (!strcmp(k, "wifi_off_after_min")) cf.wifi_after = atoi(v);
        else if (!strcmp(k, "light_br")) { cf.br = atoi(v); cf.br_set = 1; }
        else if (!strcmp(k, "light_ct")) cf.ct = atoi(v);
        else if (!strcmp(k, "light_managed")) lm.on = atoi(v) != 0;
        else if (!strcmp(k, "light_b_min")) lm.bmin = clamp100(atoi(v));
        else if (!strcmp(k, "light_b_max")) lm.bmax = clamp100(atoi(v));
        else if (!strcmp(k, "light_w_min")) lm.wmin = clamp100(atoi(v));
        else if (!strcmp(k, "light_w_max")) lm.wmax = clamp100(atoi(v));
        else if (!strcmp(k, "light_b_off")) lm.boff = atoi(v) < -50 ? -50 : atoi(v) > 50 ? 50 : atoi(v);
        else if (!strcmp(k, "light_w_off")) lm.woff = atoi(v) < -50 ? -50 : atoi(v) > 50 ? 50 : atoi(v);
    }
    fclose(f);
    stamp(); fprintf(stderr, "config: power_key=%d cover=%d idle_sleep_min=%d wifi_off_after_min=%d light=%d/%d",
                     cf.power_key, cf.cover, cf.idle_min, cf.wifi_after, cf.br, cf.ct);
    if (lm.on) fprintf(stderr, " managed b %d-%d%+d w %d-%d%+d", lm.bmin, lm.bmax, lm.boff, lm.wmin, lm.wmax, lm.woff);
    fprintf(stderr, "\n");
}

/* the desktop user (group video) may set the light and write the config */
static void setup_perms(void)
{
    struct group *g = getgrnam("video");
    gid_t gid = g ? g->gr_gid : 27;
    const char *p[] = { FL_A, FL_B };
    for (unsigned i = 0; i < 2; i++) { if (chown(p[i], 0, gid) == 0) chmod(p[i], 0664); }
    mkdir("/etc/glassine", 0755);
    int fd = open(CONF, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd >= 0) close(fd);
    if (chown(CONF, 0, gid) == 0) chmod(CONF, 0664);
}

static int usb_online(void)
{
    return rd_long("/sys/class/power_supply/pm660-charger/online", 0) == 1;
}

/* sum over CPUs of the named interrupts' counts */
static void irq_counts(unsigned long *c)
{
    memset(c, 0, NWAKE * sizeof *c);
    FILE *f = fopen("/proc/interrupts", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t len = strlen(line);
        for (unsigned i = 0; i < NWAKE; i++) {
            size_t k = strlen(wake_irqs[i]);  /* the line ends in " <name>" (a name may hold spaces) */
            if (len <= k || line[len - k - 1] != ' ' || strcmp(line + len - k, wake_irqs[i])) continue;
            char *p = strchr(line, ':');
            if (!p) break;
            p++;
            for (;;) {
                char *e;
                unsigned long v = strtoul(p, &e, 10);
                if (e == p) break;
                c[i] += v;
                p = e;
            }
        }
    }
    fclose(f);
}

/* WiFi via NetworkManager's radio switch (root may, no polkit here) */
static int wifi_on(void)
{
    FILE *p = popen("nmcli radio wifi 2>/dev/null", "r");
    char b[32] = "";
    if (p) { if (!fgets(b, sizeof b, p)) b[0] = 0; pclose(p); }
    return !strncmp(b, "enabled", 7);
}
/* NetworkManager keeps the radio switch across reboots: a reboot while asleep
 * (WiFi off by us) came back with WiFi off. The marker says the off was ours;
 * at start it is turned back on once NetworkManager answers. */
#define WIFI_MARK "/var/lib/glassine/wifi-off-by-sleep"
static int wifi_set(int on)
{
    stamp(); fprintf(stderr, "wifi %s\n", on ? "on" : "off");
    if (dry) return 0;
    if (!on) { mkdir("/var/lib/glassine", 0755); close(open(WIFI_MARK, O_WRONLY | O_CREAT, 0644)); }
    if (system(on ? "nmcli radio wifi on" : "nmcli radio wifi off") != 0) {
        stamp(); fprintf(stderr, "nmcli failed\n");
        return -1;
    }
    if (on) unlink(WIFI_MARK);
    return 0;
}

/* Bluetooth via its rfkill switch (type bluetooth) */
static char bt_state[300];
static int bt_on(void)
{
    if (!bt_state[0]) {
        DIR *d = opendir("/sys/class/rfkill");
        struct dirent *e;
        char p[300], name[32];
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, "/sys/class/rfkill/%s/type", e->d_name);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            if (fgets(name, sizeof name, f) && !strncmp(name, "bluetooth", 9))
                snprintf(bt_state, sizeof bt_state, "/sys/class/rfkill/%s/state", e->d_name);
            fclose(f);
        }
        if (d) closedir(d);
        if (!bt_state[0]) return 0;
    }
    return rd_long(bt_state, 0) == 1;
}
static void bt_set(int on)
{
    if (!bt_state[0] || dry) return;
    stamp(); fprintf(stderr, "bluetooth %s\n", on ? "on" : "off");
    wr(bt_state, on ? "1" : "0");
}

/* the fuel gauge: only the percentage, no charge counter */
static long capacity(void) { return rd_long("/sys/class/power_supply/qcom-battery/capacity", -1); }
/* battery voltage in mV: finer than the whole-percent capacity (a few mV per %) */
static long batt_mv(void) { long uv = rd_long("/sys/class/power_supply/qcom-battery/voltage_now", -1); return uv > 0 ? uv / 1000 : -1; }

/* RPM system sleep stats (qcom_stats module, debugfs): vmin = VDD-min, vlow =
 * XO shutdown. Count and accumulated time (19.2 MHz ticks); -1 if unreadable.
 * They prove whether a sleep reached the deep state; a reboot resets them. */
struct sstat { long long n, ticks; };
static struct sstat sleep_stat(const char *name)
{
    struct sstat st = { -1, -1 };
    char p[64], l[96];
    snprintf(p, sizeof p, "/sys/kernel/debug/qcom_stats/%s", name);
    FILE *f = fopen(p, "r");
    if (!f) return st;
    while (fgets(l, sizeof l, f)) {
        if (!strncmp(l, "Count:", 6)) st.n = strtoll(l + 6, NULL, 10);
        else if (!strncmp(l, "Accumulated Duration:", 21)) st.ticks = strtoll(l + 21, NULL, 10);
    }
    fclose(f);
    return st;
}
static void log_sleep_stat(const char *name, struct sstat a, struct sstat b, time_t dt)
{
    if (a.n < 0 || b.n < 0 || a.ticks < 0 || b.ticks < 0) return;
    long s = (long)((b.ticks - a.ticks) / 19200000);
    fprintf(stderr, ", %s +%lld (%ldh%02ldm", name, b.n - a.n, s / 3600, s / 60 % 60);
    if (dt > 0) fprintf(stderr, " = %ld %%", s * 100 / dt);
    fprintf(stderr, ")");
}

/* dwc3 comes back from suspend dead ("HS-PHY not in L2", "failed to
 * enable ep0out"): detach the gadget before suspending, afterwards rebind the
 * qcom glue (re-inits the PHY; a UDC re-attach or dwc3 core rebind doesn't
 * help) and attach it again.
 * The rebind once hung the whole SoC: a race in
 * the glue unbind, where dwc3_qcom_remove re-allowed runtime PM after
 * switching the block off and a runtime-suspend worker read the dead
 * registers; fixed in the kernel. Still, on battery nothing needs USB,
 * so the rebind waits until a cable is plugged in (usb_pending; the main loop
 * checks it): fewer rebinds, fewer chances. */
static char gadget_udc[200];            /* the first gadget's UDC file */
#define GLUE_DRV "/sys/bus/platform/drivers/dwc3-qcom-legacy/"
#define GLUE_DEV "a8f8800.usb"
static char udc[64];
static int usb_pending;                 /* woke on battery: rebind at the next plug-in */
static void usb_detach(void)
{
    if (dry) return;
    if (usb_pending) return;            /* still detached since the last sleep; keep udc */
    udc[0] = 0;
    glob_t g;
    gadget_udc[0] = 0;
    if (!glob("/sys/kernel/config/usb_gadget/*/UDC", 0, NULL, &g)) {
        snprintf(gadget_udc, sizeof gadget_udc, "%s", g.gl_pathv[0]);
        globfree(&g);
    }
    if (!gadget_udc[0]) return;
    int fd = open(gadget_udc, O_RDONLY);
    if (fd < 0) return;
    ssize_t k = read(fd, udc, sizeof udc - 1);
    close(fd);
    udc[k > 0 ? k : 0] = 0;
    char *nl = strchr(udc, '\n');
    if (nl) *nl = 0;
    if (udc[0]) wr(gadget_udc, "\n");
}
static void usb_reattach(void)
{
    if (dry) return;
    usb_pending = 0;
    wr(GLUE_DRV "unbind", GLUE_DEV);
    usleep(500000);
    wr(GLUE_DRV "bind", GLUE_DEV);
    if (!udc[0] || !gadget_udc[0]) return;
    char p[128];
    snprintf(p, sizeof p, "/sys/class/udc/%s", udc);
    for (int i = 0; i < 50 && access(p, F_OK); i++) usleep(100000);
    int r = wr(gadget_udc, udc);
    stamp(); fprintf(stderr, "usb: glue rebound, gadget %s %s\n", udc, r < 0 ? "NOT re-attached" : "re-attached");
}

/* the kernel's active wakeup sources (debugfs; read-only software state), for
 * the log when a suspend is refused */
static void log_wakelocks(void)
{
    FILE *f = fopen("/sys/kernel/debug/wakeup_sources", "r");
    if (!f) return;
    char line[256], name[64], list[512] = "";
    unsigned long active_count, event_count, wakeup_count, expire_count, active_since;
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }      /* header */
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63s %lu %lu %lu %lu %lu", name, &active_count, &event_count, &wakeup_count,
                   &expire_count, &active_since) != 6 || !active_since) continue;
        if (strlen(list) + strlen(name) + 2 < sizeof list) { strcat(list, " "); strcat(list, name); }
    }
    fclose(f);
    stamp(); fprintf(stderr, "active wakeup sources:%s\n", list[0] ? list : " (none)");
}

/* an ALSA playback stream is running (music) */
static int audio_playing(void)
{
    DIR *d = opendir("/proc/asound");
    struct dirent *c, *p;
    int r = 0;
    while (d && !r && (c = readdir(d))) {
        if (strncmp(c->d_name, "card", 4)) continue;
        char path[300];
        snprintf(path, sizeof path, "/proc/asound/%s", c->d_name);
        DIR *cd = opendir(path);
        while (cd && !r && (p = readdir(cd))) {
            size_t n = strlen(p->d_name);
            if (strncmp(p->d_name, "pcm", 3) || n < 4 || p->d_name[n - 1] != 'p') continue;
            char st[400], line[128];
            snprintf(st, sizeof st, "%s/%s/sub0/status", path, p->d_name);
            FILE *f = fopen(st, "r");
            if (!f) continue;
            while (fgets(line, sizeof line, f)) if (strstr(line, "RUNNING")) r = 1;
            fclose(f);
        }
        if (cd) closedir(cd);
    }
    if (d) closedir(d);
    return r;
}

static void on_alarm(int sig) { (void)sig; }

/* The kernel's handshake against a suspend racing busy work: reading
 * wakeup_count blocks while a wakeup source is active (glassine-epdd-desktop holds one
 * while panel frames are queued or on the wire, kernel drivers their own),
 * writing it back arms the check, and "mem" then fails (EBUSY) if anything
 * fired since. Bounded: a source still active after WC_WAIT_S counts as a
 * refusal, so the caller logs it and retries; after a few it goes without
 * (handshake = 0), so one stuck source cannot keep the device awake. */
#define WC_WAIT_S 5
static int suspend_once(int handshake)
{
    if (dry) { sleep(3); return 0; }
    if (handshake) {
        int wc = open("/sys/power/wakeup_count", O_RDWR);
        if (wc >= 0) {
            struct sigaction sa = { .sa_handler = on_alarm }, was;  /* no SA_RESTART: read gets EINTR */
            sigaction(SIGALRM, &sa, &was);
            alarm(WC_WAIT_S);
            char b[32];
            ssize_t n = read(wc, b, sizeof b - 1);
            int err = errno;
            alarm(0);
            sigaction(SIGALRM, &was, NULL);
            if (n <= 0) {
                close(wc);
                stamp(); fprintf(stderr, "wakeup sources still active after %d s\n", WC_WAIT_S);
                return -(n < 0 && err != EINTR ? err : EBUSY);
            }
            /* back into the same open file: lseek, not a reopen (sysfs) */
            lseek(wc, 0, SEEK_SET);
            int ok = write(wc, b, n) == n;
            err = errno;
            close(wc);
            if (!ok) return -(err ? err : EBUSY);   /* EINVAL: an event since the read */
        }
    }
    int fd = open("/sys/power/state", O_WRONLY);
    int ok = fd >= 0 && write(fd, "mem", 3) == 3;
    int err = errno;
    if (fd >= 0) close(fd);
    return ok ? 0 : -err;
}

/* the sensor hub's IIO streams (qcom-smgr-*: the rotation's accelerometer)
 * off for the sleep: every sample is a QMI report that wakes the APSS, and the
 * sticky sleep would re-suspend several times a second. Bit i of the result:
 * iio:device<i> was streaming (turned on again by sensors_resume). */
static unsigned sensors_pause(void)
{
    unsigned on = 0;
    for (int i = 0; i < 32; i++) {
        char p[96], b[32] = "";
        snprintf(p, sizeof p, "/sys/bus/iio/devices/iio:device%d/name", i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(b, sizeof b, f)) b[0] = 0;
        fclose(f);
        if (strncmp(b, "qcom-smgr-", 10)) continue;
        snprintf(p, sizeof p, "/sys/bus/iio/devices/iio:device%d/buffer/enable", i);
        if (rd_long(p, 0) == 1 && !wr(p, "0")) on |= 1u << i;
    }
    if (on) { stamp(); fprintf(stderr, "sensor streams paused (0x%x)\n", on); }
    return on;
}

static void sensors_resume(unsigned on)
{
    for (int i = 0; i < 32; i++) {
        char p[96];
        if (!(on & 1u << i)) continue;
        snprintf(p, sizeof p, "/sys/bus/iio/devices/iio:device%d/buffer/enable", i);
        wr(p, "1");
    }
}

/* one user-level sleep: lasts until a user wakeup (or USB, or failure) */
static void do_sleep(const char *why)
{
    if (usb_online()) {
        stamp(); fprintf(stderr, "%s: USB connected, suspend impossible, ignored\n", why);
        return;
    }
    stamp(); fprintf(stderr, "sleep (%s)\n", why);
    int handshake = !dry;
    int wifi_was = wifi_on(), wifi_is = wifi_was, bt_was = bt_on();
    /* the sleep pill: the shell's theme, the screen's turn and the time;
     * glassine-epdd-desktop draws it over the page, else its plain bar */
    unlink(SLEEP_PILL);
    if (access(SLEEP_PILL_BIN, X_OK) == 0 &&
        system("timeout 3 " SLEEP_PILL_BIN " " SLEEP_PILL " >/dev/null 2>&1") != 0) {
        stamp(); fprintf(stderr, "sleep pill: renderer failed, the bar instead\n");
    }
    epdd("sleep\n");
    light(0);
    if (bt_was) bt_set(0);
    if (wifi_is && cf.wifi_after == 0 && !dry) { wifi_set(0); wifi_is = 0; }
    /* the banner GC16 must be on the panel before the system freezes, else its
     * queued frames go out at resume and the banner only flashes on wake.
     * glassine-epdd-desktop holds a wakeup source while frames are queued, and
     * the wakeup_count handshake in suspend_once waits for it. Without
     * CONFIG_PM_WAKELOCKS a fixed 1.2 s */
    if (!handshake || access("/sys/power/wake_lock", F_OK)) usleep(1200000);
    sync();

    time_t t0 = time(NULL), alarm_at = 0;
    long p0 = capacity(), v0 = batt_mv();
    struct sstat vmin0 = sleep_stat("vmin"), vlow0 = sleep_stat("vlow");
    usb_detach();
    unsigned sensors = dry ? 0 : sensors_pause();
    if (wifi_is && cf.wifi_after > 0) {
        char b[32];
        alarm_at = t0 + cf.wifi_after * 60;
        wr(RTC_ALARM, "0");
        /* relative: the PMIC RTC is read-only and far from system time */
        snprintf(b, sizeof b, "+%d", cf.wifi_after * 60);
        if (wr(RTC_ALARM, b) < 0) { stamp(); fprintf(stderr, "no RTC alarm: wifi off now\n"); wifi_set(0); wifi_is = 0; alarm_at = 0; }
    }
    int wakes = 0, busy = 0;
    /* against the counts from before the first suspend: a key press between two
     * attempts (during a pause after EBUSY) counts too */
    unsigned long c0[NWAKE], c1[NWAKE];
    irq_counts(c0);
    for (;;) {
        int r = suspend_once(handshake && busy < 4);
        irq_counts(c1);
        int user = -1;
        for (unsigned i = 0; i < NWAKE; i++) if (c1[i] != c0[i]) { user = i; break; }
        if (user >= 0) { stamp(); fprintf(stderr, "woken by %s\n", wake_irqs[user]); break; }
        if (usb_online()) { stamp(); fprintf(stderr, "woken, USB connected\n"); break; }
        if (alarm_at && time(NULL) >= alarm_at - 2) {
            stamp(); fprintf(stderr, "slept %d min: ", cf.wifi_after);
            wifi_set(0); wifi_is = 0; alarm_at = 0;
        }
        if (r < 0) {
            /* 1, 2, ... 10 s between attempts; the wakelock list at the 1st, 10th, ... */
            if (busy++ % 10 == 0) { stamp(); fprintf(stderr, "suspend refused (%d): %s\n", busy, strerror(-r)); log_wakelocks(); }
            sleep(busy < 10 ? busy : 10);
            continue;
        }
        if (busy) { stamp(); fprintf(stderr, "suspended after %d refused attempts\n", busy); busy = 0; }
        wakes++;
        /* a moment for whatever woke us (a packet, a gauge interrupt) to be
         * handled; a repeated suspend right away tends to abort (EBUSY) */
        usleep(500000);
    }
    if (alarm_at) wr(RTC_ALARM, "0");
    time_t dt = time(NULL) - t0;
    long v1 = batt_mv();                /* before the USB/WiFi/light work loads the battery */
    struct sstat vmin1 = sleep_stat("vmin"), vlow1 = sleep_stat("vlow");
    if (usb_online()) usb_reattach();
    else if (!dry && !usb_pending) {
        usb_pending = 1;
        stamp(); fprintf(stderr, "usb: on battery, glue rebind deferred until plug-in\n");
    }
    long p1 = capacity();
    stamp(); fprintf(stderr, "awake after %ldh%02ldm, %d spurious wakeups", (long)dt / 3600, (long)dt / 60 % 60, wakes);
    if (p0 >= 0 && p1 >= 0)
        fprintf(stderr, ", battery %ld%% -> %ld%%", p0, p1);
    if (v0 > 0 && v1 > 0) fprintf(stderr, ", %ld -> %ld mV", v0, v1);
    log_sleep_stat("vmin", vmin0, vmin1, dt);
    log_sleep_stat("vlow", vlow0, vlow1, dt);
    fprintf(stderr, "\n");
    load_conf(0);
    managed_tick();
    light(1);
    if (wifi_was && !wifi_is) wifi_set(1);
    if (bt_was) bt_set(1);
    sensors_resume(sensors);
    epdd("wake\n");
}

/* Managed light: brighter and cooler by day, dimmer and warmer at night (the
 * settings' "Managed brightness & warmth"). Day = sunrise to sunset
 * at the time zone's location (zone1970.tab), with a 90-minute fade centred on
 * each: f = 0 at night, 1 by day; brightness = bmin + (bmax - bmin) * f,
 * warmth = wmax - (wmax - wmin) * f, then the user's offsets on top. */
static double zlat = NAN, zlon = NAN;
static char zname[96];

static int tab_coord(const char *s, int deg_digits, double *out)
{
    int n = 0;
    while (s[1 + n] >= '0' && s[1 + n] <= '9') n++;
    if ((s[0] != '+' && s[0] != '-') || n < deg_digits + 2) return -1;
    char d[8] = "", m[3] = "", sec[3] = "";
    memcpy(d, s + 1, deg_digits); memcpy(m, s + 1 + deg_digits, 2);
    if (n >= deg_digits + 4) memcpy(sec, s + 3 + deg_digits, 2);
    *out = (atoi(d) + atoi(m) / 60.0 + atoi(sec) / 3600.0) * (s[0] == '-' ? -1 : 1);
    return 0;
}

/* the zone name (/etc/localtime -> .../zoneinfo/REGION/CITY) and its coordinates */
static void zone_locate(void)
{
    char z[160] = "", l[256];
    ssize_t k = readlink("/etc/localtime", l, sizeof l - 1);
    const char *p;
    if (k > 0) { l[k] = 0; if ((p = strstr(l, "zoneinfo/"))) snprintf(z, sizeof z, "%s", p + 9); }
    if (!*z) { FILE *f = fopen("/etc/timezone", "r"); if (f) { if (fscanf(f, "%159s", z) != 1) *z = 0; fclose(f); } }
    if (!strcmp(z, zname)) return;
    snprintf(zname, sizeof zname, "%s", z);
    zlat = zlon = NAN;
    const char *tabs[] = { ZONEINFO "zone1970.tab", ZONEINFO "zone.tab" };
    for (int i = 0; i < 2 && isnan(zlat) && *z; i++) {
        FILE *f = fopen(tabs[i], "r");
        char line[512], cc[64], co[32], name[128];
        while (f && fgets(line, sizeof line, f)) {
            if (*line == '#' || sscanf(line, "%63s %31s %127s", cc, co, name) != 3 || strcmp(name, z)) continue;
            char *lon = co + 1;
            while (*lon && *lon != '+' && *lon != '-') lon++;
            double la, lo;
            char lat[16];
            snprintf(lat, sizeof lat, "%.*s", (int)(lon - co), co);
            if (!tab_coord(lat, 2, &la) && !tab_coord(lon, 3, &lo)) { zlat = la; zlon = lo; }
            break;
        }
        if (f) fclose(f);
    }
    stamp(); fprintf(stderr, "light: zone %s at %.2f %.2f\n", *z ? z : "?", zlat, zlon);
}

/* NOAA's sunrise/sunset (zenith 90.833 deg), local hours; no location: 7..19 */
static void sun_times(const struct tm *lt, double *rise, double *set)
{
    if (isnan(zlat)) { *rise = 7; *set = 19; return; }
    double g = 2 * M_PI / 365 * lt->tm_yday;
    double eqt = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
    double dec = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g)
                 - 0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
    double la = zlat * M_PI / 180;
    double c = cos(90.833 * M_PI / 180) / (cos(la) * cos(dec)) - tan(la) * tan(dec);
    double off = lt->tm_gmtoff / 3600.0;
    if (c > 1) { *rise = *set = 12; return; }           /* polar night */
    if (c < -1) { *rise = 0; *set = 24; return; }       /* polar day */
    double ha = acos(c) * 180 / M_PI;
    *rise = (720 - 4 * (zlon + ha) - eqt) / 60 + off;
    *set = (720 - 4 * (zlon - ha) - eqt) / 60 + off;
}

static double fclamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* every loop (10 s) while awake: the schedule now; the light follows it when managed */
static void managed_tick(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    static int yday = -1;
    static double rise, set;
    static time_t zchk;
    if (now - zchk > 60) { zchk = now; zone_locate(); yday = -1; }
    if (lt.tm_yday != yday) { yday = lt.tm_yday; sun_times(&lt, &rise, &set); }
    double x = lt.tm_hour + lt.tm_min / 60.0 + lt.tm_sec / 3600.0;
    double f = fclamp01((x - (rise - 0.75)) / 1.5) * fclamp01((set + 0.75 - x) / 1.5);
    lm.b = (int)lround(lm.bmin + (lm.bmax - lm.bmin) * f);
    lm.w = (int)lround(lm.wmax - (lm.wmax - lm.wmin) * f);
    char buf[256];
    snprintf(buf, sizeof buf, "zone=%s\nlocated=%d\nsunrise=%.3f\nsunset=%.3f\nb=%d\nw=%d\n",
             *zname ? zname : "?", !isnan(zlat), rise, set, lm.b, lm.w);
    static char last[256];
    if (strcmp(buf, last)) {
        snprintf(last, sizeof last, "%s", buf);
        int fd = print_only ? -1 : open(LIGHT_RUN ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (print_only) { fputs(buf, stdout); fd = -1; }
        if (fd >= 0) {
            int ok = write(fd, buf, strlen(buf)) == (ssize_t)strlen(buf);
            close(fd);
            if (ok) rename(LIGHT_RUN ".tmp", LIGHT_RUN);
        }
    }
    if (!lm.on) return;
    int br = (int)lround(managed_b() * 28 / 100.0), ct = (int)lround(clamp100(lm.w + lm.woff) * 24 / 100.0);
    if (br != lm.br || ct != lm.ct) light(1);
}

struct dev { int fd, kind; };           /* kind: 0 other input, 1 power key, 2 hall */

static int scan_inputs(struct dev *d)
{
    int n = 0;
    DIR *dir = opendir("/dev/input");
    struct dirent *e;
    while (dir && (e = readdir(dir)) && n < MAXDEV) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300], name[128] = "";
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        /* the power key and the cover's hall sensor (gpio-keys, SW_LID) */
        int kind = !strcmp(name, "pm8941_pwrkey") ? 1 : !strcmp(name, "gpio-keys") ? 2 : 0;
        static int logged;
        if (kind && logged < 2) { logged++; stamp(); fprintf(stderr, "watching %s \"%s\"\n", path, name); }
        d[n++] = (struct dev){ fd, kind };
    }
    if (dir) closedir(dir);
    return n;
}

/* SIGUSR1: sleep now (the shell's Power popup: glassine-power-root sleep) */
static volatile sig_atomic_t sleep_req;
static void on_usr1(int s) { (void)s; sleep_req = 1; }

int main(int argc, char **argv)
{
    dry = argc > 1 && !strcmp(argv[1], "-n");
    print_only = argc > 1 && !strcmp(argv[1], "-p");
    if (getenv("GLASSINE_POWER_CONF")) CONF = getenv("GLASSINE_POWER_CONF");
    if (print_only) {                   /* the test's view: no files written, nothing switched */
        load_conf(1);
        managed_tick();
        light(1);
        return 0;
    }
    struct sigaction usr1 = { .sa_handler = on_usr1 };     /* poll returns EINTR */
    sigaction(SIGUSR1, &usr1, NULL);
    setup_perms();
    load_conf(1);
    managed_tick();
    light(1);
    struct dev d[MAXDEV];
    int n = scan_inputs(d);
    int64_t awake_since = now_ms(), last_input = now_ms(), last_rescan = now_ms();
    int idle_blocked = 0;               /* "not sleeping because ..." logged for this idle period */
    int wifi_restore = access(WIFI_MARK, F_OK) == 0, wifi_tries = 0;
    for (;;) {
        if (wifi_restore && !dry && wifi_tries++ < 12) {      /* ~2 min for NetworkManager to come up */
            if (wifi_set(1) == 0) wifi_restore = 0;
        }
        struct pollfd pf[MAXDEV];
        for (int i = 0; i < n; i++) pf[i] = (struct pollfd){ d[i].fd, POLLIN, 0 };
        /* 2 s while a USB rebind waits for the cable, so the host sees the gadget soon */
        if (poll(pf, n, usb_pending ? 2000 : 10000) < 0 && errno != EINTR) return 1;
        if (usb_pending && usb_online()) { stamp(); fprintf(stderr, "usb: plugged in\n"); usb_reattach(); }
        for (int i = 0; i < n; i++)      /* a device went away (a pen proxy restart): rescan now */
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) last_rescan = 0;
        load_conf(0);
        managed_tick();
        const char *go = NULL;
        for (int i = 0; i < n; i++) {
            struct input_event ev[32];
            ssize_t k;
            while ((k = read(d[i].fd, ev, sizeof ev)) > 0)
                for (int j = 0; j < (int)(k / sizeof *ev); j++) {
                    last_input = now_ms();
                    idle_blocked = 0;
                    if (!d[i].kind || (ev[j].type != EV_KEY && ev[j].type != EV_SW)) continue;
                    stamp(); fprintf(stderr, "%s %s %d value %d\n", d[i].kind == 1 ? "power" : "hall",
                                     ev[j].type == EV_SW ? "switch" : "key", ev[j].code, ev[j].value);
                    int press = ev[j].type == EV_KEY ? ev[j].code == KEY_POWER && ev[j].value == 1
                                                     : ev[j].code == SW_LID && ev[j].value == 1;  /* closed */
                    if (press && now_ms() - awake_since > GRACE_MS &&
                        (d[i].kind == 1 ? cf.power_key : cf.cover))
                        go = d[i].kind == 1 ? "power key" : "cover";
                }
        }
        if (!go && cf.idle_min > 0 && now_ms() - last_input > (int64_t)cf.idle_min * 60000) {
            /* /run/glassine-no-sleep: a root job that must finish (glassine-setup-apply holds it) */
            const char *no = usb_online() ? "USB connected" : audio_playing() ? "audio playing" :
                             access("/run/glassine-no-sleep", F_OK) == 0 ? "/run/glassine-no-sleep" : NULL;
            if (!no) go = "idle";
            else if (!idle_blocked) { idle_blocked = 1; stamp(); fprintf(stderr, "idle, no sleep: %s\n", no); }
        }
        if (!go && sleep_req) go = "menu";
        sleep_req = 0;
        if (go) {
            do_sleep(go);
            sleep_req = 0;
            struct input_event ev[32];
            for (int i = 0; i < n; i++) while (read(d[i].fd, ev, sizeof ev) > 0) {}
            awake_since = last_input = now_ms();
        }
        /* (un)plugged input devices (pen proxy restarts, keyboards) */
        if (now_ms() - last_rescan > 60000) {
            for (int i = 0; i < n; i++) close(d[i].fd);
            n = scan_inputs(d);
            last_rescan = now_ms();
        }
    }
}
