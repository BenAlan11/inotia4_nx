/* inotia_config.c -- config.ini, on the runtime's INI engine.
 *
 * The runtime reads [display] resolution, [performance] boost_cpu_when_loading
 * and the three [debug] switches itself (rt_config()); this file only has to
 * list them in the file's order and keep the port's own fields. Values are
 * written as the player sees them ("auto", "true"), never as indexes.
 *
 * MIT.
 */
#include <string.h>

#include "inotia.h"
#include "rt_cfg.h"
#include "rt_settings.h"

typedef struct {
  int swap_ab;
} InotiaConfig;

static InotiaConfig g_cfg;

static const CfgOpt k_opts[] = {
    CFG_ROW_RESOLUTION("auto", CFG_HELP_RESOLUTION),
    CFG_ROW_BOOST(CFG_HELP_BOOST, NULL),
    CFG_ROW_SWAP_AB("Swap A and B in the virtual-finger controls.", &g_cfg.swap_ab),
    CFG_ROW_GL_SELFTEST(NULL),
    CFG_ROW_BOOT_LOG(CFG_HELP_BOOT_LOG, NULL),
    /* The runtime's row defaults to false; this port is at the stage where the
     * engine's Java calls are the only way to see what it is missing, so it
     * starts on. Turn it off once the port is stable -- it is slow. */
    {"debug", "log_java_calls", "true", CFG_HELP_LOG_JNI, CFG_BOOL, NULL, NULL, 0, 0, 0, 0},
};

static const CfgSection k_sections[] = {
    {"display", "Rendering."},
    {"performance", "CPU clocks."},
    {"controls", "Controller input (the touchscreen is unaffected)."},
    {"debug", "Troubleshooting: slow, but the only way to report a problem."},
};

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = CFG_COUNT(k_opts),
    .sections = k_sections,
    .nsections = CFG_COUNT(k_sections),
    .version = 1,
};

void dcr_config_load(void) { rt_config_load(&k_table); }

int inotia_config_swap_ab(void) { return g_cfg.swap_ab; }
