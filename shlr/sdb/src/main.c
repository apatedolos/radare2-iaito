/* sdb - MIT - Copyright 2011-2016 - pancake */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "sdb.h"

#define MODE_ZERO '0'
#define MODE_JSON 'j'
#define MODE_DFLT 0

static int save = 0;
static Sdb *s = NULL;
static ut32 options = SDB_OPTION_FS | SDB_OPTION_NOSTAMP;

static void terminate(int sig UNUSED) {
	if (!s) {
		return;
	}
	if (save && !sdb_sync (s)) {
		sdb_free (s);
		s = NULL;
		exit (1);
	}
	sdb_free (s);
	exit (0);
}

#define BS 128
#define USE_SLURPIN 1

static char *stdin_slurp(int *sz) {
	int blocksize = BS;
	static int bufsize = BS;
	static char *next = NULL;
	static int nextlen = 0;
	int len, rr, rr2;
	char *tmp, *buf = NULL;
	if (sz) {
		*sz = 0;
	}
#if USE_SLURPIN
	if (!sz) {
		/* this is faster but have limits */
		/* run test/add10k.sh script to benchmark */
		const int buf_size = 96096;
		buf = calloc (1, buf_size);
		if (!fgets (buf, buf_size - 1, stdin)) {
			free (buf);
			return NULL;
		}
		if (feof (stdin)) {
			free (buf);
			return NULL;
		}
		int buf_len = strlen (buf) - 1;
		buf[buf_len] = 0;
		char *newbuf = realloc (buf, buf_len + 1);
		if (newbuf) {
			return newbuf;
		}
		free (buf);
		return NULL;
	}
#endif
	buf = calloc (BS + 1, 1);
	if (!buf) {
		return NULL;
	}

	len = 0;
	for (;;) {
		if (next) {
			free (buf);
			buf = next;
			bufsize = nextlen + blocksize;
			//len = nextlen;
			rr = nextlen;
			rr2 = read (0, buf + nextlen, blocksize);
			if (rr2 > 0) {
				rr += rr2;
				bufsize += rr2;
			}
			next = NULL;
			nextlen = 0;
		} else {
			rr = read (0, buf + len, blocksize);
		}
		if (rr < 1) { // EOF
			buf[len] = 0;
			next = NULL;
			break;
		}
		len += rr;
		//buf[len] = 0;
#if !USE_SLURPIN
		if (!sz) {
			char *nl = strchr (buf, '\n');
			if (nl) {
				*nl++ = 0;
				int nlen = nl - buf;
				nextlen = len - nlen;
				if (nextlen > 0) {
					next = malloc (nextlen + blocksize + 1);
					if (!next) {
						eprintf ("Cannot malloc %d\n", nextlen);
						break;
					}
					memcpy (next, nl, nextlen);
					if (!*next) {
						next = NULL;
					} else {
						//	continue;
					}
				} else {
					next = NULL;
					nextlen = 0; //strlen (next);;
				}
				break;
			}
		}
#endif
		bufsize += blocksize;
		tmp = realloc (buf, bufsize + 1);
		if (!tmp) {
			bufsize -= blocksize;
			break;
		}
		buf = tmp;
	}
	if (sz) {
		*sz = len;
	}
	//eprintf ("LEN %d (%s)\n", len, buf);
	if (len < 1) {
		free (buf);
		buf = NULL;
		return NULL;
	}
	buf[len] = 0;
	return buf;
}

