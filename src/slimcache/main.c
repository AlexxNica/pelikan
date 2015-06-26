#include <slimcache/setting.h>
#include <slimcache/stats.h>

#include <util/core.h>
#include <util/util.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sysexits.h>

static struct setting setting = {
    SETTING(OPTION_INIT)
};

#define PRINT_DEFAULT(_name, _type, _default, _description) \
    log_stdout("  %-31s ( default: %s )", #_name,  _default);

static const unsigned int nopt = OPTION_CARDINALITY(struct setting);

static void
show_usage(void)
{
    log_stdout(
            "Usage:" CRLF
            "  broadbill_slimcache [option|config]" CRLF
            );
    log_stdout(
            "Description:" CRLF
            "  broadbill_slimcache is one of the unified cache backends. " CRLF
            "  It uses cuckoo hashing to efficiently store small key/val " CRLF
            "  pairs. It speaks the memcached protocol and supports all " CRLF
            "  ASCII memcached commands (except for prepend/append). " CRLF
            CRLF
            "  The storage in slimcache is preallocated as a hash table " CRLF
            "  The maximum key/val size allowed has to be specified when " CRLF
            "  starting the service, and cannot be updated after launch." CRLF
            );
    log_stdout(
            "Options:" CRLF
            "  -h, --help        show this message" CRLF
            "  -v, --version     show version number" CRLF
            );
    log_stdout(
            "Example:" CRLF
            "  ./broadbill_slimcache ../template/slimcache.config" CRLF
            );
    log_stdout("Setting & Default Values:");
    SETTING(PRINT_DEFAULT)
}

static void
setup(void)
{
    struct addrinfo *ai;
    int ret;
    uint32_t max_conns;
    rstatus_t status;

    /* setup log first, so we log properly */
    ret = log_setup((int)setting.log_level.val.vuint,
            setting.log_name.val.vstr);
    if (ret < 0) {
        log_error("log setup failed");

        goto error;
    }

    metric_setup();

    array_setup((uint32_t)setting.array_nelem_delta.val.vuint);
    buf_setup((uint32_t)setting.buf_init_size.val.vuint, &glob_stats.buf_metrics);
    event_setup(&glob_stats.event_metrics);
    tcp_setup((int)setting.tcp_backlog.val.vuint, &glob_stats.tcp_metrics);

    time_setup();
    status = cuckoo_setup((size_t)setting.cuckoo_item_size.val.vuint,
            (uint32_t)setting.cuckoo_nitem.val.vuint,
            (uint32_t)setting.cuckoo_policy.val.vuint,
            setting.cuckoo_item_cas.val.vbool,
            &glob_stats.cuckoo_metrics);
    if (status != CC_OK) {
        log_error("cuckoo module setup failed");

        goto error;
    }
    procinfo_setup(&glob_stats.procinfo_metrics);
    request_setup(&glob_stats.request_metrics);
    codec_setup(&glob_stats.codec_metrics);
    process_setup(&glob_stats.process_metrics);

    /**
     * Here we don't create buf or conn pool because buf_sock will allocate
     * those objects and hold onto them as part of its create/allocate process.
     * So it will not use buf/conn pool resources and we have no use of them
     * outside the context of buf_sock.
     * Do not set those poolsizes in the config script, they will not be used.
     */
    buf_sock_pool_create((uint32_t)setting.buf_sock_poolsize.val.vuint);
    request_pool_create((uint32_t)setting.request_poolsize.val.vuint);

    /* set up core after static resources are ready */
    status = getaddr(&ai, setting.server_host.val.vstr,
            setting.server_port.val.vstr);
    if (status != CC_OK) {
        log_error("address invalid");

        goto error;
    }
    /**
     * Set up core with connection ring array being either the tcp poolsize or
     * the ring array default capacity if poolsize is unlimited
     */
    max_conns = setting.tcp_poolsize.val.vuint == 0 ?
        setting.ring_array_cap.val.vuint : setting.tcp_poolsize.val.vuint;
    status = core_setup(ai, max_conns, &glob_stats.server_metrics,
            &glob_stats.worker_metrics);
    freeaddrinfo(ai); /* freeing it before return/error to avoid memory leak */
    if (status != CC_OK) {
        log_crit("cannot start core event loop");

        goto error;
    }

    /* override signals that we want to customize */
    ret = signal_segv_stacktrace();
    if (ret < 0) {
        goto error;
    }

    ret = signal_ttin_logrotate();
    if (ret < 0) {
        goto error;
    }

    ret = signal_pipe_ignore();
    if (ret < 0) {
        goto error;
    }

    /* daemonize */
    if (setting.daemonize.val.vbool) {
        daemonize();
    }

    /* create pid file, call it after daemonize to have the correct pid */
    if (!option_empty(&setting.pid_filename)) {
        create_pidfile(setting.pid_filename.val.vstr);
    }

    return;

error:
    log_crit("setup failed");

    if (!option_empty(&setting.pid_filename)) {
        remove_pidfile(setting.pid_filename.val.vstr);
    }

    core_teardown();

    request_pool_destroy();
    buf_sock_pool_destroy();
    tcp_conn_pool_destroy();
    buf_pool_destroy();

    cuckoo_teardown();
    process_teardown();
    codec_teardown();
    request_teardown();
    procinfo_teardown();
    time_teardown();
    tcp_teardown();
    event_teardown();
    buf_teardown();
    array_teardown();
    metric_teardown();
    log_teardown();

    exit(EX_CONFIG);
}

int
main(int argc, char **argv)
{
    rstatus_t status;
    FILE *fp = NULL;

    if (argc > 2) {
        show_usage();
        exit(EX_USAGE);
    }

    if (argc == 1) {
        log_stderr("launching server with default values.");
    }

    if (argc == 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            show_usage();

            exit(EX_OK);
        }
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            show_version();

            exit(EX_OK);
        }
        fp = fopen(argv[1], "r");
        if (fp == NULL) {
            log_stderr("cannot open config: incorrect path or doesn't exist");

            exit(EX_DATAERR);
        }
    }

    status = option_load_default((struct option *)&setting, nopt);
    if (status != CC_OK) {
        log_stderr("fail to load default option values");

        exit(EX_CONFIG);
    }
    if (fp != NULL) {
        log_stderr("load config from %s", argv[1]);
        status = option_load_file(fp, (struct option *)&setting, nopt);
        fclose(fp);
    }
    if (status != CC_OK) {
        log_stderr("fail to load config");

        exit(EX_DATAERR);
    }
    option_printall((struct option *)&setting, nopt);

    setup();

    core_run();

    exit(EX_OK);
}