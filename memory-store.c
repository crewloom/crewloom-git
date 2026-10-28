#include "git-compat-util.h"
#include "memory.h"
#include "config.h"
#include "path.h"
#include "repo-settings.h"
#include "setup.h"

static int private_dir(const char *path)
{
	struct stat st;
	return !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() &&
		!(st.st_mode & 077);
}

static int private_file(const char *path)
{
	struct stat st;
	return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
		st.st_nlink == 1 && !(st.st_mode & 077);
}

static int unsafe_environment(void)
{
	static const char *const names[] = {
		"GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_INDEX_FILE",
		"GIT_COMMON_DIR", "GIT_CONFIG_COUNT", "GIT_CONFIG_PARAMETERS", NULL
	};
	int i;
	const char *global = getenv("GIT_CONFIG_GLOBAL");
	const char *system = getenv("GIT_CONFIG_SYSTEM");
	if ((global && strcmp(global, "/dev/null")) ||
	    (system && strcmp(system, "/dev/null")))
		return 1;
	for (i = 0; names[i]; i++)
		if (getenv(names[i]))
			return 1;
	return 0;
}

static void config_quote(struct strbuf *out, const char *value)
{
	const unsigned char *p = (const unsigned char *)value;
	strbuf_addch(out, '"');
	for (; *p; p++) {
		if (*p == '\\' || *p == '"') {
			strbuf_addch(out, '\\');
			strbuf_addch(out, *p);
		} else if (*p == '\n')
			strbuf_addstr(out, "\\n");
		else if (*p == '\t')
			strbuf_addstr(out, "\\t");
		else if (*p == '\b')
			strbuf_addstr(out, "\\b");
		else
			strbuf_addch(out, *p);
	}
	strbuf_addch(out, '"');
}

static int write_new(const char *path, const char *data, size_t len)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	int ret;
	if (fd < 0)
		return -1;
	ret = write_in_full(fd, data, len) != (ssize_t)len;
	if (close(fd))
		ret = 1;
	return ret ? -1 : 0;
}

static int create_dir(const char *root, const char *name)
{
	char *path = xstrfmt("%s/%s", root, name);
	int ret = mkdir(path, 0700);
	free(path);
	return ret;
}

static int setup_store(struct memory_store *m, struct repository *source,
		       struct strbuf *err)
{
	const char *worktree = repo_get_work_tree(source);
	int arabic = m->arabic;
	memset(m, 0, sizeof(*m));
	m->arabic = arabic;
	m->source = source;
	m->max_file = MEMORY_MAX_FILE;
	m->max_total = MEMORY_MAX_TOTAL;
	m->max_files = MEMORY_MAX_FILES;
	string_list_init_dup(&m->scopes);
	if (!worktree || unsafe_environment())
		return memory_error(m, err, "CONFIG", "worktree or environment");
	m->path = xstrfmt("%s/memory-store", repo_get_git_dir(source));
	return 0;
}

static int valid_scope(struct repository *source, const char *scope)
{
	char *root, *path, *resolved;
	int ok;
	if (!scope || !*scope || (strcmp(scope, ".") && !verify_path(scope, S_IFREG)))
		return 0;
	root = realpath(repo_get_work_tree(source), NULL);
	path = xstrfmt("%s/%s", repo_get_work_tree(source), scope);
	resolved = realpath(path, NULL);
	ok = root && resolved && starts_with(resolved, root) &&
		(!resolved[strlen(root)] || resolved[strlen(root)] == '/');
	free(root);
	free(path);
	free(resolved);
	return ok;
}

