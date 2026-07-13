/*
 * benchmark.c — Page-Cache Fairness Benchmark harness.
 *
 * Pairs a latency-sensitive victim (Tenant A / client1_steady) against a noisy
 * neighbor (Tenant B / client2_noisy) under cgroup v2 and measures A's p99
 * read latency, decomposing the spike into:
 *   Mechanism 1 — LRU eviction / refaults (clean scan)
 *   Mechanism 2 — eviction + dirty writeback contention (buffered writer)
 *
 * This file drives fio, sets up cgroups, samples pressure/dirty-page state,
 * and snapshots per-cgroup memory.stat before/after each phase.
 *
 * Platform: cgroups, PSI, memory.stat, and /proc/vmstat require Linux
 * (cgroup v2). macOS is build-only; the sampler/cgroup paths no-op there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_PHASES        16
#define MAX_CLIENTS       16
#define MAX_CGROUPS       16
#define MAX_STR           256
#define MAX_CMD           4096
#define SAMPLE_INTERVAL_S 1

/* ------------------------------------------------------------------ *
 * Configuration model
 * ------------------------------------------------------------------ */

typedef struct {
    bool  present;                    /* was this phase defined at all?      */
    char  pattern[MAX_STR];           /* randread/read/randwrite/write/randrw*/
    char  block_size[MAX_STR];        /* e.g. 4k, 1M                         */
    char  ioengine[MAX_STR];          /* libaio, sync, ...                   */
    int   rate_iops;                  /* 0 = unlimited                       */
    int   iodepth;
    int   numjobs;
    int   runtime;                    /* seconds (time-based, or ceiling)    */
    int   rwmixread;                  /* for randrw; -1 = unset              */

    /* read a fixed amount then stop (not time_based); "" = unset */
    char  io_size[MAX_STR];           /* e.g. 1G -> fio --io_size            */
    char  rate_bw[MAX_STR];           /* bandwidth cap e.g. 100m -> --rate   */

    /* victim access-distribution skew */
    char  random_distribution[MAX_STR]; /* "" or e.g. "zipf:1.2"             */

    /* flush cadence for checkpoint / WAL B variants */
    int   fdatasync;                  /* fdatasync every N ops; 0 = unset    */
    int   fsync;                      /* fsync every N ops; 0 = unset        */
} PhaseConfig;

typedef struct {
    char        name[MAX_STR];
    char        description[MAX_STR];
    char        file_size[MAX_STR];   /* e.g. 1G, 8G                         */
    int         num_phases;
    PhaseConfig phases[MAX_PHASES];
} ClientConfig;

typedef struct {
    char cgroup_name[MAX_STR];        /* section key -> cgroup leaf name     */
    char section[MAX_STR];            /* ini section name                    */
    char memory_max[MAX_STR];         /* "" if unset                         */
    char memory_low[MAX_STR];         /* "" if unset                         */
    char io_weight[MAX_STR];          /* "" if unset                         */
} CgroupConfig;

typedef struct {
    ClientConfig clients[MAX_CLIENTS];
    int          num_clients;
} Config;

/* ------------------------------------------------------------------ *
 * Global options
 * ------------------------------------------------------------------ */

typedef enum { MODE_BOTH, MODE_CACHED, MODE_DIRECT } CacheMode;

static struct {
    const char *config_path;
    const char *cgroup_config_path;
    const char *output_dir;
    CacheMode   mode;
    bool        verbose;
    bool        use_cgroups;
    bool        use_psi;
    bool        drop_once;   /* drop caches only before phase 0, not each phase */
} opt = {
    .config_path        = "fairness_configs.ini",
    .cgroup_config_path = NULL,
    .output_dir         = "benchmark_results",
    .mode               = MODE_BOTH,
    .verbose            = false,
    .use_cgroups        = true,
    .use_psi            = true,
    .drop_once          = false,
};

#define VLOG(...) do { if (opt.verbose) { fprintf(stderr, "[v] " __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define INFO(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

static const char CGROUP_ROOT[] = "/sys/fs/cgroup";

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

static char *trim(char *s) {
    /* trim leading and trailing whitespace */
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static bool is_linux(void) {
    /* check if the platform is Linux (required for cgroup v2) */
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

static void ensure_dir(const char *path) {
    /* create the directory if it doesn't exist */
    if (!path || !*path) return;

    char buf[MAX_CMD];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
        INFO("warning: mkdir path too long: %s", path);
        return;
    }

    size_t len = strlen(buf);
    if (len > 1 && buf[len - 1] == '/')
        buf[len - 1] = '\0';

    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            INFO("warning: mkdir %s: %s", buf, strerror(errno));
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        INFO("warning: mkdir %s: %s", buf, strerror(errno));
}

/* mkdir -p for a subdir under the results dir */
static void ensure_subdir(const char *base, const char *sub, char *out, size_t n) {
    /* builds base/sub and creates the directory into out if it doesn't exist */
    snprintf(out, n, "%s/%s", base, sub);
    ensure_dir(out);
}

/* Flatten cgroup path for result filenames (clients/a -> clients_a). */
static void cgroup_file_label(const char *cg, char *out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; cg[i] && j + 1 < n; i++) {
        char c = cg[i];
        if (c == '/') c = '_';
        out[j++] = c;
    }
    out[j] = '\0';
}

