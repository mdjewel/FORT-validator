#define _XOPEN_SOURCE 500

#include "cache/local_cache.h"

#include <dirent.h> /* opendir(), readdir(), closedir() */
#include <strings.h> /* strcasecmp */
#include <sys/types.h> /* opendir(), closedir(), stat() */
#include <sys/stat.h> /* stat() */
#include <sys/queue.h> /* STAILQ */
#include <unistd.h> /* stat() */
#include <time.h>
#include <jansson.h>

#include "alloc.h"
#include "file.h"
#include "log.h"
#include "data_structure/path_builder.h"
#include "data_structure/uthash.h"
#include "http/http.h"
#include "rsync/rsync.h"

/* FIXME needs locking */

/*
 * Have we ever attempted to download this directly?
 * Otherwise we actually downloaded a descendant.
 *
 * Directly downloaded nodes need to be retained, along with their ancestors.
 * If the download was successful, they should never have children (as this
 * would be redundant), though their directory counterparts probably will.
 */
#define CNF_DIRECT (1 << 0)
/* Has it downloaded successfully at some point? */
#define CNF_SUCCESS (1 << 1)
/* Has it been traversed during the current cleanup? */
#define CNF_FOUND (1 << 2)
/*
 * If enabled, node represents a file. Otherwise, node is a directory.
 * Only valid on HTTPs trees; we never know what rsync downloads.
 */
#define CNF_FILE (1 << 3)

struct cache_node {
	char *basename; /* Simple file name, parents not included */

	/* CNF_* */
	int flags;
	/*
	 * Last successful download timestamp.
	 * (Only if CNF_DIRECT & CNF_SUCCESS.)
	 * FIXME Intended to later decide whether a file should be deleted,
	 * when the cache is running out of space.
	 */
	time_t ts_success;
	/*
	 * Last download attempt timestamp. (Only if CNF_DIRECT.)
	 * Decides whether the file needs to be updated.
	 */
	time_t ts_attempt;
	/* Last download attempt's result status. (Only if CNF_DIRECT) */
	int error;

	struct cache_node *parent; /* Simple pointer */
	struct cache_node *children; /* Hash table */

	UT_hash_handle hh; /* Hash table hook */
};

static struct cache_node *rsync;
static struct cache_node *https;

static time_t startup_time; /* When we started the last validation */

static bool
is_root(struct cache_node *node)
{
	return node->parent == NULL;
}

/* Minimizes multiple evaluation */
static struct cache_node *
add_child(struct cache_node *parent, char const *basename)
{
	struct cache_node *child;
	char *key;
	size_t keylen;

	child = pzalloc(sizeof(struct cache_node));
	child->basename = pstrdup(basename);
	child->parent = parent;

	key = child->basename;
	keylen = strlen(key);

	HASH_ADD_KEYPTR(hh, parent->children, key, keylen, child);

	return child;
}

static struct cache_node *
init_root(struct cache_node *root, char const *name)
{
	if (root != NULL)
		return root;

	root = pzalloc(sizeof(struct cache_node));
	root->basename = pstrdup(name);

	return root;
}

/* FIXME recursive */
static void
delete_node(struct cache_node *node, bool force)
{
	struct cache_node *child, *tmp;

	HASH_ITER(hh, node->children, child, tmp)
		delete_node(child, force);

	if (force || !is_root(node)) {
		if (node->parent != NULL)
			HASH_DEL(node->parent->children, node);
		free(node->basename);
		free(node);
	}
}

static int
json_tt_value(struct json_t const *json, time_t *result)
{
	char const *str;
	struct tm tm;
	time_t tmp;

	if (json == NULL)
		return -1;
	str = json_string_value(json);
	if (str == NULL)
		return -1;
	str = strptime(str, "%FT%T%z", &tm);
	if (str == NULL || *str != 0)
		return -1;
	tmp = mktime(&tm);
	if (tmp == ((time_t) -1))
		return -1;

	*result = tmp;
	return 0;
}

