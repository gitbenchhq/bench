#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "parallel-compress.h"
#include "object-file.h"
#include "hex.h"
#include "config.h"
#include "repository.h"
#include "repo-settings.h"
#include "thread-utils.h"
#include "environment.h"

/* Maximum size of object header: "blob 18446744073709551615\0" */
#define MAX_HEADER_LEN 32

/*
 * Get total system memory in MB.
 * Returns 0 if unable to determine.
 */
unsigned long get_total_memory_mb(void)
{
#ifdef _SC_PHYS_PAGES
	long pages = sysconf(_SC_PHYS_PAGES);
	long page_size = sysconf(_SC_PAGESIZE);
	if (pages > 0 && page_size > 0) {
		unsigned long long total_bytes = (unsigned long long)pages * page_size;
		return (unsigned long)(total_bytes / (1024 * 1024));
	}
#endif
	return 0;  /* Unable to determine */
}

/*
 * Parallel Compression Implementation
 *
 * Architecture:
 *   1. Producer (chunker/pack-objects) submits jobs to input queue
 *   2. Worker threads pull jobs, compress, push to output queue
 *   3. Consumer (chunker/pack-objects) pulls results from output queue
 *
 * Key features:
 *   - Bounded queues prevent memory explosion
 *   - Backpressure: producer blocks when input queue full
 *   - In-order results: output ordered by job_id
 *   - Error handling: workers mark jobs, consumer checks before use
 */

/* Helper: Calculate safe queue size based on memory and chunk size */
static size_t calculate_queue_size(unsigned int num_threads,
                                  unsigned long max_chunk_size,
                                  unsigned long queue_memory_mb)
{
	size_t desired_slots = num_threads * 2;  /* 2× threads for good parallelism */
	unsigned long queue_memory_bytes = queue_memory_mb * 1024 * 1024;

	/* How many chunks of max size fit in target memory? */
	size_t memory_limited_slots = queue_memory_bytes / max_chunk_size;

	/* Use smaller of desired vs memory-limited, but minimum 4 */
	size_t queue_size = desired_slots < memory_limited_slots
	                    ? desired_slots
	                    : memory_limited_slots;

	return queue_size < 4 ? 4 : queue_size;
}

/* Helper: Check if input queue is full */
static inline int input_queue_full(struct parallel_compressor *pc)
{
	return pc->input_count >= pc->input_queue_size;
}

/* Helper: Check if input queue is empty */
static inline int input_queue_empty(struct parallel_compressor *pc)
{
	return pc->input_count == 0;
}

/* Helper: Check if output queue is empty */
static inline int output_queue_empty(struct parallel_compressor *pc)
{
	return pc->output_count == 0;
}

/* Helper: Enqueue job to input queue (caller holds lock) */
static void enqueue_input(struct parallel_compressor *pc, struct compress_job *job)
{
	pc->input_queue[pc->input_tail] = job;
	pc->input_tail = (pc->input_tail + 1) % pc->input_queue_size;
	pc->input_count++;
}

/* Helper: Dequeue job from input queue (caller holds lock) */
static struct compress_job *dequeue_input(struct parallel_compressor *pc)
{
	struct compress_job *job = pc->input_queue[pc->input_head];
	pc->input_head = (pc->input_head + 1) % pc->input_queue_size;
	pc->input_count--;
	return job;
}

/* Helper: Enqueue result to output queue (caller holds lock) */
static void enqueue_output(struct parallel_compressor *pc, struct compress_job *job)
{
	size_t old_size, new_size;

	/* Grow output queue if needed (unlike input queue, output is unbounded) */
	if (pc->output_count >= pc->output_queue_size) {
		old_size = pc->output_queue_size;
		new_size = old_size * 2;
		REALLOC_ARRAY(pc->output_queue, new_size);
		pc->output_queue_size = new_size;

		/* If queue wrapped around, unwrap it after resize */
		if (pc->output_tail <= pc->output_head && pc->output_count > 0) {
			/* Copy wrapped portion to end of new space */
			memcpy(pc->output_queue + old_size,
			       pc->output_queue,
			       pc->output_tail * sizeof(struct compress_job *));
			pc->output_tail += old_size;
		}
	}

	pc->output_queue[pc->output_tail] = job;
	pc->output_tail = (pc->output_tail + 1) % pc->output_queue_size;
	pc->output_count++;
}

/* Helper: Dequeue result from output queue (caller holds lock) */
static struct compress_job *dequeue_output(struct parallel_compressor *pc)
{
	struct compress_job *job = pc->output_queue[pc->output_head];
	pc->output_head = (pc->output_head + 1) % pc->output_queue_size;
	pc->output_count--;
	return job;
}