/* ------------------------------------------------------------------ *
 * INI parsing
 * ------------------------------------------------------------------ */

/* Route a "phase_N_key" or bare key onto the right PhaseConfig field. */
static void apply_phase_key(PhaseConfig *p, const char *key, const char *val) {
    if      (!strcmp(key, "pattern"))             snprintf(p->pattern, MAX_STR, "%s", val);
    else if (!strcmp(key, "block_size"))          snprintf(p->block_size, MAX_STR, "%s", val);
    else if (!strcmp(key, "ioengine"))            snprintf(p->ioengine, MAX_STR, "%s", val);
    else if (!strcmp(key, "rate_iops"))           p->rate_iops = atoi(val);
    else if (!strcmp(key, "iodepth"))             p->iodepth   = atoi(val);
    else if (!strcmp(key, "numjobs"))             p->numjobs   = atoi(val);
    else if (!strcmp(key, "runtime"))             p->runtime   = atoi(val);
    else if (!strcmp(key, "rwmixread"))           p->rwmixread = atoi(val);
    else if (!strcmp(key, "io_size"))             snprintf(p->io_size, MAX_STR, "%s", val);
    else if (!strcmp(key, "rate_bw"))             snprintf(p->rate_bw, MAX_STR, "%s", val);
    /* victim access-distribution skew */
    else if (!strcmp(key, "random_distribution")) snprintf(p->random_distribution, MAX_STR, "%s", val);
    /* flush cadence for checkpoint / WAL B variants */
    else if (!strcmp(key, "fdatasync"))           p->fdatasync = atoi(val);
    else if (!strcmp(key, "fsync"))               p->fsync     = atoi(val);
    else VLOG("unknown phase key '%s' (ignored)", key);
    p->present = true;
}

static void init_phase(PhaseConfig *p) {
    /* default values */
    memset(p, 0, sizeof(*p));
    p->rwmixread = -1;
    p->iodepth   = 1;
    p->numjobs   = 1;
    p->runtime   = 60;
    snprintf(p->pattern, MAX_STR, "randread");
    snprintf(p->block_size, MAX_STR, "4k");
    snprintf(p->ioengine, MAX_STR, "libaio");
}

static ClientConfig *find_or_add_client(Config *cfg, const char *name) {
    for (int i = 0; i < cfg->num_clients; i++)
    /* check if the client name already exists and return the client config */
    if (!strcmp(cfg->clients[i].name, name)) return &cfg->clients[i];
    if (cfg->num_clients >= MAX_CLIENTS) {
        INFO("error: too many client sections (max %d)", MAX_CLIENTS);
        exit(1);
    }
    /* add the new client config */
    ClientConfig *c = &cfg->clients[cfg->num_clients++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, MAX_STR, "%s", name);
    for (int i = 0; i < MAX_PHASES; i++) init_phase(&c->phases[i]);
    return c;
}

static bool parse_config(const char *path, Config *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) { INFO("error: cannot open config '%s': %s", path, strerror(errno)); return false; }

    memset(cfg, 0, sizeof(*cfg));
    char line[1024];
    ClientConfig *cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = '\0';
            cur = find_or_add_client(cfg, s + 1);
            continue;
        }
        if (!cur) continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        /* strip inline comments */
        char *sc = strchr(val, ';'); if (sc) { *sc = '\0'; val = trim(val); }

        if (!strcmp(key, "description")) { snprintf(cur->description, MAX_STR, "%s", val); continue; }
        if (!strcmp(key, "file_size"))   { snprintf(cur->file_size, MAX_STR, "%s", val); continue; }

        if (!strncmp(key, "phase_", 6)) {
            int idx = atoi(key + 6);
            char *us = strchr(key + 6, '_');
            if (!us || idx < 0 || idx >= MAX_PHASES) { VLOG("bad phase key '%s'", key); continue; }
            apply_phase_key(&cur->phases[idx], us + 1, val);
            if (idx + 1 > cur->num_phases) cur->num_phases = idx + 1;
        } else {
            /* non-phased single-phase shorthand -> phase 0 */
            apply_phase_key(&cur->phases[0], key, val);
            if (cur->num_phases < 1) cur->num_phases = 1;
        }
    }
    fclose(f);
    return true;
}

static ClientConfig *find_client(Config *cfg, const char *name) {
    for (int i = 0; i < cfg->num_clients; i++)
        if (!strcmp(cfg->clients[i].name, name)) return &cfg->clients[i];
    return NULL;
}

/* ------------------------------------------------------------------ *
 * cgroup config parsing (separate ini)
 * ------------------------------------------------------------------ */

typedef struct { CgroupConfig groups[MAX_CGROUPS]; int n; } CgroupSet;

