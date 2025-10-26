#include "git-compat-util.h"
#include "config.h"
#include "repo-settings.h"
#include "repository.h"
#include "midx.h"
#include "pack-objects.h"
#include "setup.h"
#include "parallel-compress.h"

static void repo_cfg_bool(struct repository *r, const char *key, int *dest,
			  int def)
{
	if (repo_config_get_bool(r, key, dest))
		*dest = def;
}

static void repo_cfg_int(struct repository *r, const char *key, int *dest,
			 int def)
{
	if (repo_config_get_int(r, key, dest))
		*dest = def;
}

static void repo_cfg_ulong(struct repository *r, const char *key, unsigned long *dest,
			   unsigned long def)
{
	if (repo_config_get_ulong(r, key, dest))
		*dest = def;
}

void prepare_repo_settings(struct repository *r)
{
	int experimental;
	int value;
	const char *strval;
	int manyfiles;
	int read_changed_paths;
	unsigned long ulongval;

	if (!r->gitdir)
		BUG("Cannot add settings for uninitialized repository");

	if (r->settings.initialized)
		return;

	repo_settings_clear(r);
	r->settings.initialized++;

	/* Booleans config or default, cascades to other settings */
	repo_cfg_bool(r, "feature.manyfiles", &manyfiles, 0);
	repo_cfg_bool(r, "feature.experimental", &experimental, 0);

	/* Defaults modified by feature.* */
	if (experimental) {
		r->settings.fetch_negotiation_algorithm = FETCH_NEGOTIATION_SKIPPING;
		r->settings.pack_use_bitmap_boundary_traversal = 1;
		r->settings.pack_use_multi_pack_reuse = 1;
		r->settings.pack_use_path_walk = 1;
	}
	if (manyfiles) {
		r->settings.index_version = 4;
		r->settings.index_skip_hash = 1;
		r->settings.core_untracked_cache = UNTRACKED_CACHE_WRITE;
		r->settings.pack_use_path_walk = 1;
	}

	/* Commit graph config or default, does not cascade (simple) */
	repo_cfg_bool(r, "core.commitgraph", &r->settings.core_commit_graph, 1);
	repo_cfg_int(r, "commitgraph.generationversion", &r->settings.commit_graph_generation_version, 2);
	repo_cfg_bool(r, "commitgraph.readchangedpaths", &read_changed_paths, 1);
	repo_cfg_int(r, "commitgraph.changedpathsversion",
		     &r->settings.commit_graph_changed_paths_version,
		     read_changed_paths ? -1 : 0);
	repo_cfg_bool(r, "gc.writecommitgraph", &r->settings.gc_write_commit_graph, 1);
	repo_cfg_bool(r, "fetch.writecommitgraph", &r->settings.fetch_write_commit_graph, 0);

	/* Boolean config or default, does not cascade (simple)  */
	repo_cfg_bool(r, "pack.usesparse", &r->settings.pack_use_sparse, 1);
	repo_cfg_bool(r, "pack.usepathwalk", &r->settings.pack_use_path_walk, 0);
	repo_cfg_bool(r, "core.multipackindex", &r->settings.core_multi_pack_index, 1);
	repo_cfg_bool(r, "index.sparse", &r->settings.sparse_index, 0);
	repo_cfg_bool(r, "index.skiphash", &r->settings.index_skip_hash, r->settings.index_skip_hash);
	repo_cfg_bool(r, "pack.readreverseindex", &r->settings.pack_read_reverse_index, 1);
	repo_cfg_bool(r, "pack.usebitmapboundarytraversal",
		      &r->settings.pack_use_bitmap_boundary_traversal,
		      r->settings.pack_use_bitmap_boundary_traversal);
	repo_cfg_bool(r, "core.usereplacerefs", &r->settings.read_replace_refs, 1);

	/*
	 * The GIT_TEST_MULTI_PACK_INDEX variable is special in that
	 * either it *or* the config sets
	 * r->settings.core_multi_pack_index if true. We don't take
	 * the environment variable if it exists (even if false) over
	 * any config, as in most other cases.
	 */
	if (git_env_bool(GIT_TEST_MULTI_PACK_INDEX, 0))
		r->settings.core_multi_pack_index = 1;

	/*
	 * Non-boolean config
	 */
	if (!repo_config_get_int(r, "index.version", &value))
		r->settings.index_version = value;

	if (!repo_config_get_string_tmp(r, "core.untrackedcache", &strval)) {
		int v = git_parse_maybe_bool(strval);

		/*
		 * If it's set to "keep", or some other non-boolean
		 * value then "v < 0". Then we do nothing and keep it
		 * at the default of UNTRACKED_CACHE_KEEP.
		 */
		if (v >= 0)
			r->settings.core_untracked_cache = v ?
				UNTRACKED_CACHE_WRITE : UNTRACKED_CACHE_REMOVE;
	}

	if (!repo_config_get_string_tmp(r, "fetch.negotiationalgorithm", &strval)) {
		int fetch_default = r->settings.fetch_negotiation_algorithm;
		if (!strcasecmp(strval, "skipping"))
			r->settings.fetch_negotiation_algorithm = FETCH_NEGOTIATION_SKIPPING;
		else if (!strcasecmp(strval, "noop"))
			r->settings.fetch_negotiation_algorithm = FETCH_NEGOTIATION_NOOP;
		else if (!strcasecmp(strval, "consecutive"))
			r->settings.fetch_negotiation_algorithm = FETCH_NEGOTIATION_CONSECUTIVE;
		else if (!strcasecmp(strval, "default"))
			r->settings.fetch_negotiation_algorithm = fetch_default;
		else
			die("unknown fetch negotiation algorithm '%s'", strval);
	}

	/*
	 * This setting guards all index reads to require a full index
	 * over a sparse index. After suitable guards are placed in the
	 * codebase around uses of the index, this setting will be
	 * removed.
	 */
	r->settings.command_requires_full_index = 1;

	if (!repo_config_get_ulong(r, "core.deltabasecachelimit", &ulongval))
		r->settings.delta_base_cache_limit = ulongval;

	if (!repo_config_get_ulong(r, "core.packedgitwindowsize", &ulongval)) {
		int pgsz_x2 = getpagesize() * 2;

		/* This value must be multiple of (pagesize * 2) */
		ulongval /= pgsz_x2;
		if (ulongval < 1)
			ulongval = 1;
		r->settings.packed_git_window_size = ulongval * pgsz_x2;
	}

	if (!repo_config_get_ulong(r, "core.packedgitlimit", &ulongval))
		r->settings.packed_git_limit = ulongval;
}

