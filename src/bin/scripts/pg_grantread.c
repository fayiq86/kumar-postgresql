/*-------------------------------------------------------------------------
 *
 * pg_grantread
 *		Give a role read access to one database (Kumar Server).
 *
 * pg_read_all_data grants read access to every database of the cluster.
 * Read access to a single database needs several kinds of grants, and the
 * last one is easy to forget:
 *
 *	GRANT CONNECT ON DATABASE db TO role;
 *	GRANT USAGE ON SCHEMA s TO role;						(each schema)
 *	GRANT SELECT ON ALL TABLES IN SCHEMA s TO role;			(each schema)
 *	GRANT SELECT ON ALL SEQUENCES IN SCHEMA s TO role;		(each schema)
 *	ALTER DEFAULT PRIVILEGES FOR ROLE owner IN SCHEMA s
 *		GRANT SELECT ON TABLES TO role;						(future tables)
 *
 * Default privileges apply only to objects created later by the role named
 * in FOR ROLE, so they are set for every role that owns a table in the
 * schema, and for the schema's owner. A schema owned by the pseudo-role
 * pg_database_owner (like "public" since PostgreSQL 15) uses the database's
 * actual owner instead, since nobody creates tables as pg_database_owner.
 * "ALL TABLES" also covers views, materialized views and foreign tables.
 *
 * Everything runs in one transaction. A GRANT that fails aborts the whole
 * run (nothing is changed); default privileges that the running user may
 * not set for a given owner are skipped and reported instead.
 *
 * src/bin/scripts/pg_grantread.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include "common.h"
#include "common/logging.h"
#include "fe_utils/cancel.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/query_utils.h"
#include "fe_utils/simple_list.h"

static PGconn *conn;
static bool echo = false;

static void
help(const char *progname)
{
	printf(_("%s gives a role read access to one database.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... ROLE\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -d, --dbname=DBNAME       database to give read access to\n"));
	printf(_("  -n, --schema=SCHEMA       only this schema (can be repeated;\n"
			 "                            default: all non-system schemas)\n"));
	printf(_("      --revoke              take the read access away again\n"));
	printf(_("      --no-future           do not change default privileges for\n"
			 "                            tables created in the future\n"));
	printf(_("      --dry-run             print the SQL statements, change nothing\n"));
	printf(_("  -e, --echo                show the commands being sent to the server\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME       database server host or socket directory\n"));
	printf(_("  -p, --port=PORT           database server port\n"));
	printf(_("  -U, --username=USERNAME   user name to connect as\n"));
	printf(_("  -w, --no-password         never prompt for password\n"));
	printf(_("  -W, --password            force password prompt\n"));
	printf(_("\nUnlike the predefined role pg_read_all_data, which covers every database\n"
			 "of the cluster, this grants read access to one database only. Row-level\n"
			 "security policies still apply. Functions, large objects and other\n"
			 "databases are not affected.\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/* Quote an identifier; the result lives until the program exits. */
static char *
qid(const char *name)
{
	char	   *q = PQescapeIdentifier(conn, name, strlen(name));

	if (q == NULL)
		pg_fatal("could not quote identifier \"%s\": %s", name, PQerrorMessage(conn));
	return q;
}

/* Quote a literal; the result lives until the program exits. */
static char *
qlit(const char *value)
{
	char	   *q = PQescapeLiteral(conn, value, strlen(value));

	if (q == NULL)
		pg_fatal("could not quote literal \"%s\": %s", value, PQerrorMessage(conn));
	return q;
}