int memory_initialize(struct memory_store *m, struct repository *source,
		      int nr, const char **scopes, struct strbuf *err)
{
	struct strbuf config = STRBUF_INIT;
	char *worktree = NULL, *gitdir = NULL, *path = NULL, *lock_path = NULL;
	struct lock_file init_lock = LOCK_INIT;
	int i, ret = -1, created = 0;
	if (setup_store(m, source, err))
		return -1;
	if (nr < 1) {
		memory_error(m, err, "SCOPE", "empty scope");
		goto done;
	}
	for (i = 0; i < nr; i++)
		if (!valid_scope(source, scopes[i])) {
			memory_error(m, err, "SCOPE", scopes[i]);
			goto done;
		}
	worktree = realpath(repo_get_work_tree(source), NULL);
	gitdir = realpath(repo_get_git_dir(source), NULL);
	if (!worktree || !gitdir) {
		memory_error(m, err, "STORE_UNSAFE", "cannot resolve repository");
		goto done;
	}
	lock_path = xstrfmt("%s.init", m->path);
	if (hold_lock_file_for_update_mode(&init_lock, lock_path, 0, 0600) < 0) {
		memory_error(m, err, "LOCK", "memory initialization");
		goto done;
	}
	if (mkdir(m->path, 0700)) {
		memory_error(m, err, "STORE_UNSAFE", "memory store already exists or cannot be created");
		goto done;
	}
	created = 1;
	if (create_dir(m->path, "objects") || create_dir(m->path, "objects/info") ||
	    create_dir(m->path, "objects/pack") || create_dir(m->path, "refs") ||
	    create_dir(m->path, "refs/heads")) {
		memory_error(m, err, "IO", "create store directories");
		goto done;
	}
	strbuf_addf(&config, "[core]\n\trepositoryformatversion = %d\n\tbare = true\n",
		    hash_algo_by_ptr(source->hash_algo) == GIT_HASH_SHA256 ? 1 : 0);
	if (hash_algo_by_ptr(source->hash_algo) == GIT_HASH_SHA256)
		strbuf_addstr(&config, "[extensions]\n\tobjectformat = sha256\n");
	strbuf_addstr(&config, "[memory]\n\tworktree = ");
	config_quote(&config, worktree);
	strbuf_addstr(&config, "\n\tgitdir = ");
	config_quote(&config, gitdir);
	for (i = 0; i < nr; i++) {
		strbuf_addstr(&config, "\n\tscope = ");
		config_quote(&config, scopes[i]);
	}
	strbuf_addf(&config, "\n\tmaxfile = %lu\n\tmaxtotal = %lu\n\tmaxfiles = %lu\n",
		    MEMORY_MAX_FILE, MEMORY_MAX_TOTAL, MEMORY_MAX_FILES);
	path = xstrfmt("%s/config", m->path);
	if (write_new(path, config.buf, config.len)) {
		memory_error(m, err, "IO", "write configuration");
		goto done;
	}
	free(path);
	path = xstrfmt("%s/HEAD", m->path);
	if (write_new(path, "ref: refs/heads/snapshots\n", 26)) {
		memory_error(m, err, "IO", "write HEAD");
		goto done;
	}
	ret = 0;
done:
	if (ret && created) {
		struct strbuf detail = STRBUF_INIT;
		strbuf_addf(&detail, "incomplete store retained at %s; move it aside before retrying", m->path);
		memory_error(m, err, "IO", detail.buf);
		strbuf_release(&detail);
	}
	rollback_lock_file(&init_lock);
	free(lock_path);
	free(path);
	free(worktree);
	free(gitdir);
	strbuf_release(&config);
	return ret;
}

struct store_config {
	struct memory_store *m;
	char *worktree, *gitdir;
	int invalid;
	unsigned int seen_limits;
};

static int config_value(const char *key, const char *value,
			const struct config_context *ctx UNUSED, void *data)
{
	struct store_config *c = data;
	unsigned long *limit = NULL, maximum = 0;
	unsigned int flag = 0;
	const char *digit;
	char *end;
	if (!value)
		return c->invalid = 1;
	if (!strcmp(key, "memory.worktree")) {
		if (c->worktree) return c->invalid = 1;
		c->worktree = xstrdup(value);
	} else if (!strcmp(key, "memory.gitdir")) {
		if (c->gitdir) return c->invalid = 1;
		c->gitdir = xstrdup(value);
	} else if (!strcmp(key, "memory.scope")) {
		string_list_append(&c->m->scopes, value);
	} else if (!strcmp(key, "memory.maxfile")) {
		limit = &c->m->max_file; maximum = MEMORY_MAX_FILE; flag = 1;
	} else if (!strcmp(key, "memory.maxtotal")) {
		limit = &c->m->max_total; maximum = MEMORY_MAX_TOTAL; flag = 2;
	} else if (!strcmp(key, "memory.maxfiles")) {
		limit = &c->m->max_files; maximum = MEMORY_MAX_FILES; flag = 4;
	}
	else if (!strcmp(key, "core.repositoryformatversion") ||
		 !strcmp(key, "core.bare") || !strcmp(key, "extensions.objectformat"))
		return 0;
	else return c->invalid = 1;
	if (limit) {
		if ((c->seen_limits & flag) || !*value)
			return c->invalid = 1;
		for (digit = value; *digit; digit++)
			if (*digit < '0' || *digit > '9')
				return c->invalid = 1;
		errno = 0;
		*limit = strtoul(value, &end, 10);
		if (errno || *end || !*limit || *limit > maximum)
			return c->invalid = 1;
		c->seen_limits |= flag;
	}
	return 0;
}