static struct cache_node *
json2node(json_t *json, struct cache_node *parent)
{
	struct cache_node *node, *child;
	char const *string;
	json_t *jchild;
	size_t c;

	if (json == NULL)
		return NULL;

	node = pzalloc(sizeof(struct cache_node));

	string = json_string_value(json_object_get(json, "basename"));
	if (string == NULL) {
		pr_op_warn("Tag 'basename' of a metadata.json's download node cannot be parsed as a string; skipping.");
		goto cancel;
	}
	node->basename = pstrdup(string);

	jchild = json_object_get(json, "flags");
	if (!json_is_integer(jchild)) {
		pr_op_warn("Tag 'flags' of metadata.json's download node '%s' cannot be parsed as an integer; skipping.",
		    node->basename);
		goto cancel;
	}
	node->flags = json_integer_value(jchild);

	if (json_tt_value(json_object_get(json, "ts_success"), &node->ts_success)) {
		pr_op_warn("Tag 'success' of metadata.json's download node '%s' cannot be parsed as a date; skipping.",
		    node->basename);
		goto cancel;
	}

	if (json_tt_value(json_object_get(json, "ts_attempt"), &node->ts_attempt)) {
		pr_op_warn("Tag 'attempt' of metadata.json's download node '%s' cannot be parsed as a date; skipping.",
		    node->basename);
		goto cancel;
	}

	jchild = json_object_get(json, "error");
	if (!json_is_integer(jchild)) {
		pr_op_warn("Tag 'error' of metadata.json's download node '%s' cannot be parsed as an integer; skipping.",
		    node->basename);
		goto cancel;
	}
	node->error = json_integer_value(jchild);

	jchild = json_object_get(json, "children");
	if (jchild != NULL && !json_is_array(jchild)) {
		pr_op_warn("Tag 'children' of metadata.json's download node '%s' cannot be parsed as an array; skipping.",
		    node->basename);
		goto cancel;
	}

	for (c = 0; c < json_array_size(jchild); c++) {
		child = json2node(json_array_get(jchild, c), node);
		if (child == NULL)
			goto cancel;
		HASH_ADD_KEYPTR(hh, node->children, child->basename,
				strlen(child->basename), child);
	}

	node->parent = parent;
	pr_op_debug("Node '%s' successfully loaded from metadata.json.",
	    node->basename);
	return node;

cancel:
	delete_node(node, true);
	return NULL;
}

static void
load_metadata_json(void)
{
	/*
	 * Note: Loading metadata.json is one of few things Fort can fail at
	 * without killing itself. It's just a cache of a cache.
	 */

	struct path_builder pb;
	char *filename;
	json_t *root;
	json_error_t jerror;

	struct cache_node *node;
	size_t d;

	int error;

	path_init(&pb);
	path_append(&pb, config_get_local_repository());
	path_append(&pb, "metadata.json");
	error = path_compile(&pb, &filename);
	if (error) {
		pr_op_err("Unable to build metadata.json's path: %s",
		    strerror(error));
		goto end;
	}

	root = json_load_file(filename, 0, &jerror);

	free(filename);

	if (root == NULL) {
		pr_op_err("Json parsing failure at metadata.json (%d:%d): %s",
		    jerror.line, jerror.column, jerror.text);
		goto end;
	}
	if (json_typeof(root) != JSON_ARRAY) {
		pr_op_err("The root tag of metadata.json is not an array.");
		goto end;
	}

	for (d = 0; d < json_array_size(root); d++) {
		node = json2node(json_array_get(root, d), NULL);
		if (node == NULL)
			continue;
		else if (strcasecmp(node->basename, "rsync") == 0)
			rsync = node;
		else if (strcasecmp(node->basename, "https") == 0)
			https = node;
		else {
			pr_op_warn("Ignoring unrecognized json node '%s'.",
			    node->basename);
			delete_node(node, true);
		}
	}

end:
	json_decref(root);
	if (rsync == NULL)
		rsync = init_root(rsync, "rsync");
	if (https == NULL)
		https = init_root(https, "https");
}

