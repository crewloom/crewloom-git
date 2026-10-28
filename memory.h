#ifndef GIT_MEMORY_H
#define GIT_MEMORY_H

#include "git-compat-util.h"
#include "repository.h"
#include "hash.h"
#include "strbuf.h"
#include "string-list.h"
#include "lockfile.h"
#include "read-cache-ll.h"

#define MEMORY_SNAPSHOTS "refs/heads/snapshots"
#define MEMORY_NOTES "refs/heads/notes"
#define MEMORY_MAX_FILE (1024UL * 1024)
#define MEMORY_MAX_TOTAL (32UL * 1024 * 1024)
#define MEMORY_MAX_FILES 5000UL
#define MEMORY_CONTEXT_BYTES 16384UL
#define MEMORY_MAX_CONTEXT (2UL * 1024 * 1024)
#define MEMORY_HISTORY_LIMIT 64

struct memory_store {
	struct repository *source;
	struct repository repo;
	char *path;
	struct string_list scopes;
	struct lock_file lock;
	unsigned long max_file, max_total, max_files;
	int opened, arabic;
};

struct memory_entry {
	struct object_id oid;
	unsigned int mode;
};

struct memory_attribution {
	const char *actor, *model, *session, *task;
};

int memory_error(struct memory_store *m, struct strbuf *err,
		 const char *code, const char *detail);
int memory_initialize(struct memory_store *m, struct repository *source,
		      int nr, const char **scopes, struct strbuf *err);
int memory_open(struct memory_store *m, struct repository *source, struct strbuf *err);
void memory_close(struct memory_store *m);
int memory_lock(struct memory_store *m, struct strbuf *err);
void memory_unlock(struct memory_store *m);
int memory_ref(struct memory_store *m, const char *ref, struct object_id *oid);
int memory_blob_write(struct memory_store *m, const void *data, size_t len,
		      struct object_id *oid, struct strbuf *err);
int memory_blob_read(struct memory_store *m, const struct object_id *oid,
		     struct strbuf *data, struct strbuf *err);
int memory_tree_write(struct memory_store *m, struct index_state *index,
		      struct object_id *oid, struct strbuf *err);
int memory_tree_add(struct memory_store *m, struct index_state *index,
		    const char *path, unsigned mode, const struct object_id *oid,
		    struct strbuf *err);
int memory_tree_files(struct memory_store *m, const struct object_id *tree,
		      struct string_list *files, struct strbuf *err);
int memory_snapshot_entry(struct memory_store *m, const struct object_id *snapshot,
			  const char *path, struct object_id *oid, struct strbuf *err);
int memory_publish(struct memory_store *m, const char *ref,
		   const struct object_id *tree, const struct object_id *old,
		   const struct object_id *extra_parent, const char *message,
		   struct object_id *snapshot, struct strbuf *err);
int memory_parent(struct memory_store *m, const struct object_id *snapshot,
		  struct object_id *parent, struct strbuf *err);

int memory_event_read(struct memory_store *m, const struct object_id *snapshot,
		      struct strbuf *event, struct strbuf *err);

int memory_capture(struct memory_store *m, const struct memory_attribution *who,
		   struct object_id *snapshot, int *changed, struct strbuf *err);
int memory_context(struct memory_store *m, const char *query, unsigned long budget,
		   const struct object_id *snapshot, struct strbuf *out, struct strbuf *err);
int memory_history(struct memory_store *m, const char *ref, unsigned limit,
		   struct strbuf *out, struct strbuf *err);
int memory_diff(struct memory_store *m, const struct object_id *old,
		const struct object_id *new_oid, struct strbuf *out, struct strbuf *err);
int memory_note(struct memory_store *m, const struct object_id *source_snapshot,
		const char *text, int nr, const char **paths,
		const struct memory_attribution *who, struct object_id *note, struct strbuf *err);
int memory_notes_context(struct memory_store *m, const struct object_id *snapshot,
			const char *query, unsigned long budget, unsigned long *omitted,
			struct strbuf *out, struct strbuf *err);
#endif
