#ifndef FETCH_FULL_H
#define FETCH_FULL_H

struct repository;

/*
 * Make the repository complete: download every object reachable from any
 * ref (all branches, tags and remote-tracking branches, in all history)
 * that a partial clone left out, from the promisor remotes. Large files
 * come in two steps: first their manifests, then the chunks those name.
 *
 * Nothing is downloaded when nothing is missing, so running it again is
 * cheap. The promisor remotes stay configured.
 *
 * Returns 0 when every reachable object is local; otherwise prints an
 * error with the number still missing and returns -1.
 */
int fetch_full(struct repository *r, int quiet);

#endif /* FETCH_FULL_H */
