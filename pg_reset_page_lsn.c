#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "access/xlog_internal.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "getopt_long.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"

static const char *progname;
static XLogRecPtr lsn = InvalidXLogRecPtr;
static bool data_checksums = false;
static bool do_sync = true;
static bool showprogress = false;

static int64 files = 0;
static int64 blocks = 0;

/* database cluster is specified as a target directory to scan? */
static bool	pgdata_mode = false;

static XLogRecPtr pg_lsn_in_internal(const char *str, bool *have_error);
static int64 scan_directory(const char *basedir, const char *subdir,
							bool sizeonly);
static void scan_file(const char *fn, BlockNumber segmentno);
static int64 scan_pgdata(const char *basedir, bool sizeonly);
static void validate_datadir(const char *datadir);
static bool skipfile(const char *fn);
static void progress_report(bool finished);

/*
 * Progress status information.
 */
int64		total_size = 0;
int64		current_size = 0;
static pg_time_t last_progress_report = 0;

static void
usage(void)
{
	printf(_("%s resets LSN of every pages in relation files.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -D, --directory=DIR      database directory to find relation files\n"));
	printf(_("  -l, --lsn=LSN            reset LSN in relation pages\n"));
	printf(_("  -k, --data-checksums     update checksums in relation pages\n"));
	printf(_("  -N, --no-sync            do not wait for changes to be written safely to disk\n"));
	printf(_("  -P, --progress           show progress information\n"));
	printf(_("  -V, --version            output version information, then exit\n"));
	printf(_("  -?, --help               show this help, then exit\n"));
}

/*
 * Copy-and-paste from src/backend/utils/adt/pg_lsn.c
 */
static XLogRecPtr
pg_lsn_in_internal(const char *str, bool *have_error)
{
#define MAXPG_LSNCOMPONENT	8
	int			len1,
				len2;
	uint32		id,
				off;
	XLogRecPtr	result;

	Assert(have_error != NULL);
	*have_error = false;

	/* Sanity check input format. */
	len1 = strspn(str, "0123456789abcdefABCDEF");
	if (len1 < 1 || len1 > MAXPG_LSNCOMPONENT || str[len1] != '/')
	{
		*have_error = true;
		return InvalidXLogRecPtr;
	}
	len2 = strspn(str + len1 + 1, "0123456789abcdefABCDEF");
	if (len2 < 1 || len2 > MAXPG_LSNCOMPONENT || str[len1 + 1 + len2] != '\0')
	{
		*have_error = true;
		return InvalidXLogRecPtr;
	}

	/* Decode result. */
	id = (uint32) strtoul(str, NULL, 16);
	off = (uint32) strtoul(str + len1 + 1, NULL, 16);
	result = ((uint64) id << 32) | off;

	return result;
}