void
cache_prepare(void)
{
	startup_time = time(NULL);
	if (startup_time == ((time_t) -1))
		pr_crit("time(NULL) returned -1");

	if (rsync == NULL)
		load_metadata_json();
}

static int
delete_node_file(struct cache_node *node, bool is_file)
{
	struct path_builder pb;
	struct cache_node *cursor;
	char *path;
	int error;

	path_init(&pb);
	for (cursor = node; cursor != NULL; cursor = cursor->parent)
		path_append(&pb, cursor->basename);
	path_append(&pb, config_get_local_repository());
	path_reverse(&pb);
	error = path_compile(&pb, &path);
	if (error) {
		pr_val_err("Cannot override '%s'; path is bogus: %s",
		    node->basename, strerror(error));
		return error;
	}

	if (is_file) {
		if (remove(path) != 0) {
			error = errno;
			pr_val_err("Cannot override file '%s': %s",
			    path, strerror(error));
		}
	} else {
		error = file_rm_rf(path);
		pr_val_err("Cannot override directory '%s': %s",
		    path, strerror(error));
	}

	free(path);
	return error;
}

static bool
was_recently_downloaded(struct cache_node *node)
{
	return (node->flags & CNF_DIRECT) && (startup_time <= node->ts_attempt);
}

static void destroy_tree(struct cache_node *);

/* FIXME recursive */
static void
drop_children(struct cache_node *node)
{
	struct cache_node *child, *tmp;

	HASH_ITER(hh, node->children, child, tmp)
		destroy_tree(child);
}

/**
 * @changed only on HTTP.
 */
int
cache_download(struct rpki_uri *uri, bool *changed)
{
	char *luri;
	char *token;
	char *saveptr;
	struct cache_node *node, *child;
	bool recursive;
	int error;

	if (changed != NULL)
		*changed = false;
	luri = pstrdup(uri_get_local(uri));
	token = strtok_r(luri, "/", &saveptr);

	switch (uri_get_type(uri)) {
	case UT_RSYNC:
		node = rsync;
		recursive = true;
		break;
	case UT_HTTPS:
		node = https;
		recursive = false;
		break;
	default:
		pr_crit("Unexpected URI type: %d", uri_get_type(uri));
	}

	while ((token = strtok_r(NULL, "/", &saveptr)) != NULL) {
		if (node->flags & CNF_FILE) {
			/* node used to be a file, now it's a dir. */
			delete_node_file(node, true);
			node->flags = 0;
		}

		HASH_FIND_STR(node->children, token, child);

		if (child == NULL) {
			/* Create child */
			do {
				node = add_child(node, token);
				token = strtok_r(NULL, "/", &saveptr);
			} while (token != NULL);
			goto download;

		} else if (recursive) {
			if (was_recently_downloaded(child) && !child->error) {
				error = 0;
				goto end;
			}

		}

		node = child;
	}

	if (was_recently_downloaded(node)) {
		error = node->error;
		goto end;
	}

	if (!recursive && !(node->flags & CNF_FILE)) {
		/* node used to be a dir, now it's a file. */
		delete_node_file(node, false);
	}

download:
	switch (uri_get_type(uri)) {
	case UT_RSYNC:
		error = rsync_download(uri);
		break;
	case UT_HTTPS:
		error = http_download(uri, changed);
		break;
	default:
		pr_crit("Unexpected URI type: %d", uri_get_type(uri));
	}

	node->error = error;
	node->flags = CNF_DIRECT;
	node->ts_attempt = time(NULL);
	if (node->ts_attempt == ((time_t) -1))
		pr_crit("time(NULL) returned -1");
	if (!error) {
		node->flags |= CNF_SUCCESS | (recursive ? 0 : CNF_FILE);
		node->ts_success = node->ts_attempt;
	}
	drop_children(node);

end:
	free(luri);
	return error;
}

