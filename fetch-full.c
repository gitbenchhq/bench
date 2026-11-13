#include "git-compat-util.h"
#include "fetch-full.h"
#include "advice.h"
#include "gettext.h"
#include "list-objects.h"
#include "manifest.h"
#include "object.h"
#include "object-file.h"
#include "odb.h"
#include "oid-array.h"
#include "oidset.h"
#include "packfile.h"
#include "progress.h"
#include "promisor-remote.h"
#include "repository.h"
#include "revision.h"
#include "trace2.h"

/* Objects asked for in one download, as in "git backfill". */
#define FETCH_FULL_BATCH 50000

struct fetch_full {
	struct repository *repo;
	int can_download;		/* the repository has a promisor remote */
	struct oid_array batch;		/* missing objects, not yet asked for */
	struct oidset queued;		/* the same objects, so none is asked twice */
	uint64_t missing;		/* missing objects found this round */
	uint64_t downloaded;		/* how many of them arrived */
	struct progress *progress;
	uint64_t walked;
};

static int is_local(struct fetch_full *ff, const struct object_id *oid)
{
	return odb_has_object(ff->repo->objects, oid, 0);
}

static void download_batch(struct fetch_full *ff);

static void queue_missing(struct fetch_full *ff, const struct object_id *oid)
{
	if (!ff->can_download) {
		/* Count only: the walk shows each object once. */
		ff->missing++;
		return;
	}
	if (oidset_insert(&ff->queued, oid))
		return;
	ff->missing++;
	oid_array_append(&ff->batch, oid);
	if (ff->batch.nr >= FETCH_FULL_BATCH)
		download_batch(ff);
}

/*
 * A manifest that was just downloaded names chunks the walk has not
 * seen, because it could not read the manifest when it passed it.
 * Queue them now instead of waiting for the next walk.
 */
static void queue_missing_chunks(struct fetch_full *ff,
				 const struct object_id *manifest_oid)
{
	struct oid_array chunks = OID_ARRAY_INIT;

	/* An unreadable manifest is left for fsck to report. */
	if (get_manifest_chunk_oids(ff->repo, manifest_oid, NULL, &chunks) < 0)
		return;
	for (size_t i = 0; i < chunks.nr; i++)
		if (!is_local(ff, &chunks.oid[i]))
			queue_missing(ff, &chunks.oid[i]);
	oid_array_clear(&chunks);
}

/*
 * Ask the promisor remotes for oids[0..nr). A remote refuses a whole
 * request if it lacks one object in it, so when nothing arrives, ask for
 * each half on its own: everything the remote still has is downloaded,
 * for about 2 log2(nr) refused requests per object it lacks.
 */
static void download(struct fetch_full *ff, const struct object_id *oids,
		     size_t nr)
{
	size_t arrived = 0;

	if (!nr)
		return;
	trace2_data_intmax("fetch-full", ff->repo, "request", nr);
	promisor_remote_get_direct_gently(ff->repo, oids, nr);
	/* The download added a pack; look for objects in it from now on. */
	reprepare_packed_git(ff->repo);

	for (size_t i = 0; i < nr; i++)
		if (is_local(ff, &oids[i]))
			arrived++;
	if (!arrived && nr > 1) {
		download(ff, oids, nr / 2);
		download(ff, oids + nr / 2, nr - nr / 2);
	}
}

static void download_batch(struct fetch_full *ff)
{
	struct oid_array manifests = OID_ARRAY_INIT;

	if (!ff->batch.nr)
		return;

	download(ff, ff->batch.oid, ff->batch.nr);

	for (size_t i = 0; i < ff->batch.nr; i++) {
		const struct object_id *oid = &ff->batch.oid[i];

		if (!is_local(ff, oid))
			continue;
		ff->downloaded++;
		if (odb_read_object_info(ff->repo->objects, oid, NULL) == OBJ_MANIFEST)
			oid_array_append(&manifests, oid);
	}
	oid_array_clear(&ff->batch);
	oidset_clear(&ff->queued);
	oidset_init(&ff->queued, 0);

	for (size_t i = 0; i < manifests.nr; i++)
		queue_missing_chunks(ff, &manifests.oid[i]);
	oid_array_clear(&manifests);
}

