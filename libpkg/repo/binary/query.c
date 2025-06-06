/*
 * Copyright (c) 2014, Vsevolod Stakhov
 * Copyright (c) 2024, Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2023, Serenity Cyber Security, LLC
 *                     Author: Gleb Popov <arrowd@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <errno.h>
#include <regex.h>
#include <grp.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>
#include <fnmatch.h>

#include <sqlite3.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkgdb.h"
#include "private/utils.h"
#include "binary.h"

static struct pkg_repo_it* pkg_repo_binary_it_new(struct pkg_repo *repo,
	sqlite3_stmt *s, short flags);

struct pkg_repo_group {
	size_t index;
	ucl_object_t *groups;
};
static int pkg_repo_binary_it_next(struct pkg_repo_it *it, struct pkg **pkg_p, unsigned flags);
static void pkg_repo_binary_it_free(struct pkg_repo_it *it);
static void pkg_repo_binary_it_reset(struct pkg_repo_it *it);

static int pkg_repo_binary_group_it_next(struct pkg_repo_it *it, struct pkg **pkg_p, unsigned flags);
static void pkg_repo_binary_group_it_free(struct pkg_repo_it *it);
static void pkg_repo_binary_group_it_reset(struct pkg_repo_it *it);

static struct pkg_repo_it_ops pkg_repo_binary_it_ops = {
	.next = pkg_repo_binary_it_next,
	.free = pkg_repo_binary_it_free,
	.reset = pkg_repo_binary_it_reset
};

static struct pkg_repo_it_ops pkg_repo_binary_group_it_ops = {
	.next = pkg_repo_binary_group_it_next,
	.free = pkg_repo_binary_group_it_free,
	.reset = pkg_repo_binary_group_it_reset
};

static struct pkg_repo_it*
pkg_repo_binary_it_new(struct pkg_repo *repo, sqlite3_stmt *s, short flags)
{
	struct pkg_repo_it *it;
	struct pkgdb fakedb;

	it = xmalloc(sizeof(*it));

	it->ops = &pkg_repo_binary_it_ops;
	it->flags = flags;
	it->repo = repo;

	fakedb.sqlite = PRIV_GET(repo);
	it->data = pkgdb_it_new_sqlite(&fakedb, s, PKG_REMOTE, flags);

	if (it->data == NULL) {
		free(it);
		return (NULL);
	}

	return (it);
}

static struct pkg_repo_it *
pkg_repo_binary_group_it_new(struct pkg_repo *repo, ucl_object_t *matching)
{
	struct pkg_repo_group *prg;
	struct pkg_repo_it *it;

	it = xcalloc(1, sizeof(*it));
	prg = xcalloc(1, sizeof(*prg));
	prg->groups = matching;
	it->repo = repo;
	it->ops = &pkg_repo_binary_group_it_ops;
	it->data = prg;

	return (it);
}

static int
pkg_repo_binary_it_next(struct pkg_repo_it *it, struct pkg **pkg_p, unsigned flags)
{
	return (pkgdb_it_next(it->data, pkg_p, flags));
}

static int
pkg_repo_binary_group_it_next(struct pkg_repo_it *it, struct pkg **pkg_p, unsigned flags __unused)
{
	int ret;
	struct pkg_repo_group *prg;
	const ucl_object_t *o, *el;

	prg = it->data;
	if (prg->index == ucl_array_size(prg->groups))
		return (EPKG_END);

	el = ucl_array_find_index(prg->groups, prg->index);
	prg->index++;
	pkg_free(*pkg_p);
	if ((ret = pkg_new(pkg_p, PKG_GROUP_REMOTE)) != EPKG_OK)
		return (ret);
	o = ucl_object_find_key(el, "name");
	xasprintf(&(*pkg_p)->name, ucl_object_tostring(o));
	xasprintf(&(*pkg_p)->uid, "@%s", (*pkg_p)->name);
	o = ucl_object_find_key(el, "comment");
	xasprintf(&(*pkg_p)->comment, ucl_object_tostring(o));
	pkg_kv_add(&(*pkg_p)->annotations, "repository",   it->repo->name, "annotation");

	return (EPKG_OK);
}

static void
pkg_repo_binary_it_free(struct pkg_repo_it *it)
{
	pkgdb_it_free(it->data);
	free(it);
}

static void
pkg_repo_binary_group_it_free(struct pkg_repo_it *it)
{
	struct pkg_repo_group *prg = it->data;
	free(prg->groups);
	free(prg);
	free(it);
}

static void
pkg_repo_binary_it_reset(struct pkg_repo_it *it)
{
	pkgdb_it_reset(it->data);
}

static void
pkg_repo_binary_group_it_reset(struct pkg_repo_it *it)
{
	struct pkg_repo_group *prg = it->data;

	prg->index = 0;
}

struct pkg_repo_it *
pkg_repo_binary_groupquery(struct pkg_repo *repo, const char *pattern, match_t match)
{
	return (pkg_repo_binary_groupsearch(repo, pattern, match, FIELD_NAME));
}

struct pkg_repo_it *
pkg_repo_binary_query(struct pkg_repo *repo, const char *cond, const char *pattern, match_t match)
{
	sqlite3 *sqlite = PRIV_GET(repo);
	sqlite3_stmt	*stmt = NULL;
	char *sql = NULL;
	const char	*comp = NULL;
	const char basesql_quick[] = ""
		"SELECT DISTINCT(p.id), origin, p.name, p.name as uniqueid, version, comment, "
		"prefix, desc, arch, maintainer, www, "
		"licenselogic, flatsize, pkgsize, "
		"cksum, manifestdigest, path AS repopath, '%s' AS dbname "
		"FROM packages  as p "
		" %s "
		"%s%s%s "
		"ORDER BY p.name;";
	const char basesql[] = ""
		"WITH flavors AS "
		"  (SELECT package_id, value.annotation AS flavor FROM pkg_annotation "
		"   LEFT JOIN annotation tag ON pkg_annotation.tag_id = tag.annotation_id "
		"   LEFT JOIN annotation value ON pkg_annotation.value_id = value.annotation_id "
		"   WHERE tag.annotation = 'flavor') "

		"SELECT DISTINCT(p.id), origin, p.name, p.name as uniqueid, version, comment, "
		"prefix, desc, arch, maintainer, www, "
		"licenselogic, flatsize, pkgsize, "
		"cksum, manifestdigest, path AS repopath, '%s' AS dbname "
		"FROM packages  as p "
		"LEFT JOIN pkg_categories ON p.id = pkg_categories.package_id "
		"LEFT JOIN categories ON categories.id = pkg_categories.category_id "
		"LEFT JOIN flavors ON flavors.package_id = p.id "
		" %s "
		"%s%s%s "
		"ORDER BY p.name;";

	const char *bsql = (match == MATCH_INTERNAL) ? basesql_quick : basesql;

	if (match != MATCH_ALL && (pattern == NULL || pattern[0] == '\0'))
		return (NULL);

	comp = pkgdb_get_pattern_query(pattern, match);
	if (comp == NULL)
		comp = "";
	if (cond == NULL)
		xasprintf(&sql, bsql, repo->name, comp, "", "", "");
	else
		xasprintf(&sql, bsql, repo->name, comp,
		    comp[0] != '\0' ? "AND (" : "WHERE ( ", cond + 7, " )");

	stmt = prepare_sql(sqlite, sql);
	free(sql);
	if (stmt == NULL)
		return (NULL);

	if (match != MATCH_ALL)
		sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
	pkgdb_debug(4, stmt);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

struct pkg_repo_it *
pkg_repo_binary_shlib_provide(struct pkg_repo *repo, const char *require)
{
	sqlite3_stmt	*stmt;
	sqlite3 *sqlite = PRIV_GET(repo);
	char *sql = NULL;
	const char	 basesql[] = ""
			"SELECT p.id, p.origin, p.name, p.version, p.comment, "
			"p.name as uniqueid, "
			"p.prefix, p.desc, p.arch, p.maintainer, p.www, "
			"p.licenselogic, p.flatsize, p.pkgsize, "
			"p.cksum, p.manifestdigest, p.path AS repopath, '%s' AS dbname "
			"FROM packages AS p INNER JOIN pkg_shlibs_provided AS ps ON "
			"p.id = ps.package_id "
			"WHERE ps.shlib_id IN (SELECT id FROM shlibs WHERE "
			"name BETWEEN ?1 AND ?1 || '.9');";

	xasprintf(&sql, basesql, repo->name);

	stmt = prepare_sql(sqlite, sql);
	free(sql);
	if (stmt == NULL)
		return (NULL);

	sqlite3_bind_text(stmt, 1, require, -1, SQLITE_TRANSIENT);
	pkgdb_debug(4, stmt);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

struct pkg_repo_it *
pkg_repo_binary_provide(struct pkg_repo *repo, const char *require)
{
	sqlite3_stmt	*stmt;
	sqlite3 *sqlite = PRIV_GET(repo);
	char *sql = NULL;
	const char	 basesql[] = ""
			"SELECT p.id, p.origin, p.name, p.version, p.comment, "
			"p.name as uniqueid, "
			"p.prefix, p.desc, p.arch, p.maintainer, p.www, "
			"p.licenselogic, p.flatsize, p.pkgsize, "
			"p.cksum, p.manifestdigest, p.path AS repopath, '%s' AS dbname "
			"FROM packages AS p INNER JOIN pkg_provides AS ps ON "
			"p.id = ps.package_id "
			"WHERE ps.provide_id IN (SELECT id from provides WHERE "
			"provide = ?1 );";

	xasprintf(&sql, basesql, repo->name);

	stmt = prepare_sql(sqlite, sql);
	free(sql);
	if (stmt == NULL)
		return (NULL);

	sqlite3_bind_text(stmt, 1, require, -1, SQLITE_TRANSIENT);
	pkgdb_debug(4, stmt);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

struct pkg_repo_it *
pkg_repo_binary_shlib_require(struct pkg_repo *repo, const char *provide)
{
	sqlite3_stmt	*stmt;
	sqlite3 *sqlite = PRIV_GET(repo);
	char *sql = NULL;
	const char	 basesql[] = ""
			"SELECT p.id, p.origin, p.name, p.version, p.comment, "
			"p.name as uniqueid, "
			"p.prefix, p.desc, p.arch, p.maintainer, p.www, "
			"p.licenselogic, p.flatsize, p.pkgsize, "
			"p.cksum, p.manifestdigest, p.path AS repopath, '%s' AS dbname "
			"FROM packages AS p INNER JOIN pkg_shlibs_required AS ps ON "
			"p.id = ps.package_id "
			"WHERE ps.shlib_id = (SELECT id FROM shlibs WHERE name=?1);";

	xasprintf(&sql, basesql, repo->name);

	stmt = prepare_sql(sqlite, sql);
	free(sql);
	if (stmt == NULL)
		return (NULL);

	pkg_debug(1, "> loading provides");
	sqlite3_bind_text(stmt, 1, provide, -1, SQLITE_TRANSIENT);
	pkgdb_debug(4, stmt);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

struct pkg_repo_it *
pkg_repo_binary_require(struct pkg_repo *repo, const char *provide)
{
	sqlite3_stmt	*stmt;
	sqlite3 *sqlite = PRIV_GET(repo);
	char *sql = NULL;
	const char	 basesql[] = ""
			"SELECT p.id, p.origin, p.name, p.version, p.comment, "
			"p.name as uniqueid, "
			"p.prefix, p.desc, p.arch, p.maintainer, p.www, "
			"p.licenselogic, p.flatsize, p.pkgsize, "
			"p.cksum, p.manifestdigest, p.path AS repopath, '%s' AS dbname "
			"FROM packages AS p INNER JOIN pkg_requires AS ps ON "
			"p.id = ps.package_id "
			"WHERE ps.require_id = (SELECT id FROM requires WHERE require=?1);";

	xasprintf(&sql, basesql, repo->name);

	stmt = prepare_sql(sqlite, sql);
	free(sql);
	if (stmt == NULL)
		return (NULL);

	sqlite3_bind_text(stmt, 1, provide, -1, SQLITE_TRANSIENT);
	pkgdb_debug(4, stmt);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

static const char *
pkg_repo_binary_search_how(match_t match)
{
	const char	*how = NULL;

	switch (match) {
	case MATCH_ALL:
		how = "TRUE";
		break;
	case MATCH_INTERNAL:
		how = "%s = ?1";
		break;
	case MATCH_EXACT:
		if (pkgdb_case_sensitive())
			how = "%s = ?1";
		else
			how = "%s = ?1 COLLATE NOCASE";
		break;
	case MATCH_GLOB:
		if (pkgdb_case_sensitive())
			how = "%s GLOB ?1";
		else
			how = "%s GLOB ?1 COLLATE NOCASE";
		break;
	case MATCH_REGEX:
		how = "%s REGEXP ?1";
		break;
	}

	return (how);
}

static int
pkg_repo_binary_build_search_query(xstring *sql, match_t match,
    pkgdb_field field, pkgdb_field sort)
{
	const char	*how;
	const char	*what = NULL;
	const char	*orderby = NULL;

	how = pkg_repo_binary_search_how(match);

	switch (field) {
	case FIELD_NONE:
		what = NULL;
		break;
	case FIELD_ORIGIN:
		what = "categories.name || substr(origin, instr(origin, '/'))";
		break;
	case FIELD_FLAVOR:
		what = "categories.name || substr(origin, instr(origin, '/')) || '@' || flavor";
		break;
	case FIELD_NAME:
		what = "p.name";
		break;
	case FIELD_NAMEVER:
		what = "p.name || '-' || version";
		break;
	case FIELD_COMMENT:
		what = "comment";
		break;
	case FIELD_DESC:
		what = "desc";
		break;
	}

	if (what != NULL && how != NULL)
		fprintf(sql->fp, how, what);

	switch (sort) {
	case FIELD_NONE:
		orderby = NULL;
		break;
	case FIELD_ORIGIN:
		orderby = " ORDER BY origin";
		break;
	case FIELD_FLAVOR:
		orderby = " ORDER BY p.name";
	case FIELD_NAME:
		orderby = " ORDER BY p.name";
		break;
	case FIELD_NAMEVER:
		orderby = " ORDER BY p.name, version";
		break;
	case FIELD_COMMENT:
		orderby = " ORDER BY comment";
		break;
	case FIELD_DESC:
		orderby = " ORDER BY desc";
		break;
	}

	if (orderby != NULL)
		fprintf(sql->fp, "%s", orderby);

	return (EPKG_OK);
}

struct pkg_repo_it *
pkg_repo_binary_search(struct pkg_repo *repo, const char *pattern, match_t match,
    pkgdb_field field, pkgdb_field sort)
{
	sqlite3 *sqlite = PRIV_GET(repo);
	sqlite3_stmt	*stmt = NULL;
	xstring	*sql = NULL;
	char *sqlcmd = NULL;
	const char	*multireposql = ""
		"WITH flavors AS "
		"  (SELECT package_id, value.annotation AS flavor FROM pkg_annotation "
		"   LEFT JOIN annotation tag ON pkg_annotation.tag_id = tag.annotation_id "
		"   LEFT JOIN annotation value ON pkg_annotation.value_id = value.annotation_id "
		"   WHERE tag.annotation = 'flavor') "
		"SELECT DISTINCT p.id, origin, p.name, version, comment, "
		"prefix, desc, arch, maintainer, www, "
		"licenselogic, flatsize, pkgsize, "
		"cksum, path AS repopath, '%1$s' AS dbname, '%2$s' AS repourl "
		"FROM packages  as p "
		"LEFT JOIN pkg_categories ON p.id = pkg_categories.package_id "
		"LEFT JOIN categories ON categories.id = pkg_categories.category_id "
		"LEFT JOIN flavors ON flavors.package_id = p.id ";

	if (match != MATCH_ALL && (pattern == NULL || pattern[0] == '\0'))
		return (NULL);

	sql = xstring_new();
	fprintf(sql->fp, multireposql, repo->name, repo->url);

	/* close the UNIONs and build the search query */
	fprintf(sql->fp, "%s", "WHERE ");

	pkg_repo_binary_build_search_query(sql, match, field, sort);
	fprintf(sql->fp, "%s", ";");
	sqlcmd = xstring_get(sql);

	stmt = prepare_sql(sqlite, sqlcmd);
	free(sqlcmd);
	if (stmt == NULL)
		return (NULL);

	sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
	pkgdb_debug(4, stmt);

	return (pkg_repo_binary_it_new(repo, stmt, PKGDB_IT_FLAG_ONCE));
}

