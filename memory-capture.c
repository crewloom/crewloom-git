#include "git-compat-util.h"
#include "memory.h"
#include "hex.h"
#include "json-writer.h"
#include "object-file.h"
#include "object-name.h"
#include "odb.h"
#include "statinfo.h"
#include "utf8.h"
#include "promisor-remote.h"
#include "config.h"

struct observation {
	struct stat st;
	int missing;
};

static int selected(struct memory_store *m, const char *path)
{
	struct string_list_item *item;
	for_each_string_list_item(item, &m->scopes) {
		size_t len = strlen(item->string);
		if (!strcmp(item->string, ".") || !strcmp(item->string, path) ||
		    (starts_with(path, item->string) && path[len] == '/'))
			return 1;
	}
	return 0;
}

static int sensitive(const char *path)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	return !strcmp(base, ".env") || starts_with(base, ".env.") ||
		ends_with(base, ".pem") || ends_with(base, ".key") ||
		!strcmp(base, "id_rsa") || !strcmp(base, "id_ed25519");
}

static int same_stat(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
		a->st_mode == b->st_mode && a->st_size == b->st_size &&
		a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime &&
		ST_MTIME_NSEC(*a) == ST_MTIME_NSEC(*b) &&
		ST_CTIME_NSEC(*a) == ST_CTIME_NSEC(*b);
}

/* Resolve each parent without following symlinks outside the selected tree. */
static int parent_fd(struct memory_store *m, const char *path, char **leaf,
		     struct strbuf *err)
{
	char *copy = xstrdup(path), *p = copy, *slash;
	int fd = open(repo_get_work_tree(m->source), O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		goto fail;
	while ((slash = strchr(p, '/'))) {
		int next;
		*slash = 0;
		next = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		close(fd);
		fd = next;
		if (fd < 0)
			goto fail;
		p = slash + 1;
	}
	*leaf = xstrdup(p);
	free(copy);
	return fd;
fail:
	free(copy);
	if (errno == ENOENT)
		return -2;
	memory_error(m, err, "IO", path);
	return -1;
}

static int raw_file(struct memory_store *m, const char *path, struct strbuf *data,
		    unsigned *mode, int *missing, struct stat *observed, struct strbuf *err)
{
	char *leaf = NULL;
	struct stat before, after;
	int parent = parent_fd(m, path, &leaf, err), fd = -1, ret = -1;
	*missing = 0;
	if (parent < 0) {
		if (parent == -2) { *missing = 1; return 0; }
		return -1;
	}
	if (fstatat(parent, leaf, &before, AT_SYMLINK_NOFOLLOW)) {
		if (errno == ENOENT) {
			*missing = 1;
			ret = 0;
		} else {
			memory_error(m, err, "IO", path);
		}
		goto done;
	}
	if ((!S_ISREG(before.st_mode) && !S_ISLNK(before.st_mode)) ||
	    (unsigned long long)before.st_size > m->max_file) {
		memory_error(m, err, S_ISREG(before.st_mode) ? "LIMIT" : "UNSUPPORTED", path);
		goto done;
	}
	if (S_ISLNK(before.st_mode)) {
		ssize_t len;
		strbuf_grow(data, m->max_file + 1);
		len = readlinkat(parent, leaf, data->buf, m->max_file + 1);
		if (len < 0 || (unsigned long)len > m->max_file)
			goto io_error;
		strbuf_setlen(data, len);
		*mode = S_IFLNK;
	} else {
		fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW);
		if (fd < 0 || fstat(fd, &after) || !same_stat(&before, &after))
			goto conflict;
		/* A bounded read never consumes an unexpectedly growing file forever. */
		while (data->len <= m->max_file) {
			char buffer[8192];
			ssize_t len = xread(fd, buffer, sizeof(buffer));
			if (len < 0)
				goto io_error;
			if (!len)
				break;
			strbuf_add(data, buffer, len);
		}
		if (data->len > m->max_file) {
			memory_error(m, err, "LIMIT", path);
			goto done;
		}
		if (fstat(fd, &after) || !same_stat(&before, &after))
			goto conflict;
		*mode = S_IFREG | ((before.st_mode & 0111) ? 0755 : 0644);
	}
	if (fstatat(parent, leaf, &after, AT_SYMLINK_NOFOLLOW) || !same_stat(&before, &after))
		goto conflict;
	*observed = before;
	ret = 0;
	goto done;
io_error:
	memory_error(m, err, "IO", path);
	goto done;
conflict:
	memory_error(m, err, "CONFLICT", path);
done:
	if (fd >= 0)
		close(fd);
	close(parent);
	free(leaf);
	return ret;
}