static int64
scan_directory(const char *basedir, const char *subdir, bool sizeonly)
{
	/* Copy and paste from src/include/storage/fd.h */
#define PG_TEMP_FILES_DIR "pgsql_tmp"
#define PG_TEMP_FILE_PREFIX "pgsql_tmp"
	int64		dirsize = 0;
	char		path[MAXPGPATH];
	bool		path_is_symlink = false;
	DIR		   *dir;
	struct dirent *de;
	struct stat st;

	if (subdir == NULL)
		strncpy(path, basedir, sizeof(path));
	else
		snprintf(path, sizeof(path), "%s/%s", basedir, subdir);

	/* Check if the current path indicates a symlink or not */
#ifndef WIN32
	if (lstat(path, &st) < 0)
	{
		pg_log_error("could not stat directory \"%s\": %m", path);
		exit(1);
	}
	else if (S_ISLNK(st.st_mode))
		path_is_symlink = true;
#else
	if (pgwin32_is_junction(path))
		path_is_symlink = true;
#endif

	dir = opendir(path);
	if (!dir)
	{
		pg_log_error("could not open directory \"%s\": %m", path);
		exit(1);
	}

	while ((de = readdir(dir)) != NULL)
	{
		char		fn[MAXPGPATH];

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		/* Skip temporary files */
		if (strncmp(de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
			continue;

		/* Skip temporary folders */
		if (strncmp(de->d_name,
					PG_TEMP_FILES_DIR,
					strlen(PG_TEMP_FILES_DIR)) == 0)
			continue;

		snprintf(fn, sizeof(fn), "%s/%s", path, de->d_name);
		if (lstat(fn, &st) < 0)
		{
			pg_log_error("could not stat file \"%s\": %m", fn);
			exit(1);
		}

		if (S_ISREG(st.st_mode))
		{
			char		fnonly[MAXPGPATH];
			char	   *segmentpath;
			BlockNumber segmentno = 0;

			if (skipfile(de->d_name))
				continue;

			/*
			 * Cut off at the segment boundary (".") to get the segment number
			 * in order to mix it into the checksum.
			 */
			if (data_checksums)
			{
				strlcpy(fnonly, de->d_name, sizeof(fnonly));
				segmentpath = strchr(fnonly, '.');
				if (segmentpath != NULL)
				{
					*segmentpath++ = '\0';
					segmentno = atoi(segmentpath);
					if (segmentno == 0)
					{
						pg_log_error("invalid segment number %d in file name \"%s\"",
									 segmentno, fn);
						exit(1);
					}
				}
			}

			dirsize += st.st_size;

			/*
			 * No need to work on the file when calculating only the size of
			 * the items in the data folder.
			 */
			if (!sizeonly)
				scan_file(fn, segmentno);
		}
#ifndef WIN32
		else if (S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
#else
		else if (S_ISDIR(st.st_mode) || pgwin32_is_junction(fn))
#endif
		{
			/*
			 * If going through the entries of pg_tblspc, we assume to operate
			 * on tablespace locations where only TABLESPACE_VERSION_DIRECTORY
			 * is valid, resolving the linked locations and dive into them
			 * directly.
			 */
			if (subdir != NULL &&
				strncmp("pg_tblspc", subdir, strlen("pg_tblspc")) == 0)
			{
				char		tblspc_path[MAXPGPATH];
				struct stat tblspc_st;

				/*
				 * Resolve tablespace location path and check whether
				 * TABLESPACE_VERSION_DIRECTORY exists.  Not finding a valid
				 * location is unexpected, since there should be no orphaned
				 * links and no links pointing to something else than a
				 * directory.
				 */
				snprintf(tblspc_path, sizeof(tblspc_path), "%s/%s/%s",
						 path, de->d_name, TABLESPACE_VERSION_DIRECTORY);

				if (lstat(tblspc_path, &tblspc_st) < 0)
				{
					pg_log_error("could not stat file \"%s\": %m",
								 tblspc_path);
					exit(1);
				}

				/* Looks like a valid tablespace location */
				dirsize += scan_directory(fn,
										  TABLESPACE_VERSION_DIRECTORY,
										  sizeonly);

				/*
				 * Issue fsync recursively on the tablespace location and
				 * all its contents if requested. This is required here
				 * because fsync_dir_recurse() that will be called at the end
				 * of this program skips symlinks. But we don't need to do
				 * that if we're scanning database cluster because
				 * fsync_pgdata() will be called later.
				 */
				if (do_sync && path_is_symlink && !pgdata_mode)
					fsync_dir_recurse(tblspc_path);
			}
			else
			{
				dirsize += scan_directory(path, de->d_name, sizeonly);

				if (do_sync && path_is_symlink && !pgdata_mode)
					fsync_dir_recurse(fn);
			}
		}
	}

	closedir(dir);
	return dirsize;
}

static void
scan_file(const char *fn, BlockNumber segmentno)
{
	PGAlignedBlock buf;
	PageHeader	header = (PageHeader) buf.data;
	int			f;
	BlockNumber blockno;

	f = open(fn, PG_BINARY | O_RDWR, 0);
	if (f < 0)
	{
		pg_log_error("could not open file \"%s\": %m", fn);
		exit(1);
	}

	files++;

	for (blockno = 0;; blockno++)
	{
		int			r = read(f, buf.data, BLCKSZ);
		int			w;

		if (r == 0)
			break;

		if (r != BLCKSZ)
		{
			if (r < 0)
				pg_log_error("could not read block %u in file \"%s\": %m",
							 blockno, fn);
			else
				pg_log_error("could not read block %u in file \"%s\": read %d of %d",
							 blockno, fn, r, BLCKSZ);
			exit(1);
		}

		current_size += r;
		blocks++;

		/* New pages have no page lsn yet */
		if (PageIsNew(header))
			continue;

		/* Set page LSN in page header */
		PageSetLSN(buf.data, lsn);

		/* Set checksum in page header if requested */
		if (data_checksums)
		{
			header->pd_checksum =
				pg_checksum_page(buf.data, blockno + segmentno * RELSEG_SIZE);
		}

		/* Seek back to beginning of block */
		if (lseek(f, -BLCKSZ, SEEK_CUR) < 0)
		{
			pg_log_error("seek failed for block %u in file \"%s\": %m", blockno, fn);
			exit(1);
		}

		/* Write block with new LSN */
		w = write(f, buf.data, BLCKSZ);
		if (w != BLCKSZ)
		{
			if (w < 0)
				pg_log_error("could not write block %u in file \"%s\": %m",
							 blockno, fn);
			else
				pg_log_error("could not write block %u in file \"%s\": wrote %d of %d",
							 blockno, fn, w, BLCKSZ);
			exit(1);
		}

		if (showprogress)
			progress_report(false);
	}

	close(f);
}

/* Subdirectories under PGDATA to scan */
static const char	*const pgdata_subdirs[] = {"base", "global", "pg_tblspc"};

static int64
scan_pgdata(const char *basedir, bool sizeonly)
{
	int64	dirsize = 0;
	int		i;

	for (i = 0; i < lengthof(pgdata_subdirs); i++)
		dirsize += scan_directory(basedir, pgdata_subdirs[i], sizeonly);
	return dirsize;
}

static void
validate_datadir(const char *datadir)
{
	struct stat st;
	int		i;

	/* Does the database directory to scan exist? */
	if (lstat(datadir, &st) < 0)
	{
		if (errno == ENOENT)
			pg_log_error("directory \"%s\" does not exist", datadir);
		else
			pg_log_error("could not stat directory \"%s\": %m", datadir);
		exit(1);
	}

	/* Check whether database directory to scan is database cluster */
	pgdata_mode = true;
	for (i = 0; i < lengthof(pgdata_subdirs); i++)
	{
		char		path[MAXPGPATH];

		snprintf(path, sizeof(path), "%s/%s", datadir, pgdata_subdirs[i]);
		if (lstat(path, &st) < 0)
		{
			pgdata_mode = false;
			break;
		}
	}
}

/* Copy and paste from src/bin/pg_checksums/pg_checksums.c */
struct exclude_list_item
{
	const char *name;
	bool		match_prefix;
};

static const struct exclude_list_item skip[] = {
	{"pg_control", false},
	{"pg_filenode.map", false},
	{"pg_internal.init", true},
	{"PG_VERSION", false},
#ifdef EXEC_BACKEND
	{"config_exec_params", true},
#endif
	{NULL, false}
};

static bool
skipfile(const char *fn)
{
	int			excludeIdx;

	for (excludeIdx = 0; skip[excludeIdx].name != NULL; excludeIdx++)
	{
		int			cmplen = strlen(skip[excludeIdx].name);

		if (!skip[excludeIdx].match_prefix)
			cmplen++;
		if (strncmp(skip[excludeIdx].name, fn, cmplen) == 0)
			return true;
	}

	return false;
}

/*
 * Report current progress status.
 * Copy and paste from src/bin/pg_checksums/pg_checksums.c.
 */
static void
progress_report(bool finished)
{
	int			percent;
	char		total_size_str[32];
	char		current_size_str[32];
	pg_time_t	now;

	Assert(showprogress);

	now = time(NULL);
	if (now == last_progress_report && !finished)
		return;					/* Max once per second */

	/* Save current time */
	last_progress_report = now;

	/* Adjust total size if current_size is larger */
	if (current_size > total_size)
		total_size = current_size;

	/* Calculate current percentage of size done */
	percent = total_size ? (int) ((current_size) * 100 / total_size) : 0;

	/*
	 * Separate step to keep platform-dependent format code out of
	 * translatable strings.  And we only test for INT64_FORMAT availability
	 * in snprintf, not fprintf.
	 */
	snprintf(total_size_str, sizeof(total_size_str), INT64_FORMAT,
			 total_size / (1024 * 1024));
	snprintf(current_size_str, sizeof(current_size_str), INT64_FORMAT,
			 current_size / (1024 * 1024));

	fprintf(stderr, _("%*s/%s MB (%d%%) computed"),
			(int) strlen(current_size_str), current_size_str, total_size_str,
			percent);

	/*
	 * Stay on the same line if reporting to a terminal and we're not done
	 * yet.
	 */
	fputc((!finished && isatty(fileno(stderr))) ? '\r' : '\n', stderr);
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"directory", required_argument, NULL, 'D'},
		{"lsn", required_argument, NULL, 'l'},
		{"data-checksums", no_argument, NULL, 'k'},
		{"no-sync", no_argument, NULL, 'N'},
		{"progress", no_argument, NULL, 'P'},
		{NULL, 0, NULL, 0}
	};

	int			c;
	int			option_index;
	char	   *datadir = NULL;
	char	   *lsn_str = NULL;
	bool		have_error = false;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_reset_page_lsn"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_reset_page_lsn (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "D:l:kNP", long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'D':
				datadir = pg_strdup(optarg);
				break;
			case 'l':
				lsn_str = pg_strdup(optarg);
				break;
			case 'k':
				data_checksums = true;
				break;
			case 'N':
				do_sync = false;
				break;
			case 'P':
				showprogress = true;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}

	/* Complain if any arguments remain */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/*
	 * Required arguments
	 */
	if (datadir == NULL)
	{
		pg_log_error("no database directory specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	if (lsn_str == NULL)
	{
		pg_log_error("no LSN specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	/* Validate input LSN */
	lsn = pg_lsn_in_internal(lsn_str, &have_error);
	if (have_error)
	{
		pg_log_error("invalid argument for option %s", "-l");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
				progname);
		exit(1);
	}

	canonicalize_path(datadir);
	validate_datadir(datadir);

	/*
	 * If progress status information is requested, we need to scan the
	 * directory tree twice: once to know how much total data needs to be
	 * processed and once to do the real work.
	 */
	if (pgdata_mode)
	{
		if (showprogress)
			total_size = scan_pgdata(datadir, true);

		(void) scan_pgdata(datadir, false);
	}
	else
	{
		if (showprogress)
			total_size = scan_directory(datadir, NULL, true);

		(void) scan_directory(datadir, NULL, false);
	}

	if (showprogress)
		progress_report(true);

	/* Make the data durable on disk */
	if (do_sync)
	{
		if (pgdata_mode)
			fsync_pgdata(datadir, PG_VERSION_NUM);
		else
			fsync_dir_recurse(datadir);
	}

	printf(_("Operation completed\n"));
	printf(_("Files scanned:  %s\n"), psprintf(INT64_FORMAT, files));
	printf(_("Blocks scanned: %s\n"), psprintf(INT64_FORMAT, blocks));

	return 0;
}
