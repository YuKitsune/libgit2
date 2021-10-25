/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "sparse.h"
#include "attrcache.h"
#include "git2/sparse.h"
#include "config.h"
#include "filebuf.h"

static bool sparse_lookup_in_rules(
        int *checkout,
        git_attr_file *file,
        git_attr_path *path)
{
	size_t j;
	git_attr_fnmatch *match;
	
	git_vector_rforeach(&file->rules, j, match) {
		if (match->flags & GIT_ATTR_FNMATCH_DIRECTORY &&
				path->is_dir == GIT_DIR_FLAG_FALSE)
			continue;
		if (git_attr_fnmatch__match(match, path)) {
			*checkout = ((match->flags & GIT_ATTR_FNMATCH_NEGATIVE) == 0) ?
			GIT_SPARSE_CHECKOUT : GIT_SPARSE_NOCHECKOUT;
			return true;
		}
	}
	
	return false;
}

static int parse_sparse_file(
        git_repository *repo,
        git_attr_file *attrs,
        const char *data,
        bool allow_macros)
{
	int error = 0;
	int ignore_case = false;
	const char *scan = data;
	git_attr_fnmatch *match = NULL;
	GIT_UNUSED(allow_macros);
	
	if (git_repository__configmap_lookup(&ignore_case, repo, GIT_CONFIGMAP_IGNORECASE) < 0)
		git_error_clear();
	
	if (git_mutex_lock(&attrs->lock)) {
		git_error_set(GIT_ERROR_OS, "failed to lock sparse-checkout file");
		return -1;
	}
	
	while (!error && *scan) {
		int valid_rule = 1;
		
		if (!match && !(match = git__calloc(1, sizeof(*match)))) {
			error = -1;
			break;
		}
		
		match->flags =
		GIT_ATTR_FNMATCH_ALLOWSPACE | GIT_ATTR_FNMATCH_ALLOWNEG;
		
		if (!(error = git_attr_fnmatch__parse(
			match, &attrs->pool, NULL, &scan)))
		{
			match->flags |= GIT_ATTR_FNMATCH_IGNORE;
			
			if (ignore_case)
				match->flags |= GIT_ATTR_FNMATCH_ICASE;
			
			scan = git__next_line(scan);
			
			/*
			 * If a negative match doesn't actually do anything,
			 * throw it away. As we cannot always verify whether a
			 * rule containing wildcards negates another rule, we
			 * do not optimize away these rules, though.
			 * */
			if (match->flags & GIT_ATTR_FNMATCH_NEGATIVE
					&& !(match->flags & GIT_ATTR_FNMATCH_HASWILD))
				error = git_attr__does_negate_rule(&valid_rule, &attrs->rules, match);
			
			if (!error && valid_rule)
				error = git_vector_insert(&attrs->rules, match);
		}
		
		if (error != 0 || !valid_rule) {
			match->pattern = NULL;
			
			if (error == GIT_ENOTFOUND)
				error = 0;
		} else {
			match = NULL; /* vector now "owns" the match */
		}
	}
	
	git_mutex_unlock(&attrs->lock);
	git__free(match);
	
	return error;
}

int git_sparse_attr_file__init(
		int *file_exists,
        git_repository *repo,
        git_sparse *sparse,
        git_str *infopath)
{
    int error = 0;
    const char *filename = GIT_SPARSE_CHECKOUT_FILE;
    git_attr_file_source source = { GIT_ATTR_FILE_SOURCE_FILE, git_str_cstr(infopath), filename, NULL };
    git_str filepath = GIT_STR_INIT;

    git_str_joinpath(&filepath, infopath->ptr, filename);

    /* Don't overwrite any existing sparse-checkout file */
	*file_exists = git_path_exists(git_str_cstr(&filepath));
    if (!*file_exists) {
        if ((error = git_futils_creat_withpath(git_str_cstr(&filepath), 0777, 0666)) < 0)
            return error;
    }

    error = git_attr_cache__get(&sparse->sparse, repo, NULL, &source, parse_sparse_file, false);

    return error;
}

int git_sparse__init(
		git_repository *repo,
		git_sparse *sparse)
{
	int b = false;
	int error = git_sparse__init_(&b, repo, sparse);
	return error;
}

int git_sparse__init_(
		int *file_exists,
        git_repository *repo,
        git_sparse *sparse)
{
	int error = 0;
	git_str infopath = GIT_STR_INIT;
	
	assert(repo && sparse);
	
	memset(sparse, 0, sizeof(*sparse));
	sparse->repo = repo;
	
	/* Read the ignore_case flag */
	if ((error = git_repository__configmap_lookup(
			&sparse->ignore_case, repo, GIT_CONFIGMAP_IGNORECASE)) < 0)
		goto cleanup;
	
	if ((error = git_attr_cache__init(repo)) < 0)
		goto cleanup;
	
	/* load .git/info/sparse_checkout if possible */
    if ((error = git_repository__item_path(&infopath, repo, GIT_REPOSITORY_ITEM_INFO)) < 0) {
        if (error != GIT_ENOTFOUND)
            goto cleanup;
        error = 0;
    }

    if ((error = git_sparse_attr_file__init(file_exists, repo, sparse, &infopath)) < 0) {
        if (error != GIT_ENOTFOUND)
            goto cleanup;
        error = 0;
    }
	
cleanup:
	git_str_dispose(&infopath);
	if (error < 0)
		git_sparse__free(sparse);
	
	return error;
}

