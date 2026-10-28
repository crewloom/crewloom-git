#include "git-compat-util.h"
#include "memory.h"
#include "cache-tree.h"
#include "commit.h"
#include "hex.h"
#include "pathspec.h"
#include "repo-settings.h"
#include "odb.h"
#include "object-file.h"
#include "refs.h"
#include "tree.h"
#include "tree-walk.h"

static int read_verified_blob(struct memory_store *m, const struct object_id *oid,
			      void **data, size_t *length, struct strbuf *err)
{
	struct object_info info = OBJECT_INFO_INIT;
	struct object_id actual;
	enum object_type type;
	size_t size = 0, read_size = 0;
	void *content = NULL;
	info.typep = &type;
	info.sizep = &size;
	if (odb_read_object_info_extended(m->repo.objects, oid, &info,
					  OBJECT_INFO_SKIP_FETCH_OBJECT) < 0 || type != OBJ_BLOB)
		return memory_error(m, err, "OBJECT", "blob metadata");
	if (size > m->max_file)
		return memory_error(m, err, "LIMIT", "blob size");
	info.sizep = &read_size;
	info.contentp = &content;
	if (odb_read_object_info_extended(m->repo.objects, oid, &info,
					  OBJECT_INFO_SKIP_FETCH_OBJECT) < 0 || !content ||
	    type != OBJ_BLOB || read_size != size) {
		free(content);
		return memory_error(m, err, "OBJECT", "blob content");
	}
	hash_object_file(m->repo.hash_algo, content, read_size, OBJ_BLOB, &actual);
	if (!oideq(oid, &actual)) {
		free(content);
		return memory_error(m, err, "OBJECT", "blob hash");
	}
	*data = content;
	*length = read_size;
	return 0;
}

int memory_blob_write(struct memory_store *m, const void *data, size_t len,
		      struct object_id *oid, struct strbuf *err)
{
	void *stored = NULL;
	size_t stored_len = 0;
	int exists;
	if (len > m->max_file)
		return memory_error(m, err, "LIMIT", "blob size");
	hash_object_file(m->repo.hash_algo, data, len, OBJ_BLOB, oid);
	exists = odb_has_object(m->repo.objects, oid, ODB_HAS_OBJECT_RECHECK_PACKED);
	if (!exists && odb_write_object(m->repo.objects, data, len, OBJ_BLOB, oid))
		return memory_error(m, err, "IO", "blob write");
	if (read_verified_blob(m, oid, &stored, &stored_len, err))
		return -1;
	if (stored_len != len || (len && memcmp(stored, data, len))) {
		free(stored);
		return memory_error(m, err, "OBJECT", "blob mismatch");
	}
	free(stored);
	return 0;
}

int memory_blob_read(struct memory_store *m, const struct object_id *oid,
		     struct strbuf *data, struct strbuf *err)
{
	void *content = NULL;
	size_t size = 0;
	if (read_verified_blob(m, oid, &content, &size, err))
		return -1;
	strbuf_reset(data);
	strbuf_add(data, content, size);
	free(content);
	return 0;
}

int memory_tree_add(struct memory_store *m, struct index_state *index,
		    const char *path, unsigned mode, const struct object_id *oid,
		    struct strbuf *err)
{
	struct cache_entry *ce;
	struct strbuf directory = STRBUF_INIT;
	const char *index_path = path;
	int result = 0;
	if (!verify_path(path, mode))
		return memory_error(m, err, "SCOPE", path);
	if (S_ISDIR(mode)) {
		strbuf_addf(&directory, "%s/", path);
		index_path = directory.buf;
	}
	ce = make_cache_entry(index, mode, oid, index_path, 0, 0);
	if (ce && S_ISDIR(mode)) {
		ce->ce_mode = S_IFDIR;
		ce->ce_flags |= CE_SKIP_WORKTREE;
	}
	if (!ce || add_index_entry(index, ce, ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE))
		result = memory_error(m, err, "OBJECT", path);
	strbuf_release(&directory);
	return result;
}

int memory_tree_write(struct memory_store *m, struct index_state *index,
		      struct object_id *oid, struct strbuf *err)
{
	struct tree *tree = write_in_core_index_as_tree(&m->repo, index);
	if (!tree)
		return memory_error(m, err, "OBJECT", "tree write");
	oidcpy(oid, &tree->object.oid);
	return 0;
}

