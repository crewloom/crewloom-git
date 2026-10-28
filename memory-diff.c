#include "git-compat-util.h"
#include "memory.h"
#include "hex.h"
#include "json-writer.h"

static void replacement_span(struct json_writer *jw, const struct strbuf *old,
			     const struct strbuf *new_data)
{
	size_t prefix = 0, suffix = 0, line = 1, column = 0, i;
	while (prefix < old->len && prefix < new_data->len &&
	       old->buf[prefix] == new_data->buf[prefix])
		prefix++;
	while (suffix < old->len - prefix && suffix < new_data->len - prefix &&
	       old->buf[old->len - suffix - 1] == new_data->buf[new_data->len - suffix - 1])
		suffix++;
	for (i = 0; i < prefix; i++) {
		if (old->buf[i] == '\n') { line++; column = 0; }
		else column++;
	}
	jw_object_string(jw, "span_kind", "bounding_replacement");
	jw_object_intmax(jw, "old_start_byte", prefix);
	jw_object_intmax(jw, "new_start_byte", prefix);
	jw_object_intmax(jw, "old_length", old->len - prefix - suffix);
	jw_object_intmax(jw, "new_length", new_data->len - prefix - suffix);
	jw_object_intmax(jw, "start_line", line);
	jw_object_intmax(jw, "start_column_bytes", column);
}

int memory_diff(struct memory_store *m, const struct object_id *old,
		const struct object_id *new_oid, struct strbuf *out, struct strbuf *err)
{
	struct object_id tree;
	struct string_list before = STRING_LIST_INIT_DUP, after = STRING_LIST_INIT_DUP;
	struct string_list paths = STRING_LIST_INIT_DUP;
	struct string_list_item *item;
	struct json_writer jw = JSON_WRITER_INIT;
	struct strbuf a = STRBUF_INIT, b = STRBUF_INIT;
	int ret = -1;
	if (memory_snapshot_entry(m, old, "worktree", &tree, err) ||
	    memory_tree_files(m, &tree, &before, err) ||
	    memory_snapshot_entry(m, new_oid, "worktree", &tree, err) ||
	    memory_tree_files(m, &tree, &after, err))
		goto done;
	for_each_string_list_item(item, &before)
		string_list_insert(&paths, item->string);
	for_each_string_list_item(item, &after)
		string_list_insert(&paths, item->string);
	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, "schema", 1);
	jw_object_string(&jw, "from", oid_to_hex(old));
	jw_object_string(&jw, "to", oid_to_hex(new_oid));
	jw_object_inline_begin_array(&jw, "changes");
	for_each_string_list_item(item, &paths) {
		struct string_list_item *ai = string_list_lookup(&before, item->string);
		struct string_list_item *bi = string_list_lookup(&after, item->string);
		struct memory_entry *ae = ai ? ai->util : NULL, *be = bi ? bi->util : NULL;
		if (ae && be && ae->mode == be->mode && oideq(&ae->oid, &be->oid))
			continue;
		strbuf_reset(&a);
		strbuf_reset(&b);
		if ((ae && memory_blob_read(m, &ae->oid, &a, err)) ||
		    (be && memory_blob_read(m, &be->oid, &b, err)))
			goto done;
		jw_array_inline_begin_object(&jw);
		jw_object_string(&jw, "path", item->string);
		jw_object_string(&jw, "kind", !ae ? "added" : !be ? "deleted" : "modified");
		jw_object_string(&jw, "old_oid", ae ? oid_to_hex(&ae->oid) : "absent");
		jw_object_string(&jw, "new_oid", be ? oid_to_hex(&be->oid) : "absent");
		jw_object_intmax(&jw, "old_mode", ae ? ae->mode : 0);
		jw_object_intmax(&jw, "new_mode", be ? be->mode : 0);
		replacement_span(&jw, &a, &b);
		jw_end(&jw);
	}
	jw_end(&jw);
	jw_object_intmax(&jw, "omissions", 0);
	jw_end(&jw);
	strbuf_addbuf(out, &jw.json);
	strbuf_addch(out, '\n');
	ret = 0;
done:
	strbuf_release(&a);
	strbuf_release(&b);
	jw_release(&jw);
	string_list_clear(&before, 1);
	string_list_clear(&after, 1);
	string_list_clear(&paths, 0);
	return ret;
}