struct pkg_repo_it *
pkg_repo_binary_groupsearch(struct pkg_repo *repo, const char *pattern, match_t match,
    pkgdb_field field)
{
	ucl_object_t *groups, *ar, *el;
	const ucl_object_t *o;
	const char *cmp;
	struct ucl_parser *p;
	int fd;
	regex_t *re = NULL;
	int flag = 0;
	bool in_comment = false;
	bool start_with = false;

	switch (field) {
		case FIELD_NAME:
		case FIELD_NAMEVER:
			break;
		case FIELD_COMMENT:
			in_comment = true;
			break;
		default:
			/* we cannot search in other fields */
			return (NULL);
	}

	if (repo->dfd == -1 && pkg_repo_open(repo) == EPKG_FATAL)
		return (NULL);
	fd = openat(repo->dfd, "groups.ucl", O_RDONLY|O_CLOEXEC);
	if (fd == -1)
		return (NULL);
	p = ucl_parser_new(0);
	if (!ucl_parser_add_fd(p, fd)) {
		pkg_emit_error("Error parsing groups for: %s'",
		    repo->name);
		ucl_parser_free(p);
		close(fd);
		return (NULL);

	}
	groups = ucl_parser_get_object(p);
	ucl_parser_free(p);
	close(fd);

	if (ucl_object_type(groups) != UCL_ARRAY) {
		ucl_object_unref(groups);
		return (NULL);
	}
	if (*pattern == '@') {
		pattern++;
		start_with = true;
	}

	ar = NULL;
	while (ucl_array_size(groups) > 0) {
		el = ucl_array_pop_first(groups);
		if (in_comment) {
			o = ucl_object_find_key(el, "comment");
		} else {
			o = ucl_object_find_key(el, "name");
		}
		if (o == NULL) {
			ucl_object_unref(el);
			continue;
		}
		cmp = ucl_object_tostring(o);
		switch (match) {
		case MATCH_ALL:
			break;
		case MATCH_INTERNAL:
			if (strcmp(cmp, pattern) != 0)
				continue;
			break;
		case MATCH_EXACT:
			if (pkgdb_case_sensitive()) {
				if (strcmp(cmp, pattern) != 0)
					continue;
			} else {
				if (strcasecmp(cmp, pattern) != 0)
					continue;
			}
			break;
		case MATCH_GLOB:
			if (pkgdb_case_sensitive() != 0)
				flag = FNM_CASEFOLD;
			if (fnmatch(cmp, pattern, flag) == FNM_NOMATCH)
				continue;
		case MATCH_REGEX:
			if (re == NULL) {
				char *newpattern = NULL;
				const char *pat = pattern;
				flag = REG_EXTENDED | REG_NOSUB;
				if (pkgdb_case_sensitive() != 0)
					flag |= REG_ICASE;
				re = xmalloc(sizeof(regex_t));
				if (start_with) {
					xasprintf(&newpattern, "^%s", pattern);
					pat = newpattern;
				}
				if (regcomp(re, pat, flag) != 0) {
					pkg_emit_error("Invalid regex: 'pattern'");
					ucl_object_unref(groups);
					if (ar != NULL)
						ucl_object_unref(ar);
					free(newpattern);
					return (NULL);
				}
				free(newpattern);
			}
			if (regexec(re, cmp, 0, NULL, 0) == REG_NOMATCH)
				continue;
		}
		if (ar == NULL)
			ar = ucl_object_typed_new(UCL_ARRAY);
		ucl_array_append(ar, el);
	}

	if (re != NULL)
		regfree(re);
	ucl_object_unref(groups);

	if (ar == NULL)
		return (NULL);

	return (pkg_repo_binary_group_it_new(repo, ar));
}