int git_sparse__lookup(
        int* checkout,
        git_sparse* sparse,
        const char* pathname,
        git_dir_flag dir_flag)
{
	git_attr_path path;
	const char *workdir;
	int error;
	
	assert(checkout && pathname && sparse);
	
	*checkout = GIT_SPARSE_CHECKOUT;
	
	workdir = git_repository_workdir(sparse->repo);
	if ((error = git_attr_path__init(&path, pathname, workdir, dir_flag)))
		return -1;
	
	/* No match -> no checkout */
	*checkout = GIT_SPARSE_NOCHECKOUT;
	
	while (1) {
		if (sparse_lookup_in_rules(checkout, sparse->sparse, &path))
			goto cleanup;
		
		/* move up one directory */
		if (path.basename == path.path)
			break;
		path.basename[-1] = '\0';
		while (path.basename > path.path && *path.basename != '/')
			path.basename--;
		if (path.basename > path.path)
			path.basename++;
		path.is_dir = 1;
	}

cleanup:
	git_attr_path__free(&path);
	return 0;
}

void git_sparse__free(git_sparse *sparse)
{
	git_attr_file__free(sparse->sparse);
}

int git_sparse_checkout__list(
        git_vector *patterns,
        git_sparse *sparse)
{
    int error = 0;
    git_str data = GIT_STR_INIT;
    char *scan, *buf;

    GIT_ASSERT_ARG(patterns);
    GIT_ASSERT_ARG(sparse);

    if ((error = git_futils_readbuffer(&data, sparse->sparse->entry->fullpath)) < 0)
        return error;

    scan = (char *)git_str_cstr(&data);
    while (!error && *scan) {

		buf = git__strtok_(&scan, "\r\n", "\n", "\0");
		if (buf)
			error = git_vector_insert(patterns, buf);
    }

    return error;
}

int git_sparse_checkout_list(git_strarray *patterns, git_repository *repo) {

    int error = 0;
	int b;
    git_sparse sparse;
    git_vector patternlist;

    GIT_ASSERT_ARG(patterns);
    GIT_ASSERT_ARG(repo);

    if ((error = git_sparse__init(repo, &sparse)))
        goto done;

    if ((error = git_vector_init(&patternlist, 0, NULL)) < 0)
        goto done;

    if ((error = git_sparse_checkout__list(&patternlist, &sparse)))
        goto done;

    patterns->strings = (char **) git_vector_detach(&patterns->count, NULL, &patternlist);

done:
    git_sparse__free(&sparse);
    git_vector_free(&patternlist);


    return error;
}

int git_sparse_checkout__set(
		git_vector *patterns,
		git_sparse *sparse)
{
	int error = 0;
	size_t i = 0;
	const char *pattern;

	git_str content = GIT_STR_INIT;

	GIT_ASSERT_ARG(patterns);
	GIT_ASSERT_ARG(sparse);

	git_vector_foreach(patterns, i, pattern) {
		git_str_join(&content, '\n', git_str_cstr(&content), pattern);
	}

	if ((error = git_futils_truncate(sparse->sparse->entry->fullpath, 0777)) < 0)
		goto done;

	if ((error = git_futils_writebuffer(&content, sparse->sparse->entry->fullpath, O_WRONLY, 0644)) < 0)
		goto done;

done:
	git_str_dispose(&content);

	return error;
}

int git_sparse_checkout_init(git_sparse_checkout_init_options *opts, git_repository *repo) {

    int error = 0;
    git_config *cfg;
    git_sparse sparse;
	int file_exists = false;
	git_vector default_patterns = GIT_VECTOR_INIT;

    GIT_ASSERT_ARG(opts);
    GIT_ASSERT_ARG(repo);

    if ((error = git_repository_config__weakptr(&cfg, repo)) < 0)
        return error;

    if ((error = git_config_set_bool(cfg, "core.sparseCheckout", true)) < 0)
        return error;

    if ((error = git_sparse__init_(&file_exists, repo, &sparse)) < 0)
        goto cleanup;

	if (!file_exists) {

		/* Default patterns that match every file in the root directory and no other directories */
		git_vector_insert(&default_patterns, "/*");
		git_vector_insert(&default_patterns, "!/*/");

		if ((error = git_sparse_checkout__set(&default_patterns, &sparse)) < 0)
			goto cleanup;
	}

cleanup:
    git_config_free(cfg);
    git_sparse__free(&sparse);
    return error;
}