void repo_settings_clear(struct repository *r)
{
	struct repo_settings empty = REPO_SETTINGS_INIT;
	FREE_AND_NULL(r->settings.fsmonitor);
	FREE_AND_NULL(r->settings.hooks_path);
	r->settings = empty;
}

unsigned long repo_settings_get_big_file_threshold(struct repository *repo)
{
	if (!repo->settings.big_file_threshold)
		repo_cfg_ulong(repo, "core.bigfilethreshold",
			       &repo->settings.big_file_threshold, 512 * 1024 * 1024);
	return repo->settings.big_file_threshold;
}

void repo_settings_set_big_file_threshold(struct repository *repo, unsigned long value)
{
	repo->settings.big_file_threshold = value;
}

/*
 * Content-defined chunking configuration
 *
 * These settings control how large files are split into chunks:
 *   - chunk.minSize: Minimum chunk size (prevents excessive fragmentation)
 *   - chunk.targetSize: Target average chunk size (controls mask granularity)
 *   - chunk.maxSize: Maximum chunk size (prevents pathologically large chunks)
 *
 * Defaults based on genomics file research:
 *   - min: 2 MB (prevents fragmentation, aligns with BorgBackup)
 *   - target: 16 MB (optimal for 50-200 GB genomics files)
 *   - max: 64 MB (reasonable upper bound, memory-safe)
 *
 * Users can configure via:
 *   bench config chunk.minSize 4m
 *   bench config chunk.targetSize 32m
 *   bench config chunk.maxSize 128m
 */

unsigned long repo_settings_get_chunk_min_size(struct repository *repo)
{
	if (!repo->settings.chunk_min_size)
		repo_cfg_ulong(repo, "chunk.minsize",
			       &repo->settings.chunk_min_size, 2 * 1024 * 1024);
	return repo->settings.chunk_min_size;
}

unsigned long repo_settings_get_chunk_target_size(struct repository *repo)
{
	if (!repo->settings.chunk_target_size)
		repo_cfg_ulong(repo, "chunk.targetsize",
			       &repo->settings.chunk_target_size, 16 * 1024 * 1024);
	return repo->settings.chunk_target_size;
}

unsigned long repo_settings_get_chunk_max_size(struct repository *repo)
{
	if (!repo->settings.chunk_max_size)
		repo_cfg_ulong(repo, "chunk.maxsize",
			       &repo->settings.chunk_max_size, 64 * 1024 * 1024);
	return repo->settings.chunk_max_size;
}

void repo_settings_set_chunk_min_size(struct repository *repo, unsigned long value)
{
	repo->settings.chunk_min_size = value;
}

void repo_settings_set_chunk_target_size(struct repository *repo, unsigned long value)
{
	repo->settings.chunk_target_size = value;
}

void repo_settings_set_chunk_max_size(struct repository *repo, unsigned long value)
{
	repo->settings.chunk_max_size = value;
}

