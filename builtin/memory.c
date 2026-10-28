#include "builtin.h"
#include "memory.h"
#include "gettext.h"
#include "hex.h"
#include "json-writer.h"
#include "object-name.h"
#include "parse-options.h"
#include "path.h"
#include "utf8.h"
#include "strvec.h"
#include "setup.h"

static const char *const memory_usage[] = {
	N_("git memory <init|capture|context|history|diff|note|status|watch> [options] [-- paths]"),
	NULL
};

static int valid_text(const char *text, size_t limit)
{
	return !text || (strlen(text) <= limit && is_utf8(text));
}

static void capture_output(const struct object_id *oid, int changed)
{
	printf("{\"schema\":1,\"snapshot\":\"%s\",\"changed\":%s}\n",
	       oid_to_hex(oid), changed ? "true" : "false");
}

static int initialize_scopes(struct memory_store *m, struct repository *repo,
			     const char *prefix, int argc, const char **argv,
			     struct strbuf *err)
{
	struct strvec scopes = STRVEC_INIT;
	int i, ret = -1;
	if (!argc || repo_read_index(repo) < 0)
		return memory_error(m, err, "SCOPE", NULL);
	for (i = 0; i < argc; i++) {
		char *path = prefix_path(repo, prefix, prefix ? strlen(prefix) : 0, argv[i]);
		size_t len = strlen(path);
		unsigned j;
		int found = 0;
		while (len && path[len - 1] == '/')
			path[--len] = 0;
		if (!len) { free(path); path = xstrdup("."); len = 1; }
		if (strpbrk(path, "*?[]") ||
		    (strcmp(path, ".") && !verify_path(path, S_IFREG))) {
			free(path);
			memory_error(m, err, "SCOPE", argv[i]);
			goto done;
		}
		for (j = 0; j < repo->index->cache_nr; j++) {
			const char *name = repo->index->cache[j]->name;
			if (!strcmp(path, ".") || !strcmp(path, name) ||
			    (starts_with(name, path) && name[len] == '/'))
				found = 1;
		}
		if (!found) {
			memory_error(m, err, "SCOPE", path);
			free(path);
			goto done;
		}
		strvec_push_nodup(&scopes, path);
	}
	ret = memory_initialize(m, repo, scopes.nr, scopes.v, err);
	if (!ret)
		puts("{\"schema\":1,\"enabled\":true,\"retention\":\"explicit_raw_worktree_and_index\"}");
done:
	strvec_clear(&scopes);
	return ret;
}

static int resolve_snapshot(struct memory_store *m, const char *name,
			    struct object_id *oid, struct strbuf *err)
{
	if (!name) {
		int exists = memory_ref(m, MEMORY_SNAPSHOTS, oid);
		if (exists < 0)
			return memory_error(m, err, "OBJECT", MEMORY_SNAPSHOTS);
		return exists ? 0 : memory_error(m, err, "NO_SNAPSHOT", NULL);
	}
	if (strlen(name) != m->repo.hash_algo->hexsz ||
	    get_oid_hex_algop(name, oid, m->repo.hash_algo))
		return memory_error(m, err, "OPTIONS", "full_snapshot_oid");
	return 0;
}

static int watch_sources(struct memory_store *m, const struct memory_attribution *who,
			 int interval, int count, struct strbuf *err)
{
	struct lock_file watch = LOCK_INIT;
	struct object_id snapshot;
	char *path = xstrfmt("%s/watch", m->path);
	int i = 0, changed, ret = -1;
	if (hold_lock_file_for_update_mode(&watch, path, 0, 0600) < 0) {
		free(path);
		return memory_error(m, err, "LOCK", "watcher");
	}
	free(path);
	do {
		if (memory_capture(m, who, &snapshot, &changed, err))
			goto done;
		if (changed) { capture_output(&snapshot, changed); fflush(stdout); }
		i++;
		if (count && i >= count)
			break;
		sleep_millisec(interval);
	} while (1);
	ret = 0;
done:
	rollback_lock_file(&watch);
	return ret;
}

static int memory_status(struct memory_store *m, struct strbuf *out, struct strbuf *err)
{
	struct json_writer jw = JSON_WRITER_INIT;
	struct object_id tip;
	struct string_list_item *scope;
	int exists = memory_ref(m, MEMORY_SNAPSHOTS, &tip);
	if (exists < 0)
		return memory_error(m, err, "OBJECT", MEMORY_SNAPSHOTS);
	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, "schema", 1);
	jw_object_true(&jw, "enabled");
	jw_object_string(&jw, "store", m->path);
	jw_object_string(&jw, "snapshot", exists ? oid_to_hex(&tip) : "none");
	jw_object_string(&jw, "source_freshness", "not_checked");
	jw_object_string(&jw, "capture_mode", "explicit_or_context_refresh_or_running_watch");
	jw_object_string(&jw, "hash_format", m->repo.hash_algo->name);
	jw_object_intmax(&jw, "max_file_bytes", m->max_file);
	jw_object_intmax(&jw, "max_snapshot_bytes", m->max_total);
	jw_object_intmax(&jw, "max_files", m->max_files);
	jw_object_inline_begin_array(&jw, "scope");
	for_each_string_list_item(scope, &m->scopes)
		jw_array_string(&jw, scope->string);
	jw_end(&jw);
	jw_end(&jw);
	strbuf_addbuf(out, &jw.json);
	strbuf_addch(out, '\n');
	jw_release(&jw);
	return 0;
}