int git_sparse_checkout_set(
        git_strarray *patterns,
        git_repository *repo)
{
    int err = 0;
    int is_enabled = false;
    git_config *cfg;
    git_sparse sparse;

    size_t i = 0;
    git_vector patternlist;

    if ((err = git_repository_config(&cfg, repo)) < 0)
        goto done;

    err = git_config_get_bool(&is_enabled, cfg, "core.sparseCheckout");
    if (err < 0 && err != GIT_ENOTFOUND)
        goto done;

    if (!is_enabled) {
        git_sparse_checkout_init_options opts = GIT_SPARSE_CHECKOUT_INIT_OPTIONS_INIT;
        if ((err = git_sparse_checkout_init(&opts, repo) < 0))
            goto done;
    }

    if ((err = git_sparse__init(repo, &sparse)) < 0)
        goto done;

    if ((err = git_vector_init(&patternlist, 0, NULL)) < 0)
        goto done;

    for (i = 0; i < patterns->count; i++) {
        git_vector_insert(&patternlist, patterns->strings[i]);
    }

    if ((err = git_sparse_checkout__set(&patternlist, &sparse)) < 0)
        goto done;

done:
    git_config_free(cfg);
    git_sparse__free(&sparse);
    git_vector_free(&patternlist);

    return err;
}

int git_sparse_checkout_disable(git_repository *repo)
{
    int error = 0;
    git_config *cfg;

    GIT_ASSERT_ARG(repo);

    if ((error = git_repository_config(&cfg, repo)) < 0)
        return error;

    if ((error = git_config_set_bool(cfg, "core.sparseCheckout", false)) < 0)
        goto done;

    /* Todo: restore working directory */

done:
    git_config_free(cfg);

    return error;
}


int git_sparse_checkout__add(
        git_vector *patterns,
        git_sparse *sparse)
{
    int error = 0;
    size_t i = 0;
    git_vector existing_patterns;
    git_vector new_patterns;
    char* pattern;

    GIT_ASSERT_ARG(patterns);
    GIT_ASSERT_ARG(sparse);

    if ((error = git_vector_init(&existing_patterns, 0, NULL)) < 0)
        goto done;

    if ((error = git_vector_init(&new_patterns, 0, NULL)) < 0)
        goto done;

    if ((error = git_sparse_checkout__list(&existing_patterns, sparse)) < 0)
        goto done;

    git_vector_foreach(&existing_patterns, i, pattern) {
        git_vector_insert(&new_patterns, pattern);
    }

    git_vector_foreach(patterns, i, pattern) {
        git_vector_insert(&new_patterns, pattern);
    }

    if ((error = git_sparse_checkout__set(&new_patterns, sparse)) < 0)
        goto done;

done:
    git_vector_free(&existing_patterns);
    git_vector_free(&new_patterns);

    return error;
}

int git_sparse_checkout_add(
        git_strarray *patterns,
        git_repository *repo)
{
    int err = 0;
    int is_enabled = false;
    git_config *cfg;
    git_sparse sparse;
    git_vector patternlist;

    if ((err = git_repository_config__weakptr(&cfg, repo)) < 0)
        return err;

    err = git_config_get_bool(&is_enabled, cfg, "core.sparseCheckout");
    if (err < 0 && err != GIT_ENOTFOUND)
        goto done;

    /* Todo: verify what git does when adding and sparse-checkout isn't enabled */
    if (!is_enabled)
        goto done;

    if ((err = git_sparse__init(repo, &sparse)) < 0)
        goto done;

    if ((err = git_vector_init(&patternlist, 0, NULL)))
        goto done;

    if ((err = git_strarray__to_vector(&patternlist, patterns)))
        goto done;

    if ((err = git_sparse_checkout__add(&patternlist, &sparse)) < 0)
        goto done;

done:
    git_config_free(cfg);
    git_sparse__free(&sparse);
    git_vector_free(&patternlist);

    return err;
}

int git_sparse_check_path(
        int *checkout,
        git_repository *repo,
        const char *pathname)
{
    int error;
    int sparse_checkout_enabled = false;
    git_sparse sparse;
    git_dir_flag dir_flag = GIT_DIR_FLAG_FALSE;

    assert(repo && checkout && pathname);

    *checkout = GIT_SPARSE_CHECKOUT;

    if ((error = git_repository__configmap_lookup(&sparse_checkout_enabled, repo, GIT_CONFIGMAP_SPARSECHECKOUT)) < 0 ||
        sparse_checkout_enabled == false)
        return 0;

    if ((error = git_sparse__init(repo, &sparse)) < 0)
        goto cleanup;

    if (!git__suffixcmp(pathname, "/"))
        dir_flag = GIT_DIR_FLAG_TRUE;
    else if (git_repository_is_bare(repo))
        dir_flag = GIT_DIR_FLAG_FALSE;

    error = git_sparse__lookup(checkout, &sparse, pathname, dir_flag);

    cleanup:
    git_sparse__free(&sparse);
    return error;
}

int git_strarray__to_vector(git_vector *dest, git_strarray *src) {
    int error = 0;
    size_t i = 0;

    GIT_ASSERT_ARG(dest);
    GIT_ASSERT_ARG(src);

    for (i = 0; i < src->count; i++) {
        if ((error = git_vector_insert(dest, src->strings[i])) < 0)
            return error;
    }

    return error;
}