int memory_open(struct memory_store *m, struct repository *source, struct strbuf *err)
{
	struct store_config c = { 0 };
	char *worktree = NULL, *gitdir = NULL, *path = NULL;
	static const char *const dirs[] = { "objects", "objects/info", "objects/pack", "refs", "refs/heads" };
	size_t i;
	int ret = -1;
	if (setup_store(m, source, err)) return -1;
	if (!private_dir(m->path)) {
		memory_error(m, err, errno == ENOENT ? "STORE_MISSING" : "STORE_UNSAFE", m->path);
		goto done;
	}
	for (i = 0; i < ARRAY_SIZE(dirs); i++) {
		path = xstrfmt("%s/%s", m->path, dirs[i]);
		if (!private_dir(path)) {
			memory_error(m, err, "STORE_UNSAFE", path);
			goto done;
		}
		free(path); path = NULL;
	}
	path = xstrfmt("%s/objects/info/alternates", m->path);
	{
		struct stat st;
		if (!lstat(path, &st) || errno != ENOENT) {
			memory_error(m, err, "STORE_UNSAFE", "alternates");
			goto done;
		}
	}
	free(path); path = xstrfmt("%s/config", m->path);
	if (!private_file(path)) {
		memory_error(m, err, "STORE_UNSAFE", path);
		goto done;
	}
	m->max_file = MEMORY_MAX_FILE; m->max_total = MEMORY_MAX_TOTAL; m->max_files = MEMORY_MAX_FILES;
	c.m = m;
	if (git_config_from_file(config_value, path, &c) || c.invalid || !c.worktree || !c.gitdir ||
	    !m->scopes.nr || m->max_file > m->max_total) {
		memory_error(m, err, "CONFIG", "store config");
		goto done;
	}
	worktree = realpath(repo_get_work_tree(source), NULL);
	gitdir = realpath(repo_get_git_dir(source), NULL);
	if (!worktree || !gitdir || strcmp(worktree, c.worktree) || strcmp(gitdir, c.gitdir)) {
		memory_error(m, err, "STORE_FOREIGN", m->path);
		goto done;
	}
	for (i = 0; i < m->scopes.nr; i++)
		if (strcmp(m->scopes.items[i].string, ".") &&
		    !verify_path(m->scopes.items[i].string, S_IFREG)) {
			memory_error(m, err, "SCOPE", m->scopes.items[i].string);
			goto done;
		}
	free(path); path = xstrfmt("%s/HEAD", m->path);
	if (!private_file(path) || repo_init(&m->repo, m->path, NULL)) {
		memory_error(m, err, "STORE_UNSAFE", "invalid repository");
		goto done;
	}
	m->opened = 1;
	/* Seed the secondary config cache from only the validated store config. */
	if (!m->repo.config)
		CALLOC_ARRAY(m->repo.config, 1);
	else
		git_configset_clear(m->repo.config);
	git_configset_init(m->repo.config);
	free(path); path = xstrfmt("%s/config", m->path);
	if (git_configset_add_file(m->repo.config, path)) {
		memory_error(m, err, "CONFIG", "store config");
		goto done;
	}
	prepare_repo_settings(&m->repo);
	/* Never inherit user/global reference-transaction hooks in the private store. */
	free(m->repo.settings.hooks_path);
	m->repo.settings.hooks_path = xstrdup("/dev/null");
	if (m->repo.hash_algo != source->hash_algo) {
		memory_error(m, err, "STORE_FOREIGN", "hash format");
		goto done;
	}
	ret = 0;
done:
	free(path); free(worktree); free(gitdir); free(c.worktree); free(c.gitdir);
	return ret;
}

int memory_lock(struct memory_store *m, struct strbuf *err)
{
	char *path = xstrfmt("%s/capture", m->path);
	int fd = hold_lock_file_for_update_mode(&m->lock, path, 0, 0600);
	free(path);
	return fd < 0 ? memory_error(m, err, "LOCK", NULL) : 0;
}

void memory_unlock(struct memory_store *m)
{
	rollback_lock_file(&m->lock);
}

void memory_close(struct memory_store *m)
{
	memory_unlock(m);
	if (m->opened) repo_clear(&m->repo);
	string_list_clear(&m->scopes, 0);
	free(m->path);
	m->path = NULL;
	m->opened = 0;
}