int cmd_memory(int argc, const char **argv, const char *prefix, struct repository *repo)
{
	struct memory_store m = { 0 };
	struct memory_attribution who = { 0 };
	struct strbuf out = STRBUF_INIT, err = STRBUF_INIT;
	struct object_id snapshot, from, note;
	const char *operation, *language = "en", *query = "", *text = NULL;
	const char *from_name = NULL, *to_name = NULL, *at_name = NULL;
	int worktree = 0, no_refresh = 0, max_bytes = MEMORY_CONTEXT_BYTES;
	int limit = MEMORY_HISTORY_LIMIT, interval = 1000, count = 0, changed, ret = -1;
	struct option options[] = {
		OPT_BOOL(0, "worktree", &worktree, N_("consent to retaining selected raw worktree content")),
		OPT_BOOL(0, "no-refresh", &no_refresh, N_("use the retained snapshot without reading live source")),
		OPT_STRING(0, "language", &language, "en|ar", N_("diagnostic language")),
		OPT_STRING(0, "query", &query, "text", N_("literal path or content query")),
		OPT_STRING(0, "actor", &who.actor, "id", N_("declared actor, not authenticated")),
		OPT_STRING(0, "model", &who.model, "id", N_("declared model, not authenticated")),
		OPT_STRING(0, "session", &who.session, "id", N_("declared session identifier")),
		OPT_STRING(0, "task", &who.task, "id", N_("declared task identifier")),
		OPT_STRING(0, "text", &text, "text", N_("untrusted candidate note text")),
		OPT_STRING(0, "snapshot", &at_name, "oid", N_("source snapshot originally used for a note")),
		OPT_STRING(0, "from", &from_name, "oid", N_("previous memory snapshot")),
		OPT_STRING(0, "to", &to_name, "oid", N_("next memory snapshot")),
		OPT_INTEGER(0, "max-bytes", &max_bytes, N_("exact serialized context byte budget")),
		OPT_INTEGER(0, "limit", &limit, N_("history event limit, 1 through 64")),
		OPT_INTEGER(0, "interval-ms", &interval, N_("watch interval, 100 through 60000 milliseconds")),
		OPT_INTEGER(0, "count", &count, N_("watch cycles; zero runs until interrupted")),
		OPT_END()
	};
	if (argc < 2)
		usage_with_options(memory_usage, options);
	operation = argv[1];
	argc--;
	argv++;
	argc = parse_options(argc, argv, prefix, options, memory_usage, 0);
	m.arabic = !strcmp(language, "ar");
	if ((strcmp(language, "ar") && strcmp(language, "en")) ||
	    !valid_text(query, 256) || !valid_text(who.actor, 256) ||
	    !valid_text(who.model, 256) || !valid_text(who.session, 256) ||
	    !valid_text(who.task, 256) || max_bytes < 1024 || (unsigned long)max_bytes > MEMORY_MAX_CONTEXT ||
	    limit < 1 || limit > MEMORY_HISTORY_LIMIT || interval < 100 || interval > 60000 || count < 0) {
		memory_error(&m, &err, "OPTIONS", "bounds_or_encoding");
		goto done;
	}
	if (!strcmp(operation, "init")) {
		if (!worktree) memory_error(&m, &err, "OPTIONS", "--worktree");
		else ret = initialize_scopes(&m, repo, prefix, argc, argv, &err);
		goto done;
	}
	if (memory_open(&m, repo, &err))
		goto done;
	if (argc && strcmp(operation, "note")) {
		memory_error(&m, &err, "OPTIONS", "unexpected_paths");
		goto done;
	}
	if (!strcmp(operation, "capture")) {
		ret = memory_capture(&m, &who, &snapshot, &changed, &err);
		if (!ret) capture_output(&snapshot, changed);
	} else if (!strcmp(operation, "watch")) {
		ret = watch_sources(&m, &who, interval, count, &err);
	} else if (!strcmp(operation, "status")) {
		ret = memory_status(&m, &out, &err);
	} else if (!strcmp(operation, "history")) {
		ret = memory_history(&m, MEMORY_SNAPSHOTS, limit, &out, &err);
	} else if (!strcmp(operation, "context")) {
		if ((!no_refresh && memory_capture(&m, &who, &snapshot, &changed, &err)) ||
		    (no_refresh && resolve_snapshot(&m, NULL, &snapshot, &err)))
			goto done;
		ret = memory_context(&m, query, max_bytes, &snapshot, &out, &err);
	} else if (!strcmp(operation, "note")) {
		if (!text || resolve_snapshot(&m, at_name, &snapshot, &err))
			goto done;
		ret = memory_note(&m, &snapshot, text, argc, argv, &who, &note, &err);
		if (!ret) strbuf_addf(&out, "{\"schema\":1,\"note\":\"%s\"}\n", oid_to_hex(&note));
	} else if (!strcmp(operation, "diff")) {
		if (resolve_snapshot(&m, to_name, &snapshot, &err))
			goto done;
		if (from_name) ret = resolve_snapshot(&m, from_name, &from, &err);
		else ret = memory_parent(&m, &snapshot, &from, &err) == 1 ? 0 :
			memory_error(&m, &err, "NO_SNAPSHOT", "previous");
		if (!ret) ret = memory_diff(&m, &from, &snapshot, &out, &err);
	} else {
		memory_error(&m, &err, "OPTIONS", operation);
	}
done:
	if (ret && !err.len)
		memory_error(&m, &err, "OPTIONS", operation);
	if (ret) error("%s", err.buf);
	else if (out.len) fwrite(out.buf, 1, out.len, stdout);
	memory_close(&m);
	strbuf_release(&out);
	strbuf_release(&err);
	return ret ? 1 : 0;
}