/*
 * Parallel compression configuration
 *
 * These settings control parallel zlib compression:
 *   - core.compressionThreads: Number of worker threads for compression
 *   - core.compressionQueueMemory: Maximum memory (MB) for compression queue
 *
 * Defaults:
 *   - threads: Half of available CPUs (leaves headroom for I/O)
 *   - queue memory: 1/8 of total system memory (balances performance and memory usage)
 *
 * Users can configure via:
 *   bench config core.compressionThreads 8
 *   bench config core.compressionQueueMemory 1024
 */

unsigned int repo_settings_get_compression_threads(struct repository *repo)
{
	if (!repo->settings.compression_threads) {
		int value;
		if (!repo_config_get_int(repo, "core.compressionthreads", &value) &&
		    value > 0) {
			repo->settings.compression_threads = (unsigned int)value;
		} else {
			/*
			 * Auto-detect optimal thread count based on:
			 * 1. Available CPU cores (use half)
			 * 2. Available memory (queue must hold 2x chunks per thread)
			 *
			 * Formula: threads = min(cpus/2, queue_memory/(max_chunk_size*2))
			 */
			int ncpus = online_cpus();
			unsigned int cpu_threads = ncpus > 1 ? ncpus / 2 : 1;

			/* Get queue memory (auto-detects to 1/8 total RAM) */
			unsigned long queue_memory_mb = repo_settings_get_compression_queue_memory(repo);
			unsigned long max_chunk_mb = repo_settings_get_chunk_max_size(repo) / (1024 * 1024);

			/* How many threads can queue_memory support? (2x chunks per thread) */
			unsigned int memory_threads = (unsigned int)(queue_memory_mb / (max_chunk_mb * 2));
			if (memory_threads < 1)
				memory_threads = 1;

			/* Use smaller of CPU-based vs memory-based limit */
			repo->settings.compression_threads = cpu_threads < memory_threads
			                                     ? cpu_threads
			                                     : memory_threads;
		}
	}
	return repo->settings.compression_threads;
}

unsigned long repo_settings_get_compression_queue_memory(struct repository *repo)
{
	if (!repo->settings.compression_queue_memory) {
		unsigned long value;
		if (!repo_config_get_ulong(repo, "core.compressionqueuememory", &value)) {
			repo->settings.compression_queue_memory = value;
		} else {
			/* Auto-detect: use 1/8 of total system memory */
			unsigned long total_ram_mb = get_total_memory_mb();
			if (total_ram_mb > 0) {
				repo->settings.compression_queue_memory = total_ram_mb / 8;
			} else {
				/* Fallback if can't detect RAM */
				repo->settings.compression_queue_memory = 512;
			}
		}
	}
	return repo->settings.compression_queue_memory;
}

void repo_settings_set_compression_threads(struct repository *repo, unsigned int value)
{
	repo->settings.compression_threads = value;
}

void repo_settings_set_compression_queue_memory(struct repository *repo, unsigned long value)
{
	repo->settings.compression_queue_memory = value;
}

enum log_refs_config repo_settings_get_log_all_ref_updates(struct repository *repo)
{
	const char *value;

	if (!repo_config_get_string_tmp(repo, "core.logallrefupdates", &value)) {
		if (value && !strcasecmp(value, "always"))
			return LOG_REFS_ALWAYS;
		else if (git_config_bool("core.logallrefupdates", value))
			return LOG_REFS_NORMAL;
		else
			return LOG_REFS_NONE;
	}

	return LOG_REFS_UNSET;
}

int repo_settings_get_warn_ambiguous_refs(struct repository *repo)
{
	prepare_repo_settings(repo);
	if (repo->settings.warn_ambiguous_refs < 0)
		repo_cfg_bool(repo, "core.warnambiguousrefs",
			      &repo->settings.warn_ambiguous_refs, 1);
	return repo->settings.warn_ambiguous_refs;
}

const char *repo_settings_get_hooks_path(struct repository *repo)
{
	if (!repo->settings.hooks_path)
		repo_config_get_pathname(repo, "core.hookspath", &repo->settings.hooks_path);
	return repo->settings.hooks_path;
}

int repo_settings_get_shared_repository(struct repository *repo)
{
	if (!repo->settings.shared_repository_initialized) {
		const char *var = "core.sharedrepository";
		const char *value;
		if (!repo_config_get_value(repo, var, &value))
			repo->settings.shared_repository = git_config_perm(var, value);
		else
			repo->settings.shared_repository = PERM_UMASK;
		repo->settings.shared_repository_initialized = 1;
	}
	return repo->settings.shared_repository;
}

void repo_settings_set_shared_repository(struct repository *repo, int value)
{
	repo->settings.shared_repository = value;
	repo->settings.shared_repository_initialized = 1;
}

void repo_settings_reset_shared_repository(struct repository *repo)
{
	repo->settings.shared_repository_initialized = 0;
}