int
pkg_repo_binary_ensure_loaded(struct pkg_repo *repo,
	struct pkg *pkg, unsigned flags)
{
	sqlite3 *sqlite = PRIV_GET(repo);
	struct pkg *cached = NULL;
	char path[MAXPATHLEN];
	int rc;

	flags &= PKG_LOAD_FILES|PKG_LOAD_DIRS;
	/*
	 * If info is already present, done.
	 */
	if ((pkg->flags & flags) == flags) {
		return EPKG_OK;
	}
	if (pkg->type == PKG_INSTALLED) {
		pkg_emit_error("cached package %s-%s: "
			       "attempting to load info from an installed package",
			       pkg->name, pkg->version);
		return EPKG_FATAL;

		/* XXX If package is installed, get info from SQLite ???  */
		rc = pkgdb_ensure_loaded_sqlite(sqlite, pkg, flags);
		if (rc != EPKG_OK) {
			return rc;
		}
		/* probably unnecessary */
		if ((pkg->flags & flags) != flags) {
			return EPKG_FATAL;
		}
		return rc;
	}
	/*
	 * Try to get that information from fetched package in cache
	 */

	if (pkg_repo_cached_name(pkg, path, sizeof(path)) != EPKG_OK)
		return (EPKG_FATAL);

	pkg_debug(1, "Binary> loading %s", path);
	if (pkg_open(&cached, path, PKG_OPEN_TRY) != EPKG_OK) {
		pkg_free(cached);
		return EPKG_FATAL;
	}