static bool parse_cgroup_config(const char *path, CgroupSet *set) {
    FILE *f = fopen(path, "r");
    if (!f) { INFO("error: cannot open cgroup config '%s': %s", path, strerror(errno)); return false; }

    memset(set, 0, sizeof(*set));
    char line[1024];
    CgroupConfig *cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *end = strchr(s, ']'); if (!end) continue; *end = '\0';
            if (set->n >= MAX_CGROUPS) { INFO("too many cgroups"); break; }
            cur = &set->groups[set->n++];
            memset(cur, 0, sizeof(*cur));
            snprintf(cur->section, MAX_STR, "%s", s + 1);
            snprintf(cur->cgroup_name, MAX_STR, "%s", s + 1); /* default = section */
            continue;
        }
        if (!cur) continue;
        char *eq = strchr(s, '='); if (!eq) continue; *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        char *sc = strchr(val, ';'); if (sc) { *sc = '\0'; val = trim(val); }
        if      (!strcmp(key, "cgroup_name")) snprintf(cur->cgroup_name, MAX_STR, "%s", val);
        else if (!strcmp(key, "memory.max"))  snprintf(cur->memory_max, MAX_STR, "%s", val);
        else if (!strcmp(key, "memory.low"))  snprintf(cur->memory_low, MAX_STR, "%s", val);
        else if (!strcmp(key, "io.weight"))   snprintf(cur->io_weight, MAX_STR, "%s", val);
    }
    fclose(f);
    return true;
}

static void write_cgroup_file(const char *cg_path, const char *file, const char *val) {
    if (!val || !*val) return;
    char p[MAX_CMD];
    snprintf(p, sizeof(p), "%s/%s", cg_path, file);
    FILE *f = fopen(p, "w");
    if (!f) { INFO("warning: open %s: %s", p, strerror(errno)); return; }
    fprintf(f, "%s\n", val);
    if (fclose(f) != 0) INFO("warning: write %s: %s", p, strerror(errno));
    VLOG("set %s = %s", p, val);
}

/* Enable the memory + io controllers in a cgroup's subtree_control so that
 * its children expose memory.max / io.weight etc. Best-effort: on many hosts
 * systemd has already delegated these, and a redundant write is harmless. */
static void enable_controllers(const char *cg_path) {
    char p[MAX_CMD];
    snprintf(p, sizeof(p), "%s/cgroup.subtree_control", cg_path);
    FILE *f = fopen(p, "w");
    if (!f) { VLOG("subtree_control %s not writable: %s", p, strerror(errno)); return; }
    fprintf(f, "+memory +io\n");
    if (fclose(f) != 0) VLOG("subtree_control %s write: %s", p, strerror(errno));
    else VLOG("enabled +memory +io in %s", p);
}

/* mkdir -p for a cgroup path under CGROUP_ROOT, enabling controllers on each
 * intermediate directory top-down (a controller can only be delegated by a
 * parent that already has it). `rel` is the path relative to CGROUP_ROOT. */
static void mkdir_p_cgroup(const char *rel) {
    /* root first: delegate memory+io down into our tree */
    enable_controllers(CGROUP_ROOT);

    char acc[MAX_CMD];
    snprintf(acc, sizeof(acc), "%s", CGROUP_ROOT);

    char work[MAX_STR];
    snprintf(work, sizeof(work), "%s", rel);
    char *save = NULL;
    for (char *tok = strtok_r(work, "/", &save); tok; ) {
        size_t len = strlen(acc);
        snprintf(acc + len, sizeof(acc) - len, "/%s", tok);
        if (mkdir(acc, 0755) != 0 && errno != EEXIST)
            INFO("warning: mkdir cgroup %s: %s", acc, strerror(errno));
        char *next = strtok_r(NULL, "/", &save);
        /* enable controllers on every non-leaf dir so children get the files */
        if (next) enable_controllers(acc);
        tok = next;
    }
}

/* Create the cgroups (with intermediates) and apply limits. */
static void setup_cgroups(const CgroupSet *set) {
    if (!is_linux()) { INFO("note: not Linux — skipping cgroup setup"); return; }
    for (int i = 0; i < set->n; i++) {
        const CgroupConfig *g = &set->groups[i];
        mkdir_p_cgroup(g->cgroup_name);
        char path[MAX_CMD];
        snprintf(path, sizeof(path), "%s/%s", CGROUP_ROOT, g->cgroup_name);
        write_cgroup_file(path, "memory.max", g->memory_max);
        write_cgroup_file(path, "memory.low", g->memory_low);
        write_cgroup_file(path, "io.weight",  g->io_weight);
    }
}

static const CgroupConfig *cgroup_for_client(const CgroupSet *set, const char *client) {
    for (int i = 0; i < set->n; i++)
        if (!strcmp(set->groups[i].section, client)) return &set->groups[i];
    return NULL;
}

/* ------------------------------------------------------------------ *
 * memory.stat snapshots + workingset_refault_file delta
 * ------------------------------------------------------------------ */