static void
path_rm_rf(struct path_builder *pb, char const *filename)
{
	char const *path;
	int error;

	error = path_peek(pb, &path);
	if (error) {
		pr_op_err("Path builder error code %d; cannot delete directory. (Basename is '%s')",
		    error, filename);
		return;
	}

	error = file_rm_rf(path);
	if (error)
		pr_op_err("Cannot delete %s: %s", path, strerror(error));
}

/* FIXME recursive */
static void
cleanup_recursive(struct cache_node *node, struct path_builder *pb)
{
	char const *path;
	struct stat meta;
	DIR *dir;
	struct dirent *file;
	struct cache_node *child, *tmp;
	int error;

	/* FIXME We need to recover from path too long... */
	path_append(pb, node->basename);
	error = path_peek(pb, &path);
	if (error) {
		pr_op_err("Cannot clean up directory (basename is '%s'): %s",
		    node->basename, strerror(error));
		goto end;
	}

	if (stat(path, &meta) != 0) {
		error = errno;
		if (error == ENOENT) {
			/* Node exists but file doesn't: Delete node */
			delete_node(node, false);
			goto end;
		}

		pr_op_err("Cannot clean up '%s'; stat() returned errno %d: %s",
		    path, error, strerror(error));
		goto end;
	}

	if (was_recently_downloaded(node) && !node->error)
		goto end; /* Node is active (ie. used recently): Keep it. */

	/*
	 * From now on, file exists but node is stale.
	 * We'll aim to delete both.
	 */

	if (S_ISREG(meta.st_mode)) {
		/* Both node and file exist, but inactive: Delete */
		remove(path);
		delete_node(node, false);

	} else if (S_ISDIR(meta.st_mode)) {
		dir = opendir(path);
		if (dir == NULL) {
			error = errno;
			pr_op_err("Cannot clean up '%s'; S_ISDIR() but !opendir(): %s",
			    path, strerror(error));
			goto end;
		}

		/*
		 * Directory exists but node is stale.
		 * A child might be fresh, so recurse.
		 */

		FOREACH_DIR_FILE(dir, file) {
			if (S_ISDOTS(file))
				continue;

			HASH_FIND_STR(node->children, file->d_name, child);
			if (child != NULL) {
				child->flags |= CNF_FOUND;
				/* File child's node does exist: Recurse. */
				cleanup_recursive(child, pb);
			} else {
				/* File child's node does not exist: Delete. */
				path_append(pb, file->d_name);
				path_rm_rf(pb, file->d_name);
				path_pop(pb, true);
			}

		}
		error = errno;
		closedir(dir);
		if (error) {
			pr_op_err("Cannot clean up directory (basename is '%s'): %s",
			    node->basename, strerror(error));
			goto end;
		}

		HASH_ITER(hh, node->children, child, tmp) {
			if (child->flags & CNF_FOUND) {
				/*
				 * File child still exists, which means there's
				 * at least one active descendant.
				 * Clean the flag and keep the node.
				 */
				child->flags &= ~CNF_FOUND;
			} else {
				/* Node child's file does not exist: Delete. */
				delete_node(child, false);
			}
		}

		if (node->children == NULL && !is_root(node)) {
			/* Node is inactive and we rm'd its children: Delete. */
			path_rm_rf(pb, node->basename);
			delete_node(node, false);
		}

	} else {
		/* Outdated, not file nor directory: Delete. */
		remove(path);
		delete_node(node, false);
	}

end:
	path_pop(pb, true);
}

static int
tt2json(time_t tt, json_t **result)
{
	char str[32];
	struct tm tmbuffer, *tm;

	memset(&tmbuffer, 0, sizeof(tmbuffer));
	tm = localtime_r(&tt, &tmbuffer);
	if (tm == NULL)
		return errno;
	if (strftime(str, sizeof(str) - 1, "%FT%T%z", tm) == 0)
		return ENOSPC;

	*result = json_string(str);
	return 0;
}

