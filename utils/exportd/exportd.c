/*
 * Copyright (C) 2021 Red Hat <nfs@redhat.com>
 *
 * support/exportd/exportd.c
 *
 * Routines used to support NFSv4 exports
 *
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

#include "nfslib.h"
#include "conffile.h"
#include "exportfs.h"
#include "export.h"

extern void my_svc_run(void);

/* Number of mountd threads to start.   Default is 1 and
 * that's probably enough unless you need hundreds of
 * clients to be able to mount at once.  */
static int num_threads = 1;
/* Arbitrary limit on number of threads */
#define MAX_THREADS 64

int manage_gids;
int use_ipaddr = -1;

static struct option longopts[] =
{
	{ "foreground", 0, 0, 'F' },
	{ "debug", 1, 0, 'd' },
	{ "help", 0, 0, 'h' },
	{ "manage-gids", 0, 0, 'g' },
	{ "num-threads", 1, 0, 't' },
	{ "log-auth", 0, 0, 'l' },
	{ "cache-use-ipaddr", 0, 0, 'i' },
	{ "ttl", 0, 0, 'T' },
	{ NULL, 0, 0, 0 }
};
static char shortopts[] = "d:fghs:t:liT:";

/*
 * Signal handlers.
 */
inline static void set_signals(void);

inline void
cleanup_lockfiles (void)
{
	unlink(etab.lockfn);
}

static void
killer (int sig)
{
	if (num_threads > 1) {
		/* play Kronos and eat our children */
		kill(0, SIGTERM);
		cache_wait_for_workers("exportd");
	}
	cleanup_lockfiles();
	free_state_path_names(&etab);
	xlog (L_NOTICE, "Caught signal %d, exiting.", sig);

	exit(0);
}

static void
sig_hup (int UNUSED(sig))
{
	/* don't exit on SIGHUP */
	xlog (L_NOTICE, "Received SIGHUP... Ignoring.\n");
	return;
}

inline static void
set_signals(void)
{
	struct sigaction sa;

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, NULL);
	/* WARNING: the following works on Linux and SysV, but not BSD! */
	sigaction(SIGCHLD, &sa, NULL);

	sa.sa_handler = killer;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	sa.sa_handler = sig_hup;
	sigaction(SIGHUP, &sa, NULL);
}

static void
usage(const char *prog, int n)
{
	fprintf(stderr,
		"Usage: %s [-f|--foreground] [-h|--help] [-d kind|--debug kind]\n"
"	[-g|--manage-gids] [-l|--log-auth] [-i|--cache-use-ipaddr] [-T|--ttl ttl]\n"
"	[-s|--state-directory-path path]\n"
"	[-t num|--num-threads=num]\n", prog);
	exit(n);
}

inline static void
read_exportd_conf(char *progname, char **argv)
{
	char *s;
	int ttl;

	conf_init_file(NFS_CONFFILE);

	xlog_set_debug(progname);

	manage_gids = conf_get_bool("exportd", "manage-gids", manage_gids);
	num_threads = conf_get_num("exportd", "threads", num_threads);
	if (conf_get_bool("mountd", "cache-use-ipaddr", 0))
		use_ipaddr = 2;

	s = conf_get_str("exportd", "state-directory-path");
	if (s && !state_setup_basedir(argv[0], s))
		exit(1);

	ttl = conf_get_num("mountd", "ttl", default_ttl);
	if (ttl > 0)
		default_ttl = ttl;
}

int
main(int argc, char **argv)
{
	char *progname;
	int foreground = 0;
	int c;
	int ttl;

	/* Set the basename */
	if ((progname = strrchr(argv[0], '/')) != NULL)
		progname++;
	else
		progname = argv[0];

	/* Initialize logging. */
	xlog_open(progname);

	/* Read in config setting */
	read_exportd_conf(progname, argv);

	while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != EOF) {
		switch (c) {
		case 'd':
			xlog_sconfig(optarg, 1);
			break;
		case 'l':
			xlog_sconfig("auth", 1);
			break;
		case 'f':
			foreground++;
			break;
		case 'g':
			manage_gids = 1;
			break;
		case 'h':
			usage(progname, 0);
			break;
		case 'i':
			use_ipaddr = 2;
			break;
		case 'T':
			ttl = atoi(optarg);
			if (ttl <= 0) {
				fprintf(stderr, "%s: bad ttl number of seconds: %s\n",
					argv[0], optarg);
				usage(argv[0], 1);
			}
			default_ttl = ttl;
			break;
		case 's':
			if (!state_setup_basedir(argv[0], optarg))
				exit(1);
			break;
		case 't':
			num_threads = atoi (optarg);
			break;
		case '?':
		default:
			usage(progname, 1);
		}

	}

	if (!setup_state_path_names(progname, ETAB, ETABTMP, ETABLCK, &etab))
		return 1;

	if (!foreground)
		xlog_stderr(0);

	daemon_init(foreground);

	set_signals();
	daemon_ready();

	/* silently bounds check num_threads */
	if (foreground)
		num_threads = 1;
	else if (num_threads < 1)
		num_threads = 1;
	else if (num_threads > MAX_THREADS)
		num_threads = MAX_THREADS;

	/* Open cache channel files BEFORE forking so each upcall is
	 * only handled by one thread.  Kernel provides locking for both
	 * read and write.
	 */
	cache_open();

	if (cache_fork_workers(progname, num_threads) == 0) {
		/* We forked, waited, and now need to clean up */
		cleanup_lockfiles();
		free_state_path_names(&etab);
		xlog(L_NOTICE, "%s: no more workers, exiting\n", progname);
		exit(0);
	}

	v4clients_init();

	/* Process incoming upcalls */
	while (cache_process(NULL) >= 0)
		;

	xlog(L_ERROR, "%s: process loop terminated unexpectedly(%m). Exiting...\n",
		progname);

	free_state_path_names(&etab);
	exit(1);
}