#if USE_MMAN
static void synchronize(int sig UNUSED) {
	// TODO: must be in sdb_sync() or wat?
	Sdb *n;
	sdb_sync (s);
	n = sdb_new (s->path, s->name, s->lock);
	if (n) {
		sdb_config (n, options);
		sdb_free (s);
		s = n;
	}
}
#endif
static int sdb_grep_dump(const char *db, int fmt, bool grep,
                         const char *expgrep) {
	char *v;
	char k[SDB_MAX_KEY] = {
		0
	};
	const char *comma = "";
	Sdb *s = sdb_new (NULL, db, 0);
	if (!s) {
		return 1;
	}
	sdb_config (s, options);
	sdb_dump_begin (s);
	if (fmt == MODE_JSON) {
		printf ("{");
	}
	while (sdb_dump_dupnext (s, k, &v, NULL)) {
		if (grep && !strstr (k, expgrep) && !strstr (v, expgrep)) {
			continue;
		}
		switch (fmt) {
		case MODE_JSON:
			if (!strcmp (v, "true") || !strcmp (v, "false")) {
				printf ("%s\"%s\":%s", comma, k, v);
			} else if (sdb_isnum (v)) {
				printf ("%s\"%s\":%llu", comma, k, sdb_atoi (v));
			} else if (*v == '{' || *v == '[') {
				printf ("%s\"%s\":%s", comma, k, v);
			} else {
				printf ("%s\"%s\":\"%s\"", comma, k, v);
			}
			comma = ",";
			break;
		case MODE_ZERO:
			printf ("%s=%s", k, v);
			fwrite ("", 1, 1, stdout);
			break;
		default:
			printf ("%s=%s\n", k, v);
			break;
		}
		free (v);
	}
	switch (fmt) {
	case MODE_ZERO:
		fflush (stdout);
		write (1, "", 1);
		break;
	case MODE_JSON:
		printf ("}\n");
		break;
	}
	sdb_free (s);
	return 0;
}
static int sdb_grep(const char *db, int fmt, const char *grep) {
	return sdb_grep_dump (db, fmt, true, grep);
}

static int sdb_dump(const char *db, int fmt) {
	return sdb_grep_dump (db, fmt, false, NULL);
}

static int insertkeys(Sdb *s, const char **args, int nargs, int mode) {
	int must_save = 0;
	if (args && nargs > 0) {
		int i;
		for (i = 0; i < nargs; i++) {
			switch (mode) {
			case '-':
				must_save |= sdb_query (s, args[i]);
				break;
			case '=':
				if (strchr (args[i], '=')) {
					char *v, *kv = (char *) strdup (args[i]);
					v = strchr (kv, '=');
					if (v) {
						*v++ = 0;
						sdb_disk_insert (s, kv, v);
					}
					free (kv);
				}
				break;
			}
		}
	}
	return must_save;
}

static int createdb(const char *f, const char **args, int nargs) {
	char *line, *eq;
	s = sdb_new (NULL, f, 0);
	if (!s || !sdb_disk_create (s)) {
		eprintf ("Cannot create database\n");
		return 1;
	}
	insertkeys (s, args, nargs, '=');
	sdb_config (s, options);
	for (; (line = stdin_slurp (NULL));) {
		if ((eq = strchr (line, '='))) {
			*eq++ = 0;
			sdb_disk_insert (s, line, eq);
		}
		free (line);
	}
	sdb_disk_finish (s);
	return 0;
}

static int showusage(int o) {
	printf ("usage: sdb [-0cdehjJv|-D A B] [-|db] "
		"[.file]|[-=]|[-+][(idx)key[:json|=value] ..]\n");
	if (o == 2) {
		printf ("  -0      terminate results with \\x00\n"
			"  -c      count the number of keys database\n"
			"  -d      decode base64 from stdin\n"
			"  -D      diff two databases\n"
			"  -e      encode stdin as base64\n"
			"  -h      show this help\n"
			"  -j      output in json\n"
			"  -J      enable journaling\n"
			"  -v      show version information\n");
		return 0;
	}
	return o;
}

static int showversion(void) {
	printf ("sdb "SDB_VERSION "\n");
	fflush (stdout);
	return 0;
}

static int jsonIndent() {
	int len;
	char *out;
	char *in = stdin_slurp (&len);
	if (!in) {
		return 0;
	}
	out = sdb_json_indent (in, "  ");
	if (!out) {
		free (in);
		return 1;
	}
	puts (out);
	free (out);
	free (in);
	return 0;
}

static int base64encode() {
	char *out;
	int len = 0;
	ut8 *in = (ut8 *) stdin_slurp (&len);
	if (!in) {
		return 0;
	}
	out = sdb_encode (in, len);
	if (!out) {
		free (in);
		return 1;
	}
	puts (out);
	free (out);
	free (in);
	return 0;
}

static int base64decode() {
	ut8 *out;
	int len, ret = 1;
	char *in = (char *) stdin_slurp (&len);
	if (in) {
		out = sdb_decode (in, &len);
		if (out) {
			if (len >= 0) {
				write (1, out, len);
				ret = 0;
			}
			free (out);
		}
		free (in);
	}
	return ret;
}