static int collect_file(const struct object_id *oid, struct strbuf *base,
			const char *name, unsigned int mode, void *context)
{
	struct string_list *files = context;
	struct memory_entry *entry;
	struct strbuf path = STRBUF_INIT;
	if (S_ISDIR(mode))
		return READ_TREE_RECURSIVE;
	strbuf_addbuf(&path, base);
	strbuf_addstr(&path, name);
	CALLOC_ARRAY(entry, 1);
	oidcpy(&entry->oid, oid);
	entry->mode = mode;
	string_list_append(files, path.buf)->util = entry;
	strbuf_release(&path);
	return 0;
}

int memory_tree_files(struct memory_store *m, const struct object_id *tree_oid,
		      struct string_list *files, struct strbuf *err)
{
	struct tree *tree = lookup_tree(&m->repo, tree_oid);
	struct pathspec pathspec = { 0 };
	prepare_repo_settings(&m->repo);
	if (!tree || repo_parse_tree(&m->repo, tree) ||
	    read_tree(&m->repo, tree, &pathspec, collect_file, files))
		return memory_error(m, err, "OBJECT", "tree read");
	string_list_sort(files);
	return 0;
}

static struct commit *memory_commit(struct memory_store *m, const struct object_id *oid)
{
	struct commit *commit = lookup_commit(&m->repo, oid);
	if (!commit || repo_parse_commit(&m->repo, commit))
		return NULL;
	return commit;
}

int memory_snapshot_entry(struct memory_store *m, const struct object_id *snapshot,
			  const char *path, struct object_id *oid, struct strbuf *err)
{
	struct commit *commit = memory_commit(m, snapshot);
	struct tree *tree;
	unsigned short mode;
	if (!commit || !(tree = repo_get_commit_tree(&m->repo, commit)))
		return memory_error(m, err, "OBJECT", "snapshot");
	if (get_tree_entry(&m->repo, &tree->object.oid, path, oid, &mode))
		return memory_error(m, err, "NO_MATCH", path);
	return 0;
}

int memory_parent(struct memory_store *m, const struct object_id *snapshot,
		  struct object_id *parent, struct strbuf *err)
{
	struct commit *commit = memory_commit(m, snapshot);
	if (!commit)
		return memory_error(m, err, "OBJECT", "commit");
	if (!commit->parents) {
		oidclr(parent, m->repo.hash_algo);
		return 0;
	}
	oidcpy(parent, &commit->parents->item->object.oid);
	return 1;
}

int memory_ref(struct memory_store *m, const char *ref, struct object_id *oid)
{
	int flags = 0;
	const char *result;
	oidclr(oid, m->repo.hash_algo);
	if (check_refname_format(ref, 0))
		return -1;
	result = refs_resolve_ref_unsafe(get_main_ref_store(&m->repo), ref, 0, oid, &flags);
	if (!result || (flags & (REF_ISBROKEN | REF_ISSYMREF | REF_BAD_NAME))) {
		oidclr(oid, m->repo.hash_algo);
		return -1;
	}
	if (is_null_oid(oid))
		return 0;
	if (!odb_has_object(m->repo.objects, oid, ODB_HAS_OBJECT_RECHECK_PACKED)) {
		oidclr(oid, m->repo.hash_algo);
		return -1;
	}
	return 1;
}

int memory_publish(struct memory_store *m, const char *ref,
		   const struct object_id *tree, const struct object_id *old,
		   const struct object_id *extra_parent, const char *message,
		   struct object_id *snapshot, struct strbuf *err)
{
	struct strbuf commit = STRBUF_INIT;
	struct object_id zero;
	timestamp_t now = time(NULL);
	int result = -1;
	strbuf_addf(&commit, "tree %s\n", oid_to_hex(tree));
	if (old && !is_null_oid(old))
		strbuf_addf(&commit, "parent %s\n", oid_to_hex(old));
	if (extra_parent && !is_null_oid(extra_parent))
		strbuf_addf(&commit, "parent %s\n", oid_to_hex(extra_parent));
	strbuf_addf(&commit, "author Crewloom Memory <memory@localhost> %"PRIuMAX" +0000\n"
		    "committer Crewloom Memory <memory@localhost> %"PRIuMAX" +0000\n\n%s\n",
		    (uintmax_t)now, (uintmax_t)now, message);
	if (odb_write_object(m->repo.objects, commit.buf, commit.len, OBJ_COMMIT, snapshot))
		goto done;
	oidclr(&zero, m->repo.hash_algo);
	if (refs_update_ref(get_main_ref_store(&m->repo), "memory capture", ref,
			    snapshot, old ? old : &zero, 0, UPDATE_REFS_QUIET_ON_ERR))
		goto done;
	result = 0;
done:
	strbuf_release(&commit);
	return result ? memory_error(m, err, "IO", "publish") : 0;
}
