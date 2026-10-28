#include "git-compat-util.h"
#include "memory.h"
#include "utf8.h"

/* Schema 1 metadata is a flat object of known scalar fields, never instructions. */
static int quoted(const char **cursor, struct strbuf *value, int key)
{
	const unsigned char *p = (const unsigned char *)*cursor;
	if (*p++ != '"') return -1;
	strbuf_reset(value);
	while (*p && *p != '"') {
		if (*p < 0x20) return -1;
		if (*p == '\\') {
			unsigned i;
			if (key) return -1;
			strbuf_addch(value, *p++);
			if (!*p || !strchr("\"\\/bfnrtu", *p)) return -1;
			if (*p == 'u') {
				strbuf_addch(value, *p++);
				for (i = 0; i < 4; i++) {
					if (!isxdigit(*p)) return -1;
					strbuf_addch(value, *p++);
				}
				continue;
			}
		}
		strbuf_addch(value, *p++);
	}
	if (*p++ != '"') return -1;
	*cursor = (const char *)p;
	return 0;
}

static void whitespace(const char **p)
{
	while (**p && strchr(" \t\r\n", **p)) (*p)++;
}

static int integer(const char **p, intmax_t *value)
{
	char *end;
	const char *begin = *p;
	if (*begin == '-') begin++;
	if (!isdigit(*begin) || (*begin == '0' && isdigit(begin[1]))) return -1;
	errno = 0;
	*value = strtoimax(*p, &end, 10);
	if (errno || end == *p) return -1;
	*p = end;
	return 0;
}

static int event_schema(const struct strbuf *event)
{
	static const char *const keys[] = {
		"schema", "observed_at", "actual_edit_time", "source_head",
		"attribution", "declared_actor", "declared_model", "declared_session",
		"declared_task", "content_policy", "semantic_coverage", "source_snapshot",
		"evidence", "dependency_coverage"
	};
	const char *p = event->buf;
	struct strbuf key = STRBUF_INIT, value = STRBUF_INIT;
	unsigned seen = 0;
	int ret = -1;
	if (event->len > MEMORY_CONTEXT_BYTES || memchr(p, 0, event->len) || !is_utf8(p)) goto done;
	whitespace(&p);
	if (*p++ != '{') goto done;
	while (1) {
		size_t field;
		intmax_t number;
		whitespace(&p);
		if (quoted(&p, &key, 1)) goto done;
		for (field = 0; field < ARRAY_SIZE(keys); field++)
			if (!strcmp(keys[field], key.buf)) break;
		if (field == ARRAY_SIZE(keys) || (seen & (1U << field))) goto done;
		seen |= 1U << field;
		whitespace(&p);
		if (*p++ != ':') goto done;
		whitespace(&p);
		if (field < 2) {
			if (integer(&p, &number) || (field == 0 && number != 1)) goto done;
		} else if (field == 2) {
			if (!skip_prefix(p, "null", &p)) goto done;
		} else {
			if (quoted(&p, &value, 0)) goto done;
			if (field == 4 && strcmp(value.buf, "declared_not_authenticated")) goto done;
		}
		whitespace(&p);
		if (*p == '}') { p++; break; }
		if (*p++ != ',') goto done;
	}
	whitespace(&p);
	if (*p || (seen & 3U) != 3U || !(seen & (1U << 4)) ||
	    (!!(seen & (1U << 3)) == !!(seen & (1U << 11)))) goto done;
	ret = 0;
done:
	strbuf_release(&key);
	strbuf_release(&value);
	return ret;
}

int memory_event_read(struct memory_store *m, const struct object_id *snapshot,
		      struct strbuf *event, struct strbuf *err)
{
	struct object_id oid;
	if (memory_snapshot_entry(m, snapshot, "event.json", &oid, err) ||
	    memory_blob_read(m, &oid, event, err)) return -1;
	if (event_schema(event)) return memory_error(m, err, "OBJECT", "event_schema_1");
	return 0;
}