static int index_fingerprint(struct memory_store *m, struct object_id *oid,
			     struct strbuf *err)
{
	struct strbuf data = STRBUF_INIT;
	if (strbuf_read_file(&data, repo_get_index_file(m->source), 0) < 0 && errno != ENOENT)
		return memory_error(m, err, "IO", "index");
	hash_object_file(m->source->hash_algo, data.buf, data.len, OBJ_BLOB, oid);
	strbuf_release(&data);
	return 0;
}

static int snapshot_sources(struct memory_store *m, struct index_state *work,
			    struct index_state *staged, struct string_list *observed, struct strbuf *err)
{
	struct index_state *source = m->source->index;
	unsigned long total = 0, count = 0;
	unsigned int i;
	for (i = 0; i < source->cache_nr; i++) {
		struct cache_entry *ce = source->cache[i];
		struct object_id oid;
		struct strbuf raw = STRBUF_INIT;
		struct object_info info = OBJECT_INFO_INIT;
		struct observation observation = { 0 };
		enum object_type type;
		size_t size;
		void *data;
		unsigned mode;
		int missing, ret;
		if (!selected(m, ce->name))
			continue;
		if (!is_utf8(ce->name))
			return memory_error(m, err, "UTF8", "path");
		if (ce_stage(ce) || ce_skip_worktree(ce) || ce_intent_to_add(ce) ||
		    S_ISGITLINK(ce->ce_mode) || sensitive(ce->name))
			return memory_error(m, err, "UNSUPPORTED", ce->name);
		if (++count > m->max_files)
			return memory_error(m, err, "LIMIT", "files");
		info.typep = &type;
		info.sizep = &size;
		if (odb_read_object_info_extended(m->source->objects, &ce->oid, &info,
						OBJECT_INFO_SKIP_FETCH_OBJECT) < 0 || type != OBJ_BLOB)
			return memory_error(m, err, "OBJECT", ce->name);
		if (size > m->max_file || size > m->max_total - total)
			return memory_error(m, err, "LIMIT", ce->name);
		data = NULL;
		info.contentp = &data;
		if (odb_read_object_info_extended(m->source->objects, &ce->oid, &info,
						OBJECT_INFO_SKIP_FETCH_OBJECT) < 0 || !data) {
			free(data);
			return memory_error(m, err, "OBJECT", ce->name);
		}
		total += size;
		ret = memory_blob_write(m, data, size, &oid, err);
		free(data);
		if (ret || memory_tree_add(m, staged, ce->name, ce->ce_mode, &oid, err))
			return -1;
		if (raw_file(m, ce->name, &raw, &mode, &missing, &observation.st, err)) {
			strbuf_release(&raw);
			return -1;
		}
		observation.missing = missing;
		string_list_append(observed, ce->name)->util = xmemdupz(&observation, sizeof(observation));
		if (!missing) {
			if (raw.len > m->max_total - total) {
				strbuf_release(&raw);
				return memory_error(m, err, "LIMIT", "snapshot");
			}
			total += raw.len;
			ret = memory_blob_write(m, raw.buf, raw.len, &oid, err) ||
				memory_tree_add(m, work, ce->name, mode, &oid, err);
			strbuf_release(&raw);
			if (ret)
				return -1;
		}
	}
	return 0;
}

static int verify_observed(struct memory_store *m, struct string_list *observed,
			   struct strbuf *err)
{
	struct string_list_item *item;
	for_each_string_list_item(item, observed) {
		struct observation *initial = item->util;
		struct stat current;
		char *leaf = NULL;
		int parent = parent_fd(m, item->string, &leaf, err);
		int missing = parent == -2, invalid = parent == -1;
		if (parent >= 0) {
			if (fstatat(parent, leaf, &current, AT_SYMLINK_NOFOLLOW)) {
				missing = errno == ENOENT;
				invalid = !missing;
			}
			close(parent);
		}
		free(leaf);
		if (invalid || missing != initial->missing ||
		    (!missing && !same_stat(&current, &initial->st)))
			return memory_error(m, err, "CONFLICT", item->string);
	}
	return 0;
}