/* FIXME recursive */
static json_t *
node2json(struct cache_node *node)
{
	json_t *json, *date, *children, *jchild;
	struct cache_node *child, *tmp;
	int error;

	json = json_object();
	if (json == NULL) {
		pr_op_err("json object allocation failure.");
		return NULL;
	}

	if (json_object_set_new(json, "basename", json_string(node->basename))) {
		pr_op_err("Cannot convert string '%s' to json; unknown cause.",
		    node->basename);
		goto cancel;
	}

	if (json_object_set_new(json, "flags", json_integer(node->flags))) {
		pr_op_err("Cannot convert int '%d' to json; unknown cause.",
		    node->flags);
		goto cancel;
	}

	error = tt2json(node->ts_success, &date);
	if (error) {
		pr_op_err("Cannot convert timestamp %ld to json: %s",
		    node->ts_success, strerror(error));
		goto cancel;
	}
	if (json_object_set_new(json, "ts_success", date)) {
		pr_op_err("Cannot convert timestamp %ld to json; unknown cause.",
		    node->ts_success);
		goto cancel;
	}

	error = tt2json(node->ts_attempt, &date);
	if (error) {
		pr_op_err("Cannot convert timestamp %ld to json: %s",
		    node->ts_attempt, strerror(error));
		goto cancel;
	}
	if (json_object_set_new(json, "ts_attempt", date)) {
		pr_op_err("Cannot convert timestamp %ld to json; unknown cause.",
		    node->ts_attempt);
		goto cancel;
	}

	if (json_object_set_new(json, "error", json_integer(node->error))) {
		pr_op_err("Cannot convert int '%d' to json; unknown cause.",
		    node->error);
		goto cancel;
	}

	if (node->children != NULL) {
		children = json_array();
		if (children == NULL) {
			pr_op_err("json array allocation failure.");
			return NULL;
		}

		if (json_object_set_new(json, "children", children)) {
			pr_op_err("Cannot push children array into json node; unknown cause.");
			goto cancel;
		}

		HASH_ITER(hh, node->children, child, tmp) {
			jchild = node2json(child);
			if (jchild == NULL)
				goto cancel; /* Error msg already printed */
			if (json_array_append(children, jchild)) {
				pr_op_err("Cannot push child into json node; unknown cause.");
				goto cancel;
			}
		}
	}

	return json;

cancel:
	json_decref(json);
	return NULL;
}

static int
append_node(json_t *root, struct cache_node *node, char const *name)
{
	json_t *child;

	child = node2json(node);
	if (child == NULL)
		return -1;
	if (json_array_append(root, child)) {
		pr_op_err("Cannot push %s json node into json root; unknown cause.",
		    name);
		return -1;
	}

	return 0;
}

static json_t *
build_metadata_json(void)
{
	json_t *root;

	root = json_array();
	if (root == NULL) {
		pr_op_err("json root allocation failure.");
		return NULL;
	}

	if (append_node(root, rsync, "rsync")
	    || append_node(root, https, "https")) {
		json_decref(root);
		return NULL;
	}

	return root;
}

static void
write_metadata_json(char const *filename)
{
	struct json_t *json;

	json = build_metadata_json();
	if (json == NULL)
		return;

	if (json_dump_file(json, filename, JSON_COMPACT))
		pr_op_err("Unable to write metadata.json; unknown cause.");

	json_decref(json);
}

void
cache_cleanup(void)
{
	struct path_builder pb;
	char const *json_filename;
	int error;

	path_init(&pb);
	path_append(&pb, config_get_local_repository());

	cleanup_recursive(rsync, &pb);
	cleanup_recursive(https, &pb);

	path_append(&pb, "metadata.json");
	error = path_peek(&pb, &json_filename);
	if (error)
		pr_op_err("Cannot create metadata.json: %s", strerror(error));
	else
		write_metadata_json(json_filename);

	path_cancel(&pb);
}

/* FIXME recursive */
static void
destroy_tree(struct cache_node *node)
{
	if (node == NULL)
		return;

	free(node->basename);
	drop_children(node);
	if (node->parent != NULL)
		HASH_DEL(node->parent->children, node);
	free(node);
}

void
cache_teardown(void)
{
	destroy_tree(rsync);
	destroy_tree(https);
}