static long read_memstat_field(const char *cgroup_name, const char *field) {
    if (!is_linux()) return -1;
    char p[MAX_CMD];
    snprintf(p, sizeof(p), "%s/%s/memory.stat", CGROUP_ROOT, cgroup_name);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char key[128]; long val;
    long found = -1;
    while (fscanf(f, "%127s %ld", key, &val) == 2)
        if (!strcmp(key, field)) { found = val; break; }
    fclose(f);
    return found;
}

/* Read a single-integer cgroup control file, e.g. memory.current. */
static long read_cgroup_scalar(const char *cgroup_name, const char *filename) {
    if (!is_linux()) return -1;
    char p[MAX_CMD];
    snprintf(p, sizeof(p), "%s/%s/%s", CGROUP_ROOT, cgroup_name, filename);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    long val;
    long found = (fscanf(f, "%ld", &val) == 1) ? val : -1;
    fclose(f);
    return found;
}

/* Append a before/after memstat row to memstat/<client>_<mode>.csv */
static void record_memstat(const char *cgroup_name, const char *client,
                           const char *mode, int phase, const char *when,
                           long prev_refault, long *out_refault,
                           long prev_memcur, long *out_memcur) {
    long refault = read_memstat_field(cgroup_name, "workingset_refault_file");
    if (out_refault) *out_refault = refault;
    long memcur = read_cgroup_scalar(cgroup_name, "memory.current");
    if (out_memcur) *out_memcur = memcur;

    char dir[MAX_CMD], path[MAX_CMD];
    ensure_subdir(opt.output_dir, "memstat", dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s_%s.csv", dir, client, mode);

    bool exists = access(path, F_OK) == 0;
    FILE *f = fopen(path, "a");
    if (!f) { INFO("warning: memstat csv %s: %s", path, strerror(errno)); return; }
    if (!exists)
        fprintf(f, "phase,when,wall_time,workingset_refault_file,"
                   "workingset_refault_file_delta,memory_current_bytes,"
                   "memory_current_bytes_delta\n");
    long delta = (when && !strcmp(when, "after") && prev_refault >= 0 && refault >= 0)
                     ? refault - prev_refault : -1;
    long memdelta = (when && !strcmp(when, "after") && prev_memcur >= 0 && memcur >= 0)
                     ? memcur - prev_memcur : -1;
    fprintf(f, "%d,%s,%ld,%ld,%ld,%ld,%ld\n",
            phase, when, (long)time(NULL), refault, delta, memcur, memdelta);
    fclose(f);
}

/* ------------------------------------------------------------------ *
 * Telemetry sampler: /proc/vmstat + per-cgroup file_dirty,
 * plus per-cgroup PSI (memory.pressure / io.pressure) time series.
 * ------------------------------------------------------------------ */

static volatile sig_atomic_t sampler_stop = 0;
static void sampler_sig(int sig) { (void)sig; sampler_stop = 1; }

/* System-wide memory counters
 * nr_dirty: number of pages changes but not yet flushed to disk
 * nr_writeback: number of pages actively flushed to disk
 * pgpgin: number of pages read from disk
 * pgscan_kswapd: number of pages scanned in the LRU list for eviction
 */
static long read_vmstat_field(const char *field) {
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) return -1;
    char key[128]; long val, found = -1;
    while (fscanf(f, "%127s %ld", key, &val) == 2)
        if (!strcmp(key, field)) { found = val; break; }
    fclose(f);
    return found;
}

/* Parse "some avg10=.. avg60=.. avg300=.. total=N" from a PSI file and return
 * the `some` cumulative total (microseconds stalled). -1 on failure. */
static long read_psi_total(const char *cgroup_name, const char *resource) {
    char p[MAX_CMD];
    snprintf(p, sizeof(p), "%s/%s/%s.pressure", CGROUP_ROOT, cgroup_name, resource);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[512];
    long total = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "some", 4) == 0) {
            char *t = strstr(line, "total=");
            if (t) total = atol(t + 6);
            break;
        }
    }
    fclose(f);
    return total;
}

/*
 * Runs in a forked child. Every SAMPLE_INTERVAL_S seconds appends:
 *   dirty/vmstat_<mode>.csv       : ts, nr_dirty, nr_writeback, pgpgin, pgscan_kswapd
 *   dirty/<cg>_<mode>_dirty.csv   : ts, file_dirty, file_writeback (per cgroup;
 *                                    cgroup path slashes -> underscores in name)
 *   psi/<cg>_<mode>.csv           : ts, mem_some_total_us, io_some_total_us
 * Exits on SIGTERM/SIGINT.
 */
