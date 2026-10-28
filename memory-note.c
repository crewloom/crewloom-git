#include "git-compat-util.h"
#include "memory.h"
#include "hex.h"
#include "json-writer.h"
#include "utf8.h"

int memory_note(struct memory_store *m, const struct object_id *source_snapshot,
		const char *text, int nr, const char **paths,
		const struct memory_attribution *who, struct object_id *note, struct strbuf *err)
{
	struct index_state sources = INDEX_STATE_INIT(&m->repo), root = INDEX_STATE_INIT(&m->repo);
	struct object_id old, tree, source_tree, blob, event;
	struct json_writer jw = JSON_WRITER_INIT;
	struct string_list recorded = STRING_LIST_INIT_DUP;
	int i, ret = -1;
	if (nr < 1 || !*text || strlen(text) > MEMORY_CONTEXT_BYTES || !is_utf8(text))
		return memory_error(m, err, "OPTIONS", "note_sources_or_text");
	if (memory_lock(m, err))
		return -1;
	if (memory_snapshot_entry(m, source_snapshot, "worktree", &source_tree, err) ||
	    memory_tree_files(m, &source_tree, &recorded, err))
		goto done;
	for (i = 0; i < nr; i++) {
		struct string_list_item *found = string_list_lookup(&recorded, paths[i]);
		struct memory_entry *entry = found ? found->util : NULL;
		if (!entry) { memory_error(m, err, "NO_MATCH", paths[i]); goto done; }
		if (memory_tree_add(m, &sources, paths[i], entry->mode, &entry->oid, err))
			goto done;
	}
	if (memory_tree_write(m, &sources, &source_tree, err) ||
	    memory_blob_write(m, text, strlen(text), &blob, err))
		goto done;
	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, "schema", 1);
	jw_object_string(&jw, "source_snapshot", oid_to_hex(source_snapshot));
	jw_object_string(&jw, "evidence", "candidate_untrusted");
	jw_object_string(&jw, "dependency_coverage", "declared_only");
	jw_object_string(&jw, "attribution", "declared_not_authenticated");
	jw_object_string(&jw, "declared_actor", who->actor ? who->actor : "unknown");
	jw_object_string(&jw, "declared_model", who->model ? who->model : "unknown");
	jw_object_string(&jw, "declared_session", who->session ? who->session : "unknown");
	jw_object_string(&jw, "declared_task", who->task ? who->task : "unknown");
	jw_object_intmax(&jw, "observed_at", (intmax_t)time(NULL));
	jw_end(&jw);
	if (memory_blob_write(m, jw.json.buf, jw.json.len, &event, err) ||
	    memory_tree_add(m, &root, "sources", S_IFDIR, &source_tree, err) ||
	    memory_tree_add(m, &root, "note.txt", 0100644, &blob, err) ||
	    memory_tree_add(m, &root, "event.json", 0100644, &event, err) ||
	    memory_tree_write(m, &root, &tree, err))
		goto done;
	if (memory_ref(m, MEMORY_NOTES, &old) < 0) {
		memory_error(m, err, "OBJECT", MEMORY_NOTES);
		goto done;
	}
	if (memory_publish(m, MEMORY_NOTES, &tree, &old, NULL,
			   "Record untrusted source-linked candidate", note, err))
		goto done;
	ret = 0;
done:
	jw_release(&jw);
	string_list_clear(&recorded, 1);
	release_index(&root);
	release_index(&sources);
	memory_unlock(m);
	return ret;
}

static const char *source_state(struct string_list *dependencies, struct string_list *current)
{
	struct string_list_item *dep;
	int changed = 0;
	for_each_string_list_item(dep, dependencies) {
		struct string_list_item *found = string_list_lookup(current, dep->string);
		struct memory_entry *a = dep->util, *b = found ? found->util : NULL;
		if (!b)
			return "missing";
		if (!oideq(&a->oid, &b->oid) || a->mode != b->mode)
			changed = 1;
	}
	return changed ? "changed" : "inputs_unchanged";
}

int memory_notes_context(struct memory_store *m, const struct object_id *snapshot,
			const char *query, unsigned long budget, unsigned long *omitted,
			struct strbuf *out, struct strbuf *err)
{
	struct json_writer jw = JSON_WRITER_INIT;
	struct string_list current = STRING_LIST_INIT_DUP;
	struct object_id tree, note, parent, oid;
	struct strbuf text = STRBUF_INIT, event = STRBUF_INIT;
	unsigned count = 0;
	int exists = memory_ref(m, MEMORY_NOTES, &note), ret = -1;
	if (exists < 0)
		return memory_error(m, err, "OBJECT", MEMORY_NOTES);
	if (memory_snapshot_entry(m, snapshot, "worktree", &tree, err) ||
	    memory_tree_files(m, &tree, &current, err))
		goto done;
	jw_array_begin(&jw, 0);
	while (exists && count++ < MEMORY_HISTORY_LIMIT) {
		struct string_list deps = STRING_LIST_INIT_DUP;
		struct json_writer item = JSON_WRITER_INIT;
		int relevant = !*query;
		struct string_list_item *dep;
		if (memory_snapshot_entry(m, &note, "note.txt", &oid, err) ||
		    memory_blob_read(m, &oid, &text, err) ||
		    memory_event_read(m, &note, &event, err) ||
		    memory_snapshot_entry(m, &note, "sources", &tree, err) ||
		    memory_tree_files(m, &tree, &deps, err)) {
			string_list_clear(&deps, 1);
			goto done;
		}
		if (strstr(text.buf, query))
			relevant = 1;
		for_each_string_list_item(dep, &deps)
			if (strstr(dep->string, query))
				relevant = 1;
		if (relevant) {
			jw_object_begin(&item, 0);
			jw_object_string(&item, "id", oid_to_hex(&note));
			jw_object_string(&item, "text", text.buf);
			jw_object_string(&item, "source_state", source_state(&deps, &current));
			jw_object_string(&item, "evidence", "candidate_untrusted");
			jw_object_string(&item, "dependency_coverage", "declared_only");
			jw_object_string(&item, "event_json", event.buf);
			jw_end(&item);
			if (jw.json.len + item.json.len + 3 < budget)
				jw_array_sub_jw(&jw, &item);
			else
				(*omitted)++;
		}
		jw_release(&item);
		string_list_clear(&deps, 1);
		exists = memory_parent(m, &note, &parent, err);
		if (exists < 0)
			goto done;
		oidcpy(&note, &parent);
	}
	if (exists)
		(*omitted)++;
	jw_end(&jw);
	strbuf_addbuf(out, &jw.json);
	ret = 0;
done:
	jw_release(&jw);
	string_list_clear(&current, 1);
	strbuf_release(&text);
	strbuf_release(&event);
	return ret;
}