static int dbdiff(const char *a, const char *b) {
	int n = 0;
	char *v;
	char k[SDB_MAX_KEY] = {
		0
	};
	const char *v2;
	Sdb *A = sdb_new (NULL, a, 0);
	Sdb *B = sdb_new (NULL, b, 0);
	sdb_dump_begin (A);
	while (sdb_dump_dupnext (A, k, &v, NULL)) {
		v2 = sdb_const_get (B, k, 0);
		if (!v2) {
			printf ("%s=\n", k);
			n = 1;
		}
	}
	sdb_dump_begin (B);
	while (sdb_dump_dupnext (B, k, &v, NULL)) {
		if (!v || !*v) {
			continue;
		}
		v2 = sdb_const_get (A, k, 0);
		if (!v2 || strcmp (v, v2)) {
			printf ("%s=%s\n", k, v2);
			n = 1;
		}
	}
	sdb_free (A);
	sdb_free (B);
	free (v);
	return n;
}

int showcount (const char *db) {
	ut32 d;
	s = sdb_new (NULL, db, 0);
	if (sdb_stats (s, &d, NULL)) {
		printf ("%d\n", d);
	}
	// TODO: show version, timestamp information
	sdb_free (s);
	return 0;
}

int main(int argc, const char **argv) {
	char *line;
	const char *arg, *grep = NULL;
	int i, ret, fmt = MODE_DFLT;
	int db0 = 1, argi = 1;
	bool interactive = false;

	/* terminate flags */
	if (argc < 2) {
		return showusage (1);
	}
	arg = argv[1];

	if (arg[0] == '-') {// && arg[1] && arg[2]==0) {
		switch (arg[1]) {
		case 0:
			/* no-op */
			break;
		case '0':
			fmt = MODE_ZERO;
			db0++;
			argi++;
			if (db0 >= argc) {
				return showusage (1);
			}
			break;
		case 'g':
			db0 += 2;
			if (db0 >= argc) {
				return showusage (1);
			}
			grep = argv[2];
			argi += 2;
			break;
		case 'J':
			options |= SDB_OPTION_JOURNAL;
			db0++;
			argi++;
			if (db0 >= argc) {
				return showusage (1);
			}
			break;
		case 'c': return (argc < 3)? showusage (1): showcount (argv[2]);
		case 'v': return showversion ();
		case 'h': return showusage (2);
		case 'e': return base64encode ();
		case 'd': return base64decode ();
		case 'D':
			if (argc == 4) {
				return dbdiff (argv[2], argv[3]);
			}
			return showusage (0);
		case 'j':
			if (argc > 2) {
				return sdb_dump (argv[db0 + 1], MODE_JSON);
			}
			return jsonIndent ();
		default:
			eprintf ("Invalid flag %s\n", arg);
			break;
		}
	}

	/* sdb - */
	if (argi == 1 && !strcmp (argv[argi], "-")) {
		/* no database */
		argv[argi] = "";
		if (argc == db0 + 1) {
			interactive = true;
			/* if no argument passed */
			argv[argi] = "-";
			argc++;
			argi++;
		}
	}
	/* sdb dbname */
	if (argc - 1 == db0) {
		if (grep) {
			return sdb_grep (argv[db0], fmt, grep);
		}
		return sdb_dump (argv[db0], fmt);
	}
#if USE_MMAN
	signal (SIGINT, terminate);
	signal (SIGHUP, synchronize);
#endif
	ret = 0;
	if (interactive || !strcmp (argv[db0 + 1], "-")) {
		if ((s = sdb_new (NULL, argv[db0], 0))) {
			sdb_config (s, options);
			int kvs = db0 + 2;
			if (kvs < argc) {
				save |= insertkeys (s, argv + argi + 2, argc - kvs, '-');
			}
			for (; (line = stdin_slurp (NULL));) {
				save |= sdb_query (s, line);
				if (fmt) {
					fflush (stdout);
					write (1, "", 1);
				}
				free (line);
			}
		}
	} else if (!strcmp (argv[db0 + 1], "=")) {
		ret = createdb (argv[db0], NULL, 0);
	} else {
		s = sdb_new (NULL, argv[db0], 0);
		if (!s) {
			return 1;
		}
		sdb_config (s, options);
		for (i = db0 + 1; i < argc; i++) {
			save |= sdb_query (s, argv[i]);
			if (fmt) {
				fflush (stdout);
				write (1, "", 1);
			}
		}
	}
	terminate (0);
	return ret;
}