static int event_blob(struct memory_store *m, const struct memory_attribution *who,
		      const struct object_id *head, struct object_id *oid, struct strbuf *err)
{
	struct json_writer jw = JSON_WRITER_INIT;
	int ret;
	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, "schema", 1);
	jw_object_intmax(&jw, "observed_at", (intmax_t)time(NULL));
	jw_object_null(&jw, "actual_edit_time");
	jw_object_string(&jw, "source_head", is_null_oid(head) ? "unknown" : oid_to_hex(head));
	jw_object_string(&jw, "attribution", "declared_not_authenticated");
	jw_object_string(&jw, "declared_actor", who->actor ? who->actor : "unknown");
	jw_object_string(&jw, "declared_model", who->model ? who->model : "unknown");
	jw_object_string(&jw, "declared_session", who->session ? who->session : "unknown");
	jw_object_string(&jw, "declared_task", who->task ? who->task : "unknown");
	jw_object_string(&jw, "content_policy", "selected_tracked_raw_and_index");
	jw_object_string(&jw, "semantic_coverage", "unknown");
	jw_end(&jw);
	ret = memory_blob_write(m, jw.json.buf, jw.json.len, oid, err);
	jw_release(&jw);
	return ret;
}

int memory_capture(struct memory_store *m, const struct memory_attribution *who,
		   struct object_id *snapshot, int *changed, struct strbuf *err)
{
	struct index_state work = INDEX_STATE_INIT(&m->repo);
	struct index_state staged = INDEX_STATE_INIT(&m->repo);
	struct index_state root = INDEX_STATE_INIT(&m->repo);
	struct string_list observed = STRING_LIST_INIT_DUP;
	struct strbuf previous_event = STRBUF_INIT;
	struct object_id old, head, head_after, before, after, wt, it, prev_wt, prev_it, event, tree;
	int ret = -1, exists;
	*changed = 0;
	{
		int sparse = 0;
		repo_config_get_bool(m->source, "core.sparsecheckout", &sparse);
		if (sparse || repo_has_promisor_remote(m->source))
			return memory_error(m, err, "UNSUPPORTED", "sparse_or_partial_clone");
	}
	if (memory_lock(m, err))
		return -1;
	oidclr(&head, m->source->hash_algo);
	repo_get_oid(m->source, "HEAD", &head);
	if (index_fingerprint(m, &before, err))
		goto done;
	discard_index(m->source->index);
	if (repo_read_index(m->source) < 0) {
		memory_error(m, err, "IO", "index");
		goto done;
	}
	if (snapshot_sources(m, &work, &staged, &observed, err) ||
	    memory_tree_write(m, &work, &wt, err) ||
	    memory_tree_write(m, &staged, &it, err) ||
	    verify_observed(m, &observed, err) ||
	    index_fingerprint(m, &after, err))
		goto done;
	oidclr(&head_after, m->source->hash_algo);
	repo_get_oid(m->source, "HEAD", &head_after);
	if (!oideq(&before, &after) || !oideq(&head, &head_after)) {
		memory_error(m, err, "CONFLICT", "index_or_HEAD");
		goto done;
	}
	exists = memory_ref(m, MEMORY_SNAPSHOTS, &old);
	if (exists < 0) { memory_error(m, err, "OBJECT", MEMORY_SNAPSHOTS); goto done; }
	if (exists && (memory_snapshot_entry(m, &old, "worktree", &prev_wt, err) ||
		       memory_snapshot_entry(m, &old, "index", &prev_it, err) ||
		       memory_event_read(m, &old, &previous_event, err)))
		goto done;
	if (exists && oideq(&wt, &prev_wt) && oideq(&it, &prev_it)) {
		oidcpy(snapshot, &old);
		ret = 0;
		goto done;
	}
	if (event_blob(m, who, &head, &event, err) ||
	    memory_tree_add(m, &root, "worktree", S_IFDIR, &wt, err) ||
	    memory_tree_add(m, &root, "index", S_IFDIR, &it, err) ||
	    memory_tree_add(m, &root, "event.json", 0100644, &event, err) ||
	    memory_tree_write(m, &root, &tree, err) ||
	    memory_publish(m, MEMORY_SNAPSHOTS, &tree, &old, NULL,
			   "Observed selected source snapshot", snapshot, err))
		goto done;
	*changed = 1;
	ret = 0;
done:
	strbuf_release(&previous_event);
	string_list_clear(&observed, 1);
	release_index(&root);
	release_index(&staged);
	release_index(&work);
	memory_unlock(m);
	return ret;
}