static void run_sampler(const char *mode, char cgroup_names[][MAX_STR], int n_cg) {
    signal(SIGTERM, sampler_sig);
    signal(SIGINT,  sampler_sig);

    char psi_dir[MAX_CMD], dirty_dir[MAX_CMD];
    ensure_subdir(opt.output_dir, "psi",   psi_dir,   sizeof(psi_dir));
    ensure_subdir(opt.output_dir, "dirty", dirty_dir, sizeof(dirty_dir));

    char vmstat_path[MAX_CMD];
    snprintf(vmstat_path, sizeof(vmstat_path), "%s/vmstat_%s.csv", dirty_dir, mode);
    FILE *vf = fopen(vmstat_path, "w");
    if (vf) fprintf(vf, "ts,nr_dirty,nr_writeback,pgpgin,pgscan_kswapd\n");

    /* one file_dirty CSV + one PSI CSV per cgroup */
    FILE *cgf[MAX_CGROUPS] = {0};
    FILE *psf[MAX_CGROUPS] = {0};
    for (int i = 0; i < n_cg && i < MAX_CGROUPS; i++) {
        char label[MAX_STR];
        cgroup_file_label(cgroup_names[i], label, sizeof(label));
        char p[MAX_CMD];
        snprintf(p, sizeof(p), "%s/%s_%s_dirty.csv", dirty_dir, label, mode);
        cgf[i] = fopen(p, "w");
        if (cgf[i]) fprintf(cgf[i], "ts,file_dirty,file_writeback\n");
        else VLOG("could not open dirty csv %s: %s", p, strerror(errno));

        if (opt.use_psi) {
            snprintf(p, sizeof(p), "%s/%s_%s.csv", psi_dir, label, mode);
            psf[i] = fopen(p, "w");
            if (psf[i]) fprintf(psf[i], "ts,mem_some_total_us,io_some_total_us\n");
            else VLOG("could not open psi csv %s: %s", p, strerror(errno));
        }
    }

    long ts = 0;
    while (!sampler_stop) {
        long nd = read_vmstat_field("nr_dirty");
        long nw = read_vmstat_field("nr_writeback");
        long pi = read_vmstat_field("pgpgin");
        long pk = read_vmstat_field("pgscan_kswapd");
        if (vf) { fprintf(vf, "%ld,%ld,%ld,%ld,%ld\n", ts, nd, nw, pi, pk); fflush(vf); }

        for (int i = 0; i < n_cg && i < MAX_CGROUPS; i++) {
            if (cgf[i]) {
                long fd = read_memstat_field(cgroup_names[i], "file_dirty");
                long fw = read_memstat_field(cgroup_names[i], "file_writeback");
                fprintf(cgf[i], "%ld,%ld,%ld\n", ts, fd, fw);
                fflush(cgf[i]);
            }
            if (psf[i]) {
                long mem = read_psi_total(cgroup_names[i], "memory");
                long io  = read_psi_total(cgroup_names[i], "io");
                fprintf(psf[i], "%ld,%ld,%ld\n", ts, mem, io);
                fflush(psf[i]);
            }
        }
        sleep(SAMPLE_INTERVAL_S);
        ts += SAMPLE_INTERVAL_S;
    }
    if (vf) fclose(vf);
    for (int i = 0; i < n_cg; i++) { if (cgf[i]) fclose(cgf[i]); if (psf[i]) fclose(psf[i]); }
    _exit(0);
}

static pid_t start_sampler(const char *mode, char cgroup_names[][MAX_STR], int n_cg) {
    if (!is_linux()) { VLOG("sampler skipped (not Linux)"); return -1; }
    pid_t pid = fork();
    if (pid == 0) { run_sampler(mode, cgroup_names, n_cg); _exit(0); }
    if (pid < 0) INFO("warning: fork sampler: %s", strerror(errno));
    return pid;
}