/* Worker thread: compress jobs from input queue */
static void *compress_worker(void *arg)
{
	struct parallel_compressor *pc = arg;

	while (1) {
		struct compress_job *job;

		/* Get job from input queue */
		pthread_mutex_lock(&pc->input_lock);

		while (input_queue_empty(pc) && !pc->shutdown)
			pthread_cond_wait(&pc->input_ready, &pc->input_lock);

		if (pc->shutdown && input_queue_empty(pc)) {
			pthread_mutex_unlock(&pc->input_lock);
			break;  /* Exit thread */
		}

		job = dequeue_input(pc);

		/* Signal that space is available in input queue */
		pthread_cond_signal(&pc->input_space);

		pthread_mutex_unlock(&pc->input_lock);

		/* Initialize job outputs */
		job->error_code = COMPRESS_OK;
		job->compressed = NULL;
		job->compressed_size = 0;
		job->error_message[0] = '\0';

		/* Step 1: Compute SHA256 hash (includes header in hash) */
		hash_object_file(the_hash_algo, job->data, job->size,
		                job->type, &job->oid);

		/* Step 2: Prepare header for compression */
		char hdr[MAX_HEADER_LEN];
		int hdrlen = xsnprintf(hdr, sizeof(hdr), "%s %"PRIuMAX,
		                      type_name(job->type), (uintmax_t)job->size) + 1;

		/* Step 3: Compress header + data with zlib */
		git_zstream stream;
		unsigned long max_compressed;
		int ret;

		memset(&stream, 0, sizeof(stream));
		git_deflate_init(&stream, zlib_compression_level);

		max_compressed = git_deflate_bound(&stream, hdrlen + job->size);
		job->compressed = xmalloc(max_compressed);
		if (!job->compressed) {
			job->error_code = COMPRESS_ERR_NOMEM;
			snprintf(job->error_message, sizeof(job->error_message),
			        "out of memory during compression");
			git_deflate_end(&stream);
			goto enqueue_result;
		}

		stream.next_out = job->compressed;
		stream.avail_out = max_compressed;

		/* First compress the header */
		stream.next_in = (unsigned char *)hdr;
		stream.avail_in = hdrlen;
		while (stream.avail_in > 0) {
			ret = git_deflate(&stream, 0);
			if (ret != Z_OK) {
				job->error_code = COMPRESS_ERR_DEFLATE;
				snprintf(job->error_message, sizeof(job->error_message),
				        "zlib header compression failed: %d", ret);
				FREE_AND_NULL(job->compressed);
				git_deflate_end(&stream);
				goto enqueue_result;
			}
		}

		/* Then compress the data */
		stream.next_in = (unsigned char *)job->data;
		stream.avail_in = job->size;
		while ((ret = git_deflate(&stream, Z_FINISH)) == Z_OK)
			; /* Keep deflating */

		if (ret != Z_STREAM_END) {
			job->error_code = COMPRESS_ERR_DEFLATE;
			snprintf(job->error_message, sizeof(job->error_message),
			        "zlib compression failed: %d", ret);
			FREE_AND_NULL(job->compressed);
			git_deflate_end(&stream);
			goto enqueue_result;
		}

		job->compressed_size = stream.total_out;
		git_deflate_end(&stream);

enqueue_result:
		/* Free input data (worker takes ownership) */
		free(job->data);
		job->data = NULL;

		/* Put result in output queue and mark as completed */
		pthread_mutex_lock(&pc->output_lock);
		enqueue_output(pc, job);
		pc->jobs_completed++;
		pthread_cond_signal(&pc->output_ready);
		pthread_mutex_unlock(&pc->output_lock);
	}

	return NULL;
}

/* Initialize parallel compressor */
int parallel_compressor_init(struct parallel_compressor *pc,
                             struct repository *repo)
{
	unsigned int i;
	unsigned long max_chunk_size;
	unsigned long queue_memory;

	memset(pc, 0, sizeof(*pc));
	pc->repo = repo;

	/* Get configuration */
	pc->num_threads = repo_settings_get_compression_threads(repo);
	queue_memory = repo_settings_get_compression_queue_memory(repo);
	max_chunk_size = repo_settings_get_chunk_max_size(repo);

	/* Calculate safe queue size for input (bounded) */
	pc->input_queue_size = calculate_queue_size(pc->num_threads, max_chunk_size, queue_memory);
	pc->output_queue_size = pc->input_queue_size;  /* Start with same size, will grow if needed */

