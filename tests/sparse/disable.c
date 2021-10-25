
#include <path.h>
#include <clar_libgit2.h>
#include <futils.h>
#include "sparse_helpers.h"

static git_repository *g_repo = NULL;

void test_sparse_disable__initialize(void)
{
}

void test_sparse_disable__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

void test_sparse_disable__disables_sparse_checkout(void)
{
	git_config *config;
	int b;

	git_sparse_checkout_init_options opts = GIT_SPARSE_CHECKOUT_INIT_OPTIONS_INIT;
	g_repo = cl_git_sandbox_init("sparse");

	cl_git_pass(git_sparse_checkout_init(&opts, g_repo));
	cl_git_pass(git_sparse_checkout_disable(g_repo));

	cl_git_pass(git_repository_config(&config, g_repo));
	cl_git_pass(git_config_get_bool(&b, config, "core.sparseCheckout"));
	cl_assert_equal_b(b, false);

	git_config_free(config);
}

void test_sparse_disable__leaves_sparse_checkout_file_intact(void)
{
	const char *path;

	git_str before_content = GIT_STR_INIT;
	git_str after_content = GIT_STR_INIT;

	path = "sparse/.git/info/sparse-checkout";
	g_repo = cl_git_sandbox_init("sparse");

	cl_git_pass(git_sparse_checkout_set_default(g_repo));
	cl_git_pass(git_futils_readbuffer(&before_content, path));

	cl_git_pass(git_sparse_checkout_disable(g_repo));
	cl_git_pass(git_futils_readbuffer(&after_content, path));

	cl_assert_(git_path_exists(path), path);
	cl_assert_equal_s_(git_str_cstr(&before_content), git_str_cstr(&after_content), "git_sparse_checkout_disable should not modify or remove the sparse-checkout file");
}
