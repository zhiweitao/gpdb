/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2011, PostgreSQL Global Development Group
 *
 * src/bin/psql/copy.c
 */
#include "postgres_fe.h"
#include "copy.h"

#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>
#ifndef WIN32
#include <unistd.h>				/* for isatty */
#else
#include <io.h>					/* I think */
#endif

#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "pqsignal.h"
#include "dumputils.h"

#include "settings.h"
#include "common.h"
#include "prompt.h"
#include "stringutils.h"


/*
 * parse_slash_copy
 * -- parses \copy command line
 *
 * The documented syntax is:
 *	\copy tablename [(columnlist)] from|to filename [options]
 *	\copy ( select stmt ) to filename [options]
 *
 * where 'filename' can be one of the following:
 *  '<file path>' | PROGRAM '<command>' | stdin | stdout | pstdout | pstdout
 *
 * An undocumented fact is that you can still write BINARY before the
 * tablename; this is a hangover from the pre-7.3 syntax.  The options
 * syntax varies across backend versions, but we avoid all that mess
 * by just transmitting the stuff after the filename literally.
 *
 * table name can be double-quoted and can have a schema part.
 * column names can be double-quoted.
 * filename can be single-quoted like SQL literals.
 * command must be single-quoted like SQL literals
 *
 * returns a malloc'ed structure with the options, or NULL on parsing error
 */

struct copy_options
{
	char	   *before_tofrom;	/* COPY string before TO/FROM */
	char	   *after_tofrom;	/* COPY string after TO/FROM filename */
	char	   *file;			/* NULL = stdin/stdout */
	bool		program;		/* is 'file' a program to popen? */
	bool		psql_inout;		/* true = use psql stdin/stdout */
	bool		from;			/* true = FROM, false = TO */
};


static void
free_copy_options(struct copy_options * ptr)
{
	if (!ptr)
		return;
	free(ptr->before_tofrom);
	free(ptr->after_tofrom);
	free(ptr->file);
	free(ptr);
}


/* concatenate "more" onto "var", freeing the original value of *var */
static void
xstrcat(char **var, const char *more)
{
	char	   *newvar;

	newvar = pg_malloc(strlen(*var) + strlen(more) + 1);
	strcpy(newvar, *var);
	strcat(newvar, more);
	free(*var);
	*var = newvar;
}