	/* Allocate queues */
	pc->input_queue = xcalloc(pc->input_queue_size, sizeof(struct compress_job *));
	pc->output_queue = xcalloc(pc->output_queue_size, sizeof(struct compress_job *));

	/* Initialize synchronization primitives */
	pthread_mutex_init(&pc->input_lock, NULL);
	pthread_mutex_init(&pc->output_lock, NULL);
	pthread_cond_init(&pc->input_ready, NULL);
	pthread_cond_init(&pc->input_space, NULL);
	pthread_cond_init(&pc->output_ready, NULL);

	/* Create worker threads */
	pc->threads = xcalloc(pc->num_threads, sizeof(pthread_t));
	for (i = 0; i < pc->num_threads; i++) {
		if (pthread_create(&pc->threads[i], NULL, compress_worker, pc) != 0) {
			error("failed to create compression worker thread %u", i);
			pc->num_threads = i;  /* Only clean up created threads */
			parallel_compressor_cleanup(pc);
			return -1;
		}
	}

	return 0;
}

/* Submit job for compression */
int parallel_compressor_submit(struct parallel_compressor *pc,
                               void *data,
                               unsigned long size,
                               enum object_type type,
                               int job_id)
{
	struct compress_job *job;

	/* Allocate job */
	job = xcalloc(1, sizeof(*job));
	job->data = data;
	job->size = size;
	job->type = type;
	job->job_id = job_id;

	/* Add to input queue (may block if full) */
	pthread_mutex_lock(&pc->input_lock);

	/* Wait if queue is full (backpressure) */
	while (input_queue_full(pc))
		pthread_cond_wait(&pc->input_space, &pc->input_lock);

	enqueue_input(pc, job);
	pc->jobs_submitted++;

	/* Wake up a worker thread */
	pthread_cond_signal(&pc->input_ready);

	pthread_mutex_unlock(&pc->input_lock);

	return 0;
}

/* Get next completed job */
struct compress_job *parallel_compressor_get_result(struct parallel_compressor *pc)
{
	struct compress_job *job;

	pthread_mutex_lock(&pc->output_lock);

	/* Wait for results */
	while (output_queue_empty(pc) && pc->jobs_completed < pc->jobs_submitted)
		pthread_cond_wait(&pc->output_ready, &pc->output_lock);

	if (output_queue_empty(pc)) {
		pthread_mutex_unlock(&pc->output_lock);
		return NULL;  /* No more results */
	}

	job = dequeue_output(pc);

	pthread_mutex_unlock(&pc->output_lock);

	return job;
}

/* Wait for all pending jobs to complete */
void parallel_compressor_finish(struct parallel_compressor *pc)
{
	/* Just wait until all submitted jobs are completed */
	pthread_mutex_lock(&pc->output_lock);
	while (pc->jobs_completed < pc->jobs_submitted)
		pthread_cond_wait(&pc->output_ready, &pc->output_lock);
	pthread_mutex_unlock(&pc->output_lock);
}

/* Clean up and free all resources */
void parallel_compressor_cleanup(struct parallel_compressor *pc)
{
	unsigned int i;

	if (!pc->threads)
		return;  /* Not initialized */

	/* Signal shutdown */
	pthread_mutex_lock(&pc->input_lock);
	pc->shutdown = 1;
	pthread_cond_broadcast(&pc->input_ready);
	pthread_mutex_unlock(&pc->input_lock);

	/* Wait for all threads to exit */
	for (i = 0; i < pc->num_threads; i++)
		pthread_join(pc->threads[i], NULL);

	/* Free any remaining jobs in queues */
	while (!input_queue_empty(pc)) {
		struct compress_job *job = dequeue_input(pc);
		/* Input jobs still have their data (not yet processed) */
		if (job->data)
			free(job->data);
		if (job->compressed)
			free(job->compressed);
		free(job);
	}

	while (!output_queue_empty(pc)) {
		struct compress_job *job = dequeue_output(pc);
		/* Output jobs have already had their data freed by workers */
		if (job->compressed)
			free(job->compressed);
		free(job);
	}

	/* Free resources */
	free(pc->threads);
	free(pc->input_queue);
	free(pc->output_queue);

	pthread_mutex_destroy(&pc->input_lock);
	pthread_mutex_destroy(&pc->output_lock);
	pthread_cond_destroy(&pc->input_ready);
	pthread_cond_destroy(&pc->input_space);
	pthread_cond_destroy(&pc->output_ready);

	memset(pc, 0, sizeof(*pc));
}