static void show_commit(struct commit *commit UNUSED, void *data)
{
	struct fetch_full *ff = data;

	display_progress(ff->progress, ++ff->walked);
}

static void show_object(struct object *obj, const char *name UNUSED, void *data)
{
	struct fetch_full *ff = data;

	display_progress(ff->progress, ++ff->walked);
	if (!is_local(ff, &obj->oid))
		queue_missing(ff, &obj->oid);
}

/*
 * Walk every object reachable from a ref, as "rev-list --objects --all"
 * does, and download the ones that are missing. Lazy fetching is off, so
 * the walk shows a missing tree or manifest without going into it; what
 * it holds is found once it has arrived.
 */
static void walk_and_download(struct fetch_full *ff, int quiet)
{
	struct rev_info revs;
	const char *args[] = { "fetch-full", "--all", NULL };
	struct oidset_iter iter;
	const struct object_id *oid;

	/* Earlier walks in this process (a fetch's own) leave flags behind. */
	clear_object_flags(ff->repo, ~0u);

	repo_init_revisions(ff->repo, &revs, NULL);
	revs.tree_objects = 1;
	revs.blob_objects = 1;
	revs.tag_objects = 1;
	revs.do_not_die_on_missing_objects = 1;
	if (setup_revisions(ARRAY_SIZE(args) - 1, args, &revs, NULL) > 1)
		BUG("fetch-full: unexpected revision arguments");
	if (prepare_revision_walk(&revs))
		die(_("revision walk setup failed"));

	if (!quiet)
		ff->progress = start_delayed_progress(ff->repo,
						      _("Looking for missing objects"), 0);
	traverse_commit_list(&revs, show_commit, show_object, ff);
	stop_progress(&ff->progress);

	/* Commits, and objects that tags point to, that are missing. */
	oidset_iter_init(&revs.missing_commits, &iter);
	while ((oid = oidset_iter_next(&iter)))
		queue_missing(ff, oid);

	release_revisions(&revs);
	download_batch(ff);
	clear_object_flags(ff->repo, ~0u);
}

int fetch_full(struct repository *r, int quiet)
{
	struct fetch_full ff = {
		.repo = r,
		.batch = OID_ARRAY_INIT,
	};
	int saved_fetch_if_missing = fetch_if_missing;
	uint64_t total = 0;
	int ret = 0;

	oidset_init(&ff.queued, 0);
	ff.can_download = repo_has_promisor_remote(r);
	fetch_if_missing = 0;

	trace2_region_enter("fetch-full", "fetch-full", r);
	/*
	 * Each walk downloads what it finds missing. Stop once a walk finds
	 * nothing missing, or when nothing it asked for arrived. Every other
	 * walk downloads at least one object, so the loop ends.
	 */
	for (;;) {
		ff.missing = ff.downloaded = ff.walked = 0;
		walk_and_download(&ff, quiet);
		total += ff.downloaded;
		if (!ff.missing)
			break;
		if (!ff.downloaded) {
			uint64_t n = ff.missing;

			if (ff.can_download) {
				error(Q_("%"PRIuMAX" object reachable from the refs could not be downloaded",
					 "%"PRIuMAX" objects reachable from the refs could not be downloaded",
					 n), (uintmax_t)n);
				advise(_("Run the command again to retry. The objects that did\n"
					 "arrive are kept, so it continues where it stopped."));
			} else {
				error(Q_("%"PRIuMAX" object reachable from the refs is missing and no\n"
					 "promisor remote is configured to download it from",
					 "%"PRIuMAX" objects reachable from the refs are missing and no\n"
					 "promisor remote is configured to download them from",
					 n), (uintmax_t)n);
			}
			ret = -1;
			break;
		}
	}
	trace2_data_intmax("fetch-full", r, "downloaded", total);
	trace2_region_leave("fetch-full", "fetch-full", r);

	fetch_if_missing = saved_fetch_if_missing;
	oid_array_clear(&ff.batch);
	oidset_clear(&ff.queued);

	/*
	 * No count: a remote may send more than was asked for (the chunks
	 * of a manifest, the contents of a tree), so it would mislead.
	 */
	if (!ret && !quiet) {
		if (total)
			fprintf_ln(stderr, _("Every object reachable from the refs is now local."));
		else
			fprintf_ln(stderr, _("Every object reachable from the refs is already local."));
	}
	return ret;
}