	/* Now move required elements to the provided package */
	pkg_list_free(pkg, PKG_FILES);
	pkg_list_free(pkg, PKG_CONFIG_FILES);
	pkg_list_free(pkg, PKG_DIRS);
	pkg->files = cached->files;
	pkg->filehash = cached->filehash;
	pkg->config_files = cached->config_files;
	pkg->config_files_hash = cached->config_files_hash;
	pkg->dirs = cached->dirs;
	pkg->dirhash = cached->dirhash;
	cached->files = NULL;
	cached->filehash = NULL;
	cached->config_files = NULL;
	cached->config_files_hash = NULL;
	cached->dirs = NULL;
	cached->dirhash = NULL;

	pkg_free(cached);
	pkg->flags |= flags;

	return EPKG_OK;
}

int64_t
pkg_repo_binary_stat(struct pkg_repo *repo, pkg_stats_t type)
{
	sqlite3 *sqlite = PRIV_GET(repo);
	sqlite3_stmt	*stmt = NULL;
	int64_t		 stats = 0;
	const char *sql = NULL;

	switch(type) {
	case PKG_STATS_LOCAL_COUNT:
	case PKG_STATS_REMOTE_REPOS:
	case PKG_STATS_LOCAL_SIZE:
		return (stats);
	case PKG_STATS_REMOTE_UNIQUE:
		sql = "SELECT COUNT(id) FROM main.packages;";
		break;
	case PKG_STATS_REMOTE_COUNT:
		sql = "SELECT COUNT(id) FROM main.packages;";
		break;
	case PKG_STATS_REMOTE_SIZE:
		sql = "SELECT SUM(pkgsize) FROM main.packages;";
		break;
	}

	pkg_debug(4, "binary_repo: running '%s'", sql);
	stmt = prepare_sql(sqlite, sql);

	if (stmt == NULL)
		return (stats);

	while (sqlite3_step(stmt) != SQLITE_DONE) {
		stats = sqlite3_column_int64(stmt, 0);
	}

	sqlite3_finalize(stmt);

	return (stats);
}