static struct copy_options *
parse_slash_copy(const char *args)
{
	struct copy_options *result;
	char	   *token;
	const char *whitespace = " \t\n\r";
	char		nonstd_backslash = standard_strings() ? 0 : '\\';

	if (!args)
	{
		psql_error("\\copy: arguments required\n");
		return NULL;
	}

	result = pg_calloc(1, sizeof(struct copy_options));

	result->before_tofrom = pg_strdup("");		/* initialize for appending */

	token = strtokx(args, whitespace, ".,()", "\"",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	/* The following can be removed when we drop 7.3 syntax support */
	if (pg_strcasecmp(token, "binary") == 0)
	{
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	/* Handle COPY (SELECT) case */
	if (token[0] == '(')
	{
		int			parens = 1;

		while (parens > 0)
		{
			xstrcat(&result->before_tofrom, " ");
			xstrcat(&result->before_tofrom, token);
			token = strtokx(NULL, whitespace, "()", "\"'",
							nonstd_backslash, true, false, pset.encoding);
			if (!token)
				goto error;
			if (token[0] == '(')
				parens++;
			else if (token[0] == ')')
				parens--;
		}
	}

	xstrcat(&result->before_tofrom, " ");
	xstrcat(&result->before_tofrom, token);
	token = strtokx(NULL, whitespace, ".,()", "\"",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	/*
	 * strtokx() will not have returned a multi-character token starting with
	 * '.', so we don't need strcmp() here.  Likewise for '(', etc, below.
	 */
	if (token[0] == '.')
	{
		/* handle schema . table */
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (token[0] == '(')
	{
		/* handle parenthesized column list */
		for (;;)
		{
			xstrcat(&result->before_tofrom, " ");
			xstrcat(&result->before_tofrom, token);
			token = strtokx(NULL, whitespace, "()", "\"",
							0, false, false, pset.encoding);
			if (!token)
				goto error;
			if (token[0] == ')')
				break;
		}
		xstrcat(&result->before_tofrom, " ");
		xstrcat(&result->before_tofrom, token);
		token = strtokx(NULL, whitespace, ".,()", "\"",
						0, false, false, pset.encoding);
		if (!token)
			goto error;
	}

	if (pg_strcasecmp(token, "from") == 0)
		result->from = true;
	else if (pg_strcasecmp(token, "to") == 0)
		result->from = false;
	else
		goto error;

	/* { 'filename' | PROGRAM 'command' | STDIN | STDOUT | PSTDIN | PSTDOUT } */
	token = strtokx(NULL, whitespace, NULL, "'",
					0, false, false, pset.encoding);
	if (!token)
		goto error;

	if (pg_strcasecmp(token, "program") == 0)
	{
		int toklen;

		token = strtokx(NULL, whitespace, NULL, "'",
						0, false, false, pset.encoding);
		if (!token)
			goto error;

		/*
		 * The shell command must be quoted. This isn't fool-proof, but catches
		 * most quoting errors.
		 */
		toklen = strlen(token);
		if (token[0] != '\'' || toklen < 2 || token[toklen - 1] != '\'')
			goto error;

		strip_quotes(token, '\'', 0, pset.encoding);

		result->program = true;
		result->file = pg_strdup(token);
	}
	else if (pg_strcasecmp(token, "stdin") == 0 ||
		pg_strcasecmp(token, "stdout") == 0)
	{
		result->file = NULL;
	}
	else if (pg_strcasecmp(token, "pstdin") == 0 ||
			 pg_strcasecmp(token, "pstdout") == 0)
	{
		result->psql_inout = true;
		result->file = NULL;
	}
	else
	{
		/* filename can be optionally quoted */
		strip_quotes(token, '\'', 0, pset.encoding);
		result->file = pg_strdup(token);
		expand_tilde(&result->file);
	}

	/* Collect the rest of the line (COPY options) */
	token = strtokx(NULL, "", NULL, NULL,
					0, false, false, pset.encoding);
	if (token)
		result->after_tofrom = pg_strdup(token);

	return result;

error:
	if (token)
		psql_error("\\copy: parse error at \"%s\"\n", token);
	else
		psql_error("\\copy: parse error at end of line\n");
	free_copy_options(result);

	return NULL;
}


/* \copy command doesn't support on segment option */
static void
trim(char *s)
{
	int s_len = strlen(s);
	if (strlen(s) == 0)
		return;
	char *read = s + 1;
	char *write = s;
	*write = toupper(*write);
	if (*write == '\t')
	{
		*write = ' ';
	}
	for (int i = 1; i < s_len; i++)
	{
		if (*read == '\0')
		{
			*(++write) = '\0';
			break;
		}

		if (!(isspace(*write) && isspace(*read)))
		{
			*(++write) = isspace(*read)?' ':toupper(*read);
		}
		read++;
	}
	return;
}

static bool
is_on_segment(char * after_tofrom)
{
	bool on_segment = false;
	if (!after_tofrom || strlen(after_tofrom) == 0)
		return false;
	char *s = pg_strdup(after_tofrom);
	trim(s);
	if (strstr(s, "ON SEGMENT"))
	{
		on_segment = true;
	}
	free(s);
	return on_segment;
}


/*
 * Execute a \copy command (frontend copy). We have to open a file, then
 * submit a COPY query to the backend and either feed it data from the
 * file or route its response into the file.
 */
bool
do_copy(const char *args)
{
	PQExpBufferData query;
	FILE	   *copystream;
	struct copy_options *options;
	PGresult   *result;
	bool		success;
	struct stat st;

	/* parse options */
	options = parse_slash_copy(args);

	if (!options)
		return false;

	if (options->after_tofrom && is_on_segment(options->after_tofrom))
	{
		psql_error("\\COPY command doesn't support ON SEGMENT\n");
		free_copy_options(options);
		return false;
	}
	/* prepare to read or write the target file */
	if (options->file && !options->program)
		canonicalize_path(options->file);

	if (options->from)
	{
		if (options->file)
		{
			if (options->program)
			{
				fflush(stdout);
				fflush(stderr);
				errno = 0;
				copystream = popen(options->file, PG_BINARY_R);
			}
			else
				copystream = fopen(options->file, PG_BINARY_R);
		}
		else if (!options->psql_inout)
			copystream = pset.cur_cmd_source;
		else
			copystream = stdin;
	}
	else
	{
		if (options->file)
		{
			if (options->program)
			{
				fflush(stdout);
				fflush(stderr);
				errno = 0;
#ifndef WIN32
				pqsignal(SIGPIPE, SIG_IGN);
#endif
				copystream = popen(options->file, PG_BINARY_W);
			}
			else
				copystream = fopen(options->file, PG_BINARY_W);
		}
		else if (!options->psql_inout)
			copystream = pset.queryFout;
		else
			copystream = stdout;
	}

	if (!copystream)
	{
		if (options->program)
			psql_error("could not execute command \"%s\": %s\n",
					options->file, strerror(errno));
		else
			psql_error("%s: %s\n",
					options->file, strerror(errno));
		free_copy_options(options);
		return false;
	}

	if (!options->program)
	{
		/* make sure the specified file is not a directory */
		fstat(fileno(copystream), &st);
		if (S_ISDIR(st.st_mode))
		{
			fclose(copystream);
			psql_error("%s: cannot copy from/to a directory\n",
					   options->file);
			free_copy_options(options);
			return false;
		}
	}

	/* build the command we will send to the backend */
	initPQExpBuffer(&query);
	printfPQExpBuffer(&query, "COPY ");
	appendPQExpBufferStr(&query, options->before_tofrom);
	if (options->from)
		appendPQExpBuffer(&query, " FROM STDIN ");
	else
		appendPQExpBuffer(&query, " TO STDOUT ");
	if (options->after_tofrom)
		appendPQExpBufferStr(&query, options->after_tofrom);

	result = PSQLexec(query.data, true);
	termPQExpBuffer(&query);

	switch (PQresultStatus(result))
	{
		case PGRES_COPY_OUT:
			SetCancelConn();
			success = handleCopyOut(pset.db, copystream);
			ResetCancelConn();
			break;
		case PGRES_COPY_IN:
			SetCancelConn();
			success = handleCopyIn(pset.db, copystream,
								   PQbinaryTuples(result));
			ResetCancelConn();
			break;
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			success = false;
			psql_error("\\copy: %s", PQerrorMessage(pset.db));
			break;
		default:
			success = false;
			psql_error("\\copy: unexpected response (%d)\n",
					   PQresultStatus(result));
			break;
	}

	PQclear(result);

	/*
	 * Make sure we have pumped libpq dry of results; else it may still be in
	 * ASYNC_BUSY state, leading to false readings in, eg, get_prompt().
	 */
	while ((result = PQgetResult(pset.db)) != NULL && PQstatus(pset.db) != CONNECTION_BAD)
	{
		success = false;
		psql_error("\\copy: unexpected response (%d)\n",
				   PQresultStatus(result));
		/* if still in COPY IN state, try to get out of it */
		if (PQresultStatus(result) == PGRES_COPY_IN)
			PQputCopyEnd(pset.db, _("trying to exit copy mode"));
		PQclear(result);
	}

	if (result != NULL)
	{
		PQclear(result);
	}

	if (options->file != NULL)
	{
		if (options->program)
		{
			int pclose_rc = pclose(copystream);
			if (pclose_rc != 0)
			{
				if (pclose_rc < 0)
					psql_error("could not close pipe to external command: %s\n",
							   strerror(errno));
				else
				{
					char *reason = wait_result_to_str(pclose_rc);
					psql_error("%s: %s\n", options->file,
							   reason ? reason : "");
					if (reason)
						free(reason);
				}
				success = false;
			}
#ifndef WIN32
			pqsignal(SIGPIPE, SIG_DFL);
#endif
		}
		else
		{
			if (fclose(copystream) != 0)
			{
				psql_error("%s: %s\n", options->file, strerror(errno));
				success = false;
			}
		}
	}
	free_copy_options(options);
	return success;
}


/*
 * Functions for handling COPY IN/OUT data transfer.
 *
 * If you want to use COPY TO STDOUT/FROM STDIN in your application,
 * this is the code to steal ;)
 */

/*
 * handleCopyOut
 * receives data as a result of a COPY ... TO STDOUT command
 *
 * conn should be a database connection that you just issued COPY TO on
 * and got back a PGRES_COPY_OUT result.
 * copystream is the file stream for the data to go to.
 *
 * result is true if successful, false if not.
 */
bool
handleCopyOut(PGconn *conn, FILE *copystream)
{
	bool		OK = true;
	char	   *buf;
	int			ret;
	PGresult   *res;

	for (;;)
	{
		ret = PQgetCopyData(conn, &buf, 0);

		if (ret < 0)
			break;				/* done or error */

		if (buf)
		{
			if (fwrite(buf, 1, ret, copystream) != ret)
			{
				if (OK)			/* complain only once, keep reading data */
					psql_error("could not write COPY data: %s\n",
							   strerror(errno));
				OK = false;
			}
			PQfreemem(buf);
		}
	}

	if (OK && fflush(copystream))
	{
		psql_error("could not write COPY data: %s\n",
				   strerror(errno));
		OK = false;
	}

	if (ret == -2)
	{
		psql_error("COPY data transfer failed: %s", PQerrorMessage(conn));
		OK = false;
	}

	/* Check command status and return to normal libpq state */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		psql_error("%s", PQerrorMessage(conn));
		OK = false;
	}
	PQclear(res);

	return OK;
}

/*
 * handleCopyIn
 * sends data to complete a COPY ... FROM STDIN command
 *
 * conn should be a database connection that you just issued COPY FROM on
 * and got back a PGRES_COPY_IN result.
 * copystream is the file stream to read the data from.
 * isbinary can be set from PQbinaryTuples().
 *
 * result is true if successful, false if not.
 */

/* read chunk size for COPY IN - size is not critical */
#define COPYBUFSIZ 8192

bool
handleCopyIn(PGconn *conn, FILE *copystream, bool isbinary)
{
	bool		OK;
	const char *prompt;
	char		buf[COPYBUFSIZ];
	PGresult   *res;

	/*
	 * Establish longjmp destination for exiting from wait-for-input. (This is
	 * only effective while sigint_interrupt_enabled is TRUE.)
	 */
	if (sigsetjmp(sigint_interrupt_jmp, 1) != 0)
	{
		/* got here with longjmp */

		/* Terminate data transfer */
		PQputCopyEnd(conn, _("canceled by user"));

		/* Check command status and return to normal libpq state */
		res = PQgetResult(conn);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			psql_error("%s", PQerrorMessage(conn));
		PQclear(res);

		return false;
	}

	/* Prompt if interactive input */
	if (isatty(fileno(copystream)))
	{
		if (!pset.quiet)
			puts(_("Enter data to be copied followed by a newline.\n"
				   "End with a backslash and a period on a line by itself."));
		prompt = get_prompt(PROMPT_COPY);
	}
	else
		prompt = NULL;

	OK = true;

	if (isbinary)
	{
		/* interactive input probably silly, but give one prompt anyway */
		if (prompt)
		{
			fputs(prompt, stdout);
			fflush(stdout);
		}

		for (;;)
		{
			int			buflen;

			/* enable longjmp while waiting for input */
			sigint_interrupt_enabled = true;

			buflen = fread(buf, 1, COPYBUFSIZ, copystream);

			sigint_interrupt_enabled = false;

			if (buflen <= 0)
				break;

			if (PQputCopyData(conn, buf, buflen) <= 0)
			{
				OK = false;
				break;
			}
		}
	}
	else
	{
		bool		copydone = false;

		while (!copydone)
		{						/* for each input line ... */
			bool		firstload;
			bool		linedone;

			if (prompt)
			{
				fputs(prompt, stdout);
				fflush(stdout);
			}

			firstload = true;
			linedone = false;

			while (!linedone)
			{					/* for each bufferload in line ... */
				int			linelen;
				char	   *fgresult;

				/* enable longjmp while waiting for input */
				sigint_interrupt_enabled = true;

				fgresult = fgets(buf, sizeof(buf), copystream);

				sigint_interrupt_enabled = false;

				if (!fgresult)
				{
					copydone = true;
					break;
				}

				linelen = strlen(buf);

				/* current line is done? */
				if (linelen > 0 && buf[linelen - 1] == '\n')
					linedone = true;

				/* check for EOF marker, but not on a partial line */
				if (firstload)
				{
					if (strcmp(buf, "\\.\n") == 0 ||
						strcmp(buf, "\\.\r\n") == 0)
					{
						copydone = true;
						break;
					}

					firstload = false;
				}

				if (PQputCopyData(conn, buf, linelen) <= 0)
				{
					OK = false;
					copydone = true;
					break;
				}
			}

			if (copystream == pset.cur_cmd_source)
				pset.lineno++;
		}
	}

	/* Check for read error */
	if (ferror(copystream))
		OK = false;

	/* Terminate data transfer */
	if (PQputCopyEnd(conn,
					 OK ? NULL : _("aborted because of read failure")) <= 0)
		OK = false;

	/* Check command status and return to normal libpq state */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		psql_error("%s", PQerrorMessage(conn));
		OK = false;
	}
	PQclear(res);

	return OK;
}