static void stop_sampler(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ *
 * iostat -dx logger (device read/write latency + queue depth)
 * ------------------------------------------------------------------ */

static pid_t start_iostat(const char *mode) {
    if (!is_linux()) { VLOG("iostat skipped (not Linux)"); return -1; }
    char dir[MAX_CMD], path[MAX_CMD], cmd[MAX_CMD];
    ensure_subdir(opt.output_dir, "iostat", dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/run_%s.iostat", dir, mode);

    /* Record iostat's own start time so analysis can map each 1s report
     * (report 0 is the since-boot average; reports 1..N are the 1s
     * intervals) back to a wall-clock time and line it up against the
     * per-phase before/after wall_time in memstat/*.csv. */
    char ts_path[MAX_CMD];
    snprintf(ts_path, sizeof(ts_path), "%s/run_%s.start_ts", dir, mode);
    FILE *tf = fopen(ts_path, "w");
    if (tf) { fprintf(tf, "%ld\n", (long)time(NULL)); fclose(tf); }

    /* iostat -dx 1 : extended device stats every second, redirected to file */
    snprintf(cmd, sizeof(cmd), "exec iostat -dx 1 > '%s' 2>/dev/null", path);

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) INFO("warning: fork iostat: %s", strerror(errno));
    else VLOG("iostat -dx 1 -> %s", path);
    return pid;
}

static void stop_iostat(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ *
 * fio command builder
 * ------------------------------------------------------------------ */

/* Append "arg" to cmd buffer, guarding against overflow. */
static void append(char *cmd, size_t cap, const char *fmt, ...) {
    size_t len = strlen(cmd);
    va_list ap; va_start(ap, fmt);
    vsnprintf(cmd + len, cap - len, fmt, ap);
    va_end(ap);
}

/*
 * Build the fio command line for one client phase.
 *   json_out : path to write --output (fio JSON) for this phase
 *   cached   : true -> buffered (direct=0); false -> direct=1 (no cache)
 *   cg_path  : cgroup dir to place fio in via --cgroup style; may be NULL
 */
static void build_fio_cmd(char *cmd, size_t cap, const ClientConfig *c,
                          const PhaseConfig *p, const char *test_file,
                          const char *json_out, bool cached) {
    cmd[0] = '\0';
    append(cmd, cap, "fio");
    append(cmd, cap, " --name=%s_p%d", c->name, (int)(p - c->phases));
    append(cmd, cap, " --filename=%s", test_file);
    append(cmd, cap, " --size=%s", c->file_size);
    append(cmd, cap, " --rw=%s", p->pattern);
    append(cmd, cap, " --bs=%s", p->block_size);
    append(cmd, cap, " --ioengine=%s", p->ioengine);
    append(cmd, cap, " --iodepth=%d", p->iodepth);
    append(cmd, cap, " --numjobs=%d", p->numjobs);
    if (p->io_size[0]) {
        /* read a fixed amount once then stop; runtime (if set) is a ceiling */
        append(cmd, cap, " --io_size=%s", p->io_size);
        if (p->runtime > 0) append(cmd, cap, " --runtime=%d", p->runtime);
    } else {
        append(cmd, cap, " --runtime=%d --time_based", p->runtime);
    }
    append(cmd, cap, " --direct=%d", cached ? 0 : 1);
    append(cmd, cap, " --group_reporting");

    if (p->rate_bw[0])    append(cmd, cap, " --rate=%s", p->rate_bw);
    if (p->rate_iops > 0) append(cmd, cap, " --rate_iops=%d", p->rate_iops);
    if (p->rwmixread >= 0 && strstr(p->pattern, "rw"))
        append(cmd, cap, " --rwmixread=%d", p->rwmixread);

    /* [TODO-1] victim access-distribution skew (zipf/pareto/normal) */
    if (p->random_distribution[0])
        append(cmd, cap, " --random_distribution=%s", p->random_distribution);

    /* [TODO-2] flush cadence — checkpoint / WAL dirtying.
     * fdatasync=N issues fdatasync() every N write ops; fsync=N likewise. */
    if (p->fdatasync > 0) append(cmd, cap, " --fdatasync=%d", p->fdatasync);
    if (p->fsync     > 0) append(cmd, cap, " --fsync=%d",     p->fsync);

    /* JSON+ gives clat_ns.percentile including p99/p999 */
    append(cmd, cap, " --output-format=json+ --output=%s", json_out);
}

/* ------------------------------------------------------------------ *
 * Test-file management
 * ------------------------------------------------------------------ */

static void test_file_for(const ClientConfig *c, char *out, size_t n) {
    snprintf(out, n, "test_file_%s_%s", c->name, c->file_size);
}

/* Pre-create a client's test file with fio --create_only if it is missing or
 * the wrong size. Doing this up front keeps the timed phases from paying file
 * layout cost and keeps refault accounting clean. */
static void ensure_test_file(const ClientConfig *c) {
    char file[MAX_STR];
    test_file_for(c, file, sizeof(file));
    if (access(file, F_OK) == 0) { VLOG("test file %s exists", file); return; }
    INFO("creating test file %s (%s)...", file, c->file_size);
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
             "fio --name=create_%s --filename=%s --size=%s --rw=write --bs=1M "
             "--create_only=1 --ioengine=libaio --direct=1 > /dev/null 2>&1",
             c->name, file, c->file_size);
    if (system(cmd) != 0)
        INFO("warning: could not pre-create %s (fio installed?)", file);
}

/* Drop the page cache before a cached-mode phase so refaults are meaningful. */
static void drop_caches(void) {
    if (!is_linux()) return;
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (!f) { INFO("warning: drop_caches (need sudo?): %s", strerror(errno)); return; }
    fprintf(f, "3\n");
    fclose(f);
    VLOG("dropped page cache");
}

/* ------------------------------------------------------------------ *
 * Running a single client (all its phases) inside its cgroup
 * ------------------------------------------------------------------ */

/* Wrap the fio command so the spawned shell joins the cgroup before exec. */
static void wrap_with_cgroup(char *cmd, size_t cap, const char *cg_path) {
    if (!cg_path || !is_linux()) return;
    char buf[MAX_CMD];
    /* echo our shell pid into cgroup.procs, then exec fio in-place */
    snprintf(buf, sizeof(buf),
             "echo $$ > %s/cgroup.procs 2>/dev/null; exec %s", cg_path, cmd);
    snprintf(cmd, cap, "%s", buf);
}

/* Fork+exec a client's phase in the background; returns pid. */
static pid_t spawn_client_phase(const ClientConfig *c, const PhaseConfig *p,
                                const char *mode_str, bool cached,
                                const char *cg_path) {
    char test_file[MAX_STR], json_out[MAX_CMD], cmd[MAX_CMD];
    test_file_for(c, test_file, sizeof(test_file));

    char json_dir[MAX_CMD];
    snprintf(json_dir, sizeof(json_dir), "%s", opt.output_dir);
    ensure_dir(json_dir);
    snprintf(json_out, sizeof(json_out), "%s/%s_%s_p%d.json",
             opt.output_dir, c->name, mode_str, (int)(p - c->phases));

    build_fio_cmd(cmd, sizeof(cmd), c, p, test_file, json_out, cached);
    wrap_with_cgroup(cmd, sizeof(cmd), cg_path);
    VLOG("exec: %s", cmd);

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* ------------------------------------------------------------------ *
 * Run orchestration
 * ------------------------------------------------------------------ */

static const char *cgpath_or_null(const CgroupSet *set, const char *client, char *buf, size_t n) {
    if (!opt.use_cgroups || !set) return NULL;
    const CgroupConfig *g = cgroup_for_client(set, client);
    if (!g) return NULL;
    snprintf(buf, n, "%s/%s", CGROUP_ROOT, g->cgroup_name);
    return buf;
}

/* Run a set of clients concurrently for one cache mode, phase by phase. */
static void run_clients(Config *cfg, const CgroupSet *cgset,
                        const char **client_names, int n_clients,
                        bool cached) {
    const char *mode_str = cached ? "cached" : "direct";
    INFO("=== Running %d client(s) in %s mode ===", n_clients, mode_str);

    /* collect cgroup names for the sampler */
    char cg_names[MAX_CGROUPS][MAX_STR];
    int n_cg = 0;
    if (opt.use_cgroups && cgset) {
        for (int i = 0; i < n_clients && n_cg < MAX_CGROUPS; i++) {
            const CgroupConfig *g = cgroup_for_client(cgset, client_names[i]);
            if (g) snprintf(cg_names[n_cg++], MAX_STR, "%s", g->cgroup_name);
        }
    }

    /* max phase count across clients */
    int max_phases = 1;
    for (int i = 0; i < n_clients; i++) {
        ClientConfig *c = find_client(cfg, client_names[i]);
        if (c && c->num_phases > max_phases) max_phases = c->num_phases;
    }

    /* pre-create every client's test file (fio --create_only) */
    for (int i = 0; i < n_clients; i++) {
        ClientConfig *c = find_client(cfg, client_names[i]);
        if (c) ensure_test_file(c);
    }

    pid_t sampler = start_sampler(mode_str, cg_names, n_cg);
    pid_t iostat  = start_iostat(mode_str);

    for (int ph = 0; ph < max_phases; ph++) {
        INFO("--- phase %d ---", ph);
        /* With --drop-once, only phase 0 starts cold; later phases keep the
         * cache so the clients compete for it across phases. */
        if (cached && (!opt.drop_once || ph == 0)) drop_caches();

        /* before-snapshot */
        long prev_refault[MAX_CLIENTS];
        long prev_memcur[MAX_CLIENTS];
        for (int i = 0; i < n_clients; i++) {
            prev_refault[i] = -1;
            prev_memcur[i] = -1;
            const CgroupConfig *g = cgset ? cgroup_for_client(cgset, client_names[i]) : NULL;
            if (g) record_memstat(g->cgroup_name, client_names[i], mode_str, ph, "before",
                                  -1, &prev_refault[i], -1, &prev_memcur[i]);
        }

        /* spawn all clients' phase-ph concurrently */
        pid_t pids[MAX_CLIENTS]; int npids = 0;
        for (int i = 0; i < n_clients; i++) {
            ClientConfig *c = find_client(cfg, client_names[i]);
            if (!c || ph >= c->num_phases || !c->phases[ph].present) { pids[i] = -1; continue; }
            char cgbuf[MAX_CMD];
            const char *cg = cgpath_or_null(cgset, client_names[i], cgbuf, sizeof(cgbuf));
            pids[i] = spawn_client_phase(c, &c->phases[ph], mode_str, cached, cg);
            if (pids[i] > 0) npids++;
        }

        /* wait for all */
        for (int i = 0; i < n_clients; i++)
            if (pids[i] > 0) waitpid(pids[i], NULL, 0);
        (void)npids;

        /* after-snapshot -> computes workingset_refault_file_delta */
        for (int i = 0; i < n_clients; i++) {
            const CgroupConfig *g = cgset ? cgroup_for_client(cgset, client_names[i]) : NULL;
            if (g) record_memstat(g->cgroup_name, client_names[i], mode_str, ph, "after",
                                  prev_refault[i], NULL, prev_memcur[i], NULL);
        }
    }

    stop_iostat(iostat);
    stop_sampler(sampler);

    /* append a one-line-per-client run summary */
    char sumpath[MAX_CMD];
    snprintf(sumpath, sizeof(sumpath), "%s/summary.txt", opt.output_dir);
    FILE *sf = fopen(sumpath, "a");
    if (sf) {
        fprintf(sf, "[%s mode] clients:", mode_str);
        for (int i = 0; i < n_clients; i++) fprintf(sf, " %s", client_names[i]);
        fprintf(sf, "  phases=%d  cgroups=%s  psi=%s\n",
                max_phases, opt.use_cgroups ? "on" : "off", opt.use_psi ? "on" : "off");
        fclose(sf);
    }
    INFO("=== %s mode complete ===", mode_str);
}

static void run_for_modes(Config *cfg, const CgroupSet *cgset,
                          const char **names, int n) {
    if (opt.mode == MODE_BOTH || opt.mode == MODE_CACHED) run_clients(cfg, cgset, names, n, true);
    if (opt.mode == MODE_BOTH || opt.mode == MODE_DIRECT) run_clients(cfg, cgset, names, n, false);
}

/* ------------------------------------------------------------------ *
 * CLI
 * ------------------------------------------------------------------ */

static void usage(const char *argv0) {
    printf(
"Usage: %s [options] <workload | dual | all>\n"
"\n"
"Modes:\n"
"  <workload>   Run a single client section from the config (baseline/characterization)\n"
"  dual         Run client1_steady (A) + client2_noisy (B) concurrently (primary experiment)\n"
"  all          Run every client section defined in the config\n"
"\n"
"Options:\n"
"  -c, --config PATH        Workload config ini (default: fairness_configs.ini)\n"
"      --cgroup-config PATH  cgroup layout ini (enables cgroup setup)\n"
"      --no-cgroup          Disable cgroup setup (shared page-cache pool)\n"
"      --no-psi             Disable PSI (memory/io.pressure) sampling\n"
"      --drop-once          Drop the page cache only before phase 0 (default: every phase);\n"
"                           lets tenants compete for cache built up across phases\n"
"  -m, --mode MODE          cached | direct | both (default: both)\n"
"  -o, --output DIR         Results directory (default: benchmark_results)\n"
"  -v, --verbose            Verbose logging\n"
"  -h, --help               This help\n"
"\n"
"Notes:\n"
"  * Writer-B (Mechanism 2) pairings MUST run with -m cached; direct=1 disables\n"
"    dirty writeback and Mechanism 2 vanishes.\n"
"  * cgroups / PSI / memory.stat / /proc/vmstat require Linux (cgroup v2).\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *workload = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) opt.verbose = true;
        else if (!strcmp(a, "--no-cgroup")) opt.use_cgroups = false;
        else if (!strcmp(a, "--no-psi")) opt.use_psi = false;
        else if (!strcmp(a, "--drop-once")) opt.drop_once = true;
        else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i + 1 < argc) opt.config_path = argv[++i];
        else if (!strcmp(a, "--cgroup-config") && i + 1 < argc) opt.cgroup_config_path = argv[++i];
        else if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && i + 1 < argc) opt.output_dir = argv[++i];
        else if ((!strcmp(a, "-m") || !strcmp(a, "--mode")) && i + 1 < argc) {
            const char *m = argv[++i];
            if      (!strcmp(m, "cached")) opt.mode = MODE_CACHED;
            else if (!strcmp(m, "direct")) opt.mode = MODE_DIRECT;
            else if (!strcmp(m, "both"))   opt.mode = MODE_BOTH;
            else { INFO("error: bad mode '%s'", m); return 2; }
        }
        else if (a[0] == '-') { INFO("error: unknown option '%s'", a); usage(argv[0]); return 2; }
        else workload = a;
    }

    if (!workload) { usage(argv[0]); return 2; }

    Config cfg;
    if (!parse_config(opt.config_path, &cfg)) return 1;
    INFO("loaded %d client section(s) from %s", cfg.num_clients, opt.config_path);

    ensure_dir(opt.output_dir);

    CgroupSet cgset; bool have_cgset = false;
    if (opt.use_cgroups && opt.cgroup_config_path) {
        if (!parse_cgroup_config(opt.cgroup_config_path, &cgset)) return 1;
        setup_cgroups(&cgset);
        have_cgset = true;
    }
    const CgroupSet *cgptr = have_cgset ? &cgset : NULL;

    if (!strcmp(workload, "dual")) {
        const char *names[] = { "client1_steady", "client2_noisy" };
        for (int i = 0; i < 2; i++)
            if (!find_client(&cfg, names[i])) { INFO("error: dual needs section [%s]", names[i]); return 1; }
        run_for_modes(&cfg, cgptr, names, 2);
    } else if (!strcmp(workload, "all")) {
        const char *names[MAX_CLIENTS];
        for (int i = 0; i < cfg.num_clients; i++) names[i] = cfg.clients[i].name;
        for (int i = 0; i < cfg.num_clients; i++) {
            const char *one[] = { names[i] };
            run_for_modes(&cfg, cgptr, one, 1);
        }
    } else {
        if (!find_client(&cfg, workload)) { INFO("error: no section [%s] in config", workload); return 1; }
        const char *one[] = { workload };
        run_for_modes(&cfg, cgptr, one, 1);
    }

    INFO("done. results in %s/", opt.output_dir);
    return 0;
}