/* Run a query and return its single-value result as a new string. */
static char *
query_value(const char *sql)
{
	PGresult   *res = executeQuery(conn, sql, echo);
	char	   *value = NULL;

	if (PQntuples(res) == 1 && !PQgetisnull(res, 0, 0))
		value = pg_strdup(PQgetvalue(res, 0, 0));
	PQclear(res);
	return value;
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"dbname", required_argument, NULL, 'd'},
		{"schema", required_argument, NULL, 'n'},
		{"echo", no_argument, NULL, 'e'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"revoke", no_argument, NULL, 1},
		{"no-future", no_argument, NULL, 2},
		{"dry-run", no_argument, NULL, 3},
		{NULL, 0, NULL, 0}
	};

	const char *progname;
	int			optindex;
	int			c;
	const char *dbname = NULL;
	const char *host = NULL;
	const char *port = NULL;
	const char *username = NULL;
	enum trivalue prompt_password = TRI_DEFAULT;
	ConnParams	cparams;
	bool		revoke = false;
	bool		future = true;
	bool		dry_run = false;
	const char *role;
	char	   *db;
	SimpleStringList schemas = {NULL, NULL};
	SimpleStringList statements = {NULL, NULL};
	SimpleStringList skipped = {NULL, NULL};
	SimpleStringListCell *cell;
	PQExpBufferData sql;
	PGresult   *res;
	int			nschemas = 0;
	long		ntables = 0;
	long		nsequences = 0;
	int			nowners = 0;
	int			ndefault_ok = 0;
	int			ndefault_skipped = 0;
	const char *verb;
	const char *prep;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pgscripts"));

	handle_help_version_opts(argc, argv, "pg_grantread", help);

	while ((c = getopt_long(argc, argv, "d:n:eh:p:U:wW", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'd':
				dbname = pg_strdup(optarg);
				break;
			case 'n':
				simple_string_list_append(&schemas, optarg);
				break;
			case 'e':
				echo = true;
				break;
			case 'h':
				host = pg_strdup(optarg);
				break;
			case 'p':
				port = pg_strdup(optarg);
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'w':
				prompt_password = TRI_NO;
				break;
			case 'W':
				prompt_password = TRI_YES;
				break;
			case 1:
				revoke = true;
				break;
			case 2:
				future = false;
				break;
			case 3:
				dry_run = true;
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	if (optind >= argc)
	{
		pg_log_error("no role specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}
	role = argv[optind++];
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")", argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	cparams.dbname = dbname;
	cparams.pghost = host;
	cparams.pgport = port;
	cparams.pguser = username;
	cparams.prompt_password = prompt_password;
	cparams.override_dbname = NULL;

	setup_cancel_handler(NULL);

	conn = connectDatabase(&cparams, progname, echo, false, true);
	initPQExpBuffer(&sql);

	/* The role must exist */
	printfPQExpBuffer(&sql, "SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = %s", qlit(role));
	if (query_value(sql.data) == NULL)
		pg_fatal("role \"%s\" does not exist", role);

	db = query_value("SELECT pg_catalog.current_database()");

	/* Schemas: the given ones (they must exist), or all non-system ones */
	if (schemas.head)
	{
		for (cell = schemas.head; cell; cell = cell->next)
		{
			printfPQExpBuffer(&sql, "SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname = %s",
							  qlit(cell->val));
			if (query_value(sql.data) == NULL)
				pg_fatal("schema \"%s\" does not exist in database \"%s\"", cell->val, db);
		}
	}
	else
	{
		int			i;

		res = executeQuery(conn,
						   "SELECT nspname FROM pg_catalog.pg_namespace\n"
						   "WHERE nspname <> 'information_schema'\n"
						   "  AND nspname !~ '^pg_'\n"
						   "ORDER BY 1", echo);
		for (i = 0; i < PQntuples(res); i++)
			simple_string_list_append(&schemas, PQgetvalue(res, i, 0));
		PQclear(res);
	}

	verb = revoke ? "REVOKE" : "GRANT";
	prep = revoke ? "FROM" : "TO";

	/* Build the list of statements */
	printfPQExpBuffer(&sql, "%s CONNECT ON DATABASE %s %s %s;", verb, qid(db), prep, qid(role));
	simple_string_list_append(&statements, sql.data);

	for (cell = schemas.head; cell; cell = cell->next)
	{
		const char *s = qid(cell->val);
		int			i;

		nschemas++;

		/* counts for the summary */
		printfPQExpBuffer(&sql,
						  "SELECT count(*) FILTER (WHERE c.relkind IN ('r','p','v','m','f')),\n"
						  "       count(*) FILTER (WHERE c.relkind = 'S')\n"
						  "FROM pg_catalog.pg_class c\n"
						  "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
						  "WHERE n.nspname = %s", qlit(cell->val));
		res = executeQuery(conn, sql.data, echo);
		ntables += atol(PQgetvalue(res, 0, 0));
		nsequences += atol(PQgetvalue(res, 0, 1));
		PQclear(res);

		if (!revoke)
		{
			printfPQExpBuffer(&sql, "GRANT USAGE ON SCHEMA %s TO %s;", s, qid(role));
			simple_string_list_append(&statements, sql.data);
		}
		printfPQExpBuffer(&sql, "%s SELECT ON ALL TABLES IN SCHEMA %s %s %s;",
						  verb, s, prep, qid(role));
		simple_string_list_append(&statements, sql.data);
		printfPQExpBuffer(&sql, "%s SELECT ON ALL SEQUENCES IN SCHEMA %s %s %s;",
						  verb, s, prep, qid(role));
		simple_string_list_append(&statements, sql.data);
		if (revoke)
		{
			printfPQExpBuffer(&sql, "REVOKE USAGE ON SCHEMA %s FROM %s;", s, qid(role));
			simple_string_list_append(&statements, sql.data);
		}

		if (!future)
			continue;

		/*
		 * Default privileges only cover objects later created by the role in
		 * FOR ROLE: set them for each owner of a table in the schema and for
		 * the schema owner.
		 */
		printfPQExpBuffer(&sql,
						  "SELECT pg_catalog.pg_get_userbyid(c.relowner)\n"
						  "FROM pg_catalog.pg_class c\n"
						  "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
						  "WHERE n.nspname = %s AND c.relkind IN ('r','p','v','m','f','S')\n"
						  "UNION\n"
						  "SELECT CASE WHEN r.rolname = 'pg_database_owner'\n"
						  "            THEN pg_catalog.pg_get_userbyid(d.datdba)\n"
						  "            ELSE r.rolname END\n"
						  "FROM pg_catalog.pg_namespace n\n"
						  "JOIN pg_catalog.pg_roles r ON r.oid = n.nspowner\n"
						  "JOIN pg_catalog.pg_database d ON d.datname = pg_catalog.current_database()\n"
						  "WHERE n.nspname = %s\n"
						  "ORDER BY 1", qlit(cell->val), qlit(cell->val));
		res = executeQuery(conn, sql.data, echo);
		for (i = 0; i < PQntuples(res); i++)
		{
			const char *owner = qid(PQgetvalue(res, i, 0));

			nowners++;
			printfPQExpBuffer(&sql,
							  "ALTER DEFAULT PRIVILEGES FOR ROLE %s IN SCHEMA %s %s SELECT ON TABLES %s %s;",
							  owner, s, verb, prep, qid(role));
			simple_string_list_append(&statements, sql.data);
			printfPQExpBuffer(&sql,
							  "ALTER DEFAULT PRIVILEGES FOR ROLE %s IN SCHEMA %s %s SELECT ON SEQUENCES %s %s;",
							  owner, s, verb, prep, qid(role));
			simple_string_list_append(&statements, sql.data);
		}
		PQclear(res);
	}

	/* --dry-run: show and stop */
	if (dry_run)
	{
		for (cell = statements.head; cell; cell = cell->next)
			printf("%s\n", cell->val);
		printf(_("-- dry run: nothing was changed\n"));
		PQfinish(conn);
		return 0;
	}

	/*
	 * Run everything in one transaction. executeCommand() exits on error,
	 * which closes the connection and so rolls the transaction back: either
	 * all grants happen or none. Default privileges may legitimately be
	 * refused for an owner the running user cannot act for; those run in a
	 * savepoint and are reported as skipped instead.
	 */
	executeCommand(conn, "BEGIN", echo);
	for (cell = statements.head; cell; cell = cell->next)
	{
		if (strncmp(cell->val, "ALTER DEFAULT PRIVILEGES", 24) != 0)
		{
			executeCommand(conn, cell->val, echo);
			continue;
		}

		executeCommand(conn, "SAVEPOINT pg_grantread", echo);
		if (echo)
			printf("%s\n", cell->val);
		res = PQexec(conn, cell->val);
		if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			executeCommand(conn, "RELEASE SAVEPOINT pg_grantread", echo);
			if (strstr(cell->val, " ON TABLES "))
				ndefault_ok++;
		}
		else
		{
			if (strstr(cell->val, " ON TABLES "))
				ndefault_skipped++;
			printfPQExpBuffer(&sql, "%s\n    %s", cell->val,
							  PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY));
			simple_string_list_append(&skipped, sql.data);
			executeCommand(conn, "ROLLBACK TO SAVEPOINT pg_grantread", echo);
		}
		PQclear(res);
	}
	executeCommand(conn, "COMMIT", echo);

	if (revoke)
		printf(_("Revoked read access on database \"%s\" from \"%s\": "), db, role);
	else
		printf(_("Granted read access on database \"%s\" to \"%s\": "), db, role);
	printf(_("%d schema(s), %ld table(s), %ld sequence(s)"), nschemas, ntables, nsequences);
	if (future)
	{
		printf(_("; future tables of %d of %d owner(s) covered"), ndefault_ok, nowners);
		if (ndefault_skipped > 0)
			printf(_(" (%d skipped, see warnings)"), ndefault_skipped);
	}
	printf(".\n");

	for (cell = skipped.head; cell; cell = cell->next)
		pg_log_warning("skipped: %s", cell->val);

	termPQExpBuffer(&sql);
	PQfinish(conn);
	return 0;
}
