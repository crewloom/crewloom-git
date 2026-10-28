#include "git-compat-util.h"
#include "memory.h"
#include "hex.h"
#include "json-writer.h"
#include "utf8.h"

static void add_excerpt(struct json_writer *item, const char *path,
			const struct memory_entry *entry, const struct strbuf *raw,
			const char *query, unsigned long budget)
{
	const char *match = *query ? strstr(raw->buf, query) : NULL;
	size_t start = match ? (size_t)(match - raw->buf) : 0, end, i;
	unsigned long line = 1;
	while (start && raw->buf[start - 1] != '\n')
		start--;
	for (i = 0; i < start; i++)
		if (raw->buf[i] == '\n')
			line++;
	end = st_add(start, budget / 8);
	if (end > raw->len)
		end = raw->len;
	/* Never cut a UTF-8 continuation byte in a JSON string. */
	while (end < raw->len && end > start && ((unsigned char)raw->buf[end] & 0xc0) == 0x80)
		end--;
	jw_object_begin(item, 0);
	jw_object_string(item, "path", path);
	jw_object_string(item, "oid", oid_to_hex(&entry->oid));
	jw_object_intmax(item, "mode", entry->mode);
	jw_object_intmax(item, "start_line", line);
	jw_object_intmax(item, "start_byte", start);
	jw_object_intmax(item, "source_bytes", raw->len);
	jw_object_bool(item, "truncated", start != 0 || end != raw->len);
	{
		char *text = xmemdupz(raw->buf + start, end - start);
		jw_object_string(item, "text", text);
		free(text);
	}
	jw_end(item);
}

int memory_context(struct memory_store *m, const char *query, unsigned long budget,
		   const struct object_id *snapshot, struct strbuf *out, struct strbuf *err)
{
	struct object_id tree;
	struct string_list files = STRING_LIST_INIT_DUP;
	struct string_list_item *file;
	struct json_writer jw = JSON_WRITER_INIT, notes = JSON_WRITER_INIT;
	struct strbuf raw = STRBUF_INIT, note_data = STRBUF_INIT, event = STRBUF_INIT;
	unsigned long omitted = 0, unmatched = 0, truncated = 0, binary = 0;
	int ret = -1;
	if (budget < 1024 || budget > MEMORY_MAX_CONTEXT)
		return memory_error(m, err, "BUDGET", "1024..2097152");
	if (memory_event_read(m, snapshot, &event, err) ||
	    memory_snapshot_entry(m, snapshot, "worktree", &tree, err) ||
	    memory_tree_files(m, &tree, &files, err) ||
	    memory_notes_context(m, snapshot, query, budget / 4, &omitted, &note_data, err))
		goto done;
	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, "schema", 1);
	jw_object_string(&jw, "snapshot", oid_to_hex(snapshot));
	jw_object_string(&jw, "selection", "literal_path_or_content_match");
	jw_object_string(&jw, "semantic_coverage", "unknown");
	jw_object_string(&jw, "syntax_parser", "not_implemented");
	jw_object_null(&jw, "provider_tokens");
	jw_object_null(&jw, "quota_savings");
	strbuf_addbuf(&notes.json, &note_data);
	jw_object_sub_jw(&jw, "notes", &notes);
	jw_object_inline_begin_array(&jw, "files");
	for_each_string_list_item(file, &files) {
		struct memory_entry *entry = file->util;
		struct json_writer item = JSON_WRITER_INIT;
		if (memory_blob_read(m, &entry->oid, &raw, err))
			goto done;
		if (memchr(raw.buf, 0, raw.len) || !is_utf8(raw.buf)) {
			omitted++;
			binary++;
			continue;
		}
		if (*query && !strstr(file->string, query) && !strstr(raw.buf, query)) {
			unmatched++;
			continue;
		}
		add_excerpt(&item, file->string, entry, &raw, query, budget);
		if (jw.json.len + item.json.len + 256 < budget) {
			jw_array_sub_jw(&jw, &item);
			if (raw.len > budget / 8) {
				truncated++;
				omitted++;
			}
		} else {
			omitted++;
		}
		jw_release(&item);
	}
	jw_end(&jw);
	jw_object_intmax(&jw, "omissions", omitted);
	jw_object_intmax(&jw, "unmatched_files", unmatched);
	jw_object_intmax(&jw, "truncated_files", truncated);
	jw_object_intmax(&jw, "binary_or_non_utf8_omissions", binary);
	jw_object_string(&jw, "retention_in_model", "unknown");
	jw_end(&jw);
	if (jw.json.len + 1 > budget) {
		memory_error(m, err, "BUDGET", "context envelope");
		goto done;
	}
	strbuf_addbuf(out, &jw.json);
	strbuf_addch(out, '\n');
	ret = 0;
done:
	jw_release(&jw);
	jw_release(&notes);
	strbuf_release(&raw);
	strbuf_release(&note_data);
	strbuf_release(&event);
	string_list_clear(&files, 1);
	return ret;
}

int memory_history(struct memory_store *m, const char *ref, unsigned limit,
		   struct strbuf *out, struct strbuf *err)
{
	struct json_writer jw = JSON_WRITER_INIT;
	struct strbuf event = STRBUF_INIT;
	struct object_id current, parent;
	unsigned count = 0;
	int exists = memory_ref(m, ref, &current), ret = -1;
	if (exists < 0)
		return memory_error(m, err, "OBJECT", ref);
	jw_object_begin(&jw, 0);
	jw_object_intmax(&jw, "schema", 1);
	jw_object_inline_begin_array(&jw, "events");
	while (exists && count++ < limit) {
		if (memory_event_read(m, &current, &event, err))
			goto done;
		jw_array_inline_begin_object(&jw);
		jw_object_string(&jw, "snapshot", oid_to_hex(&current));
		jw_object_string(&jw, "event_json", event.buf);
		jw_end(&jw);
		exists = memory_parent(m, &current, &parent, err);
		if (exists < 0)
			goto done;
		oidcpy(&current, &parent);
	}
	jw_end(&jw);
	jw_object_bool(&jw, "more_history", exists);
	jw_object_intmax(&jw, "omissions", exists ? 1 : 0);
	jw_end(&jw);
	strbuf_addbuf(out, &jw.json);
	strbuf_addch(out, '\n');
	ret = 0;
done:
	strbuf_release(&event);
	jw_release(&jw);
	return ret;
}
