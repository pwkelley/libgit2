/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "smart.h"
#include "refs.h"

#define NETWORK_XFER_THRESHOLD (100*1024)

int git_smart__store_refs(transport_smart *t, int flushes)
{
	gitno_buffer *buf = &t->buffer;
	git_vector *refs = &t->refs;
	int error, flush = 0, recvd;
	const char *line_end;
	git_pkt *pkt;

	do {
		if (buf->offset > 0)
			error = git_pkt_parse_line(&pkt, buf->data, &line_end, buf->offset);
		else
			error = GIT_EBUFS;

		if (error < 0 && error != GIT_EBUFS)
			return -1;

		if (error == GIT_EBUFS) {
			if ((recvd = gitno_recv(buf)) < 0)
				return -1;

			if (recvd == 0 && !flush) {
				giterr_set(GITERR_NET, "Early EOF");
				return -1;
			}

			continue;
		}

		gitno_consume(buf, line_end);
		if (pkt->type == GIT_PKT_ERR) {
			giterr_set(GITERR_NET, "Remote error: %s", ((git_pkt_err *)pkt)->error);
			git__free(pkt);
			return -1;
		}

		if (pkt->type != GIT_PKT_FLUSH && git_vector_insert(refs, pkt) < 0)
			return -1;

		if (pkt->type == GIT_PKT_FLUSH) {
			flush++;
			git_pkt_free(pkt);
		}
	} while (flush < flushes);

	return flush;
}

int git_smart__detect_caps(git_pkt_ref *pkt, transport_smart_caps *caps)
{
	const char *ptr;

	/* No refs or capabilites, odd but not a problem */
	if (pkt == NULL || pkt->capabilities == NULL)
		return 0;

	ptr = pkt->capabilities;
	while (ptr != NULL && *ptr != '\0') {
		if (*ptr == ' ')
			ptr++;

		if(!git__prefixcmp(ptr, GIT_CAP_OFS_DELTA)) {
			caps->common = caps->ofs_delta = 1;
			ptr += strlen(GIT_CAP_OFS_DELTA);
			continue;
		}

		if(!git__prefixcmp(ptr, GIT_CAP_MULTI_ACK)) {
			caps->common = caps->multi_ack = 1;
			ptr += strlen(GIT_CAP_MULTI_ACK);
			continue;
		}

		if(!git__prefixcmp(ptr, GIT_CAP_INCLUDE_TAG)) {
			caps->common = caps->include_tag = 1;
			ptr += strlen(GIT_CAP_INCLUDE_TAG);
			continue;
		}

		/* Keep side-band check after side-band-64k */
		if(!git__prefixcmp(ptr, GIT_CAP_SIDE_BAND_64K)) {
			caps->common = caps->side_band_64k = 1;
			ptr += strlen(GIT_CAP_SIDE_BAND_64K);
			continue;
		}

		if(!git__prefixcmp(ptr, GIT_CAP_SIDE_BAND)) {
			caps->common = caps->side_band = 1;
			ptr += strlen(GIT_CAP_SIDE_BAND);
			continue;
		}

		/* We don't know this capability, so skip it */
		ptr = strchr(ptr, ' ');
	}

	return 0;
}

static int recv_pkt(git_pkt **out, gitno_buffer *buf)
{
	const char *ptr = buf->data, *line_end = ptr;
	git_pkt *pkt;
	int pkt_type, error = 0, ret;

	do {
		if (buf->offset > 0)
			error = git_pkt_parse_line(&pkt, ptr, &line_end, buf->offset);
		else
			error = GIT_EBUFS;

		if (error == 0)
			break; /* return the pkt */

		if (error < 0 && error != GIT_EBUFS)
			return -1;

		if ((ret = gitno_recv(buf)) < 0)
			return -1;
	} while (error);

	gitno_consume(buf, line_end);
	pkt_type = pkt->type;
	if (out != NULL)
		*out = pkt;
	else
		git__free(pkt);

	return pkt_type;
}

static int store_common(transport_smart *t)
{
	git_pkt *pkt = NULL;
	gitno_buffer *buf = &t->buffer;

	do {
		if (recv_pkt(&pkt, buf) < 0)
			return -1;

		if (pkt->type == GIT_PKT_ACK) {
			if (git_vector_insert(&t->common, pkt) < 0)
				return -1;
		} else {
			git__free(pkt);
			return 0;
		}

	} while (1);

	return 0;
}

static int fetch_setup_walk(git_revwalk **out, git_repository *repo)
{
	git_revwalk *walk;
	git_strarray refs;
	unsigned int i;
	git_reference *ref;

	if (git_reference_list(&refs, repo, GIT_REF_LISTALL) < 0)
		return -1;

	if (git_revwalk_new(&walk, repo) < 0)
		return -1;

	git_revwalk_sorting(walk, GIT_SORT_TIME);

	for (i = 0; i < refs.count; ++i) {
		/* No tags */
		if (!git__prefixcmp(refs.strings[i], GIT_REFS_TAGS_DIR))
			continue;

		if (git_reference_lookup(&ref, repo, refs.strings[i]) < 0)
			goto on_error;

		if (git_reference_type(ref) == GIT_REF_SYMBOLIC)
			continue;
		if (git_revwalk_push(walk, git_reference_oid(ref)) < 0)
			goto on_error;

		git_reference_free(ref);
	}

	git_strarray_free(&refs);
	*out = walk;
	return 0;

on_error:
	git_reference_free(ref);
	git_strarray_free(&refs);
	return -1;
}

int git_smart__negotiate_fetch(git_transport *transport, git_repository *repo, git_remote_head **refs, size_t count)
{
	transport_smart *t = (transport_smart *)transport;
	gitno_buffer *buf = &t->buffer;
	git_buf data = GIT_BUF_INIT;
	git_revwalk *walk = NULL;
	int error = -1, pkt_type;
	unsigned int i;
	git_oid oid;

	/* No own logic, do our thing */
	if (git_pkt_buffer_wants(refs, count, &t->caps, &data) < 0)
		return -1;

	if (fetch_setup_walk(&walk, repo) < 0)
		goto on_error;
	/*
	 * We don't support any kind of ACK extensions, so the negotiation
	 * boils down to sending what we have and listening for an ACK
	 * every once in a while.
	 */
	i = 0;
	while ((error = git_revwalk_next(&oid, walk)) == 0) {
		git_pkt_buffer_have(&oid, &data);
		i++;
		if (i % 20 == 0) {
			if (t->cancelled.val) {
				giterr_set(GITERR_NET, "The fetch was cancelled by the user");
				error = GIT_EUSER;
				goto on_error;
			}

			git_pkt_buffer_flush(&data);
			if (git_buf_oom(&data))
				goto on_error;

			if (git_smart__negotiation_step(&t->parent, data.ptr, data.size) < 0)
				goto on_error;

			git_buf_clear(&data);
			if (t->caps.multi_ack) {
				if (store_common(t) < 0)
					goto on_error;
			} else {
				pkt_type = recv_pkt(NULL, buf);

				if (pkt_type == GIT_PKT_ACK) {
					break;
				} else if (pkt_type == GIT_PKT_NAK) {
					continue;
				} else {
					giterr_set(GITERR_NET, "Unexpected pkt type");
					goto on_error;
				}
			}
		}

		if (t->common.length > 0)
			break;

		if (i % 20 == 0 && t->rpc) {
			git_pkt_ack *pkt;
			unsigned int i;

			if (git_pkt_buffer_wants(refs, count, &t->caps, &data) < 0)
				goto on_error;

			git_vector_foreach(&t->common, i, pkt) {
				git_pkt_buffer_have(&pkt->oid, &data);
			}

			if (git_buf_oom(&data))
				goto on_error;
		}
	}

	if (error < 0 && error != GIT_ITEROVER)
		goto on_error;

	/* Tell the other end that we're done negotiating */
	if (t->rpc && t->common.length > 0) {
		git_pkt_ack *pkt;
		unsigned int i;

		if (git_pkt_buffer_wants(refs, count, &t->caps, &data) < 0)
			goto on_error;

		git_vector_foreach(&t->common, i, pkt) {
			git_pkt_buffer_have(&pkt->oid, &data);
		}

		if (git_buf_oom(&data))
			goto on_error;
	}

	git_pkt_buffer_done(&data);
	if (t->cancelled.val) {
		giterr_set(GITERR_NET, "The fetch was cancelled by the user");
		error = GIT_EUSER;
		goto on_error;
	}
	if (git_smart__negotiation_step(&t->parent, data.ptr, data.size) < 0)
		goto on_error;

	git_buf_free(&data);
	git_revwalk_free(walk);

	/* Now let's eat up whatever the server gives us */
	if (!t->caps.multi_ack) {
		pkt_type = recv_pkt(NULL, buf);
		if (pkt_type != GIT_PKT_ACK && pkt_type != GIT_PKT_NAK) {
			giterr_set(GITERR_NET, "Unexpected pkt type");
			return -1;
		}
	} else {
		git_pkt_ack *pkt;
		do {
			if (recv_pkt((git_pkt **)&pkt, buf) < 0)
				return -1;

			if (pkt->type == GIT_PKT_NAK ||
			    (pkt->type == GIT_PKT_ACK && pkt->status != GIT_ACK_CONTINUE)) {
				git__free(pkt);
				break;
			}

			git__free(pkt);
		} while (1);
	}

	return 0;

on_error:
	git_revwalk_free(walk);
	git_buf_free(&data);
	return error;
}

static int no_sideband(transport_smart *t, git_indexer_stream *idx, gitno_buffer *buf, git_transfer_progress *stats)
{
	int recvd;

	do {
		if (t->cancelled.val) {
			giterr_set(GITERR_NET, "The fetch was cancelled by the user");
			return GIT_EUSER;
		}

		if (git_indexer_stream_add(idx, buf->data, buf->offset, stats) < 0)
			return -1;

		gitno_consume_n(buf, buf->offset);

		if ((recvd = gitno_recv(buf)) < 0)
			return -1;
	} while(recvd > 0);

	if (git_indexer_stream_finalize(idx, stats))
		return -1;

	return 0;
}

struct network_packetsize_payload	
{
	git_transfer_progress_callback callback;
	void *payload;
	git_transfer_progress *stats;
	git_off_t last_fired_bytes;
};

static void network_packetsize(int received, void *payload)
{
	struct network_packetsize_payload *npp = (struct network_packetsize_payload*)payload;

	/* Accumulate bytes */
	npp->stats->received_bytes += received;

	/* Fire notification if the threshold is reached */
	if ((npp->stats->received_bytes - npp->last_fired_bytes) > NETWORK_XFER_THRESHOLD) {
		npp->last_fired_bytes = npp->stats->received_bytes;	
		npp->callback(npp->stats, npp->payload);	
	}
}

int git_smart__download_pack(
	git_transport *transport,
	git_repository *repo,
	git_transfer_progress *stats,
	git_transfer_progress_callback progress_cb,
	void *progress_payload)
{
	transport_smart *t = (transport_smart *)transport;
	git_buf path = GIT_BUF_INIT;
	gitno_buffer *buf = &t->buffer;
	git_indexer_stream *idx = NULL;
	int error = -1;
	struct network_packetsize_payload npp = {0};

	if (progress_cb) {
		npp.callback = progress_cb;
		npp.payload = progress_payload;
		npp.stats = stats;
		t->packetsize_cb = &network_packetsize;
		t->packetsize_payload = &npp;
	}

	if (git_buf_joinpath(&path, git_repository_path(repo), "objects/pack") < 0)
		return -1;

	if (git_indexer_stream_new(&idx, git_buf_cstr(&path), progress_cb, progress_payload) < 0)
		goto on_error;

	git_buf_free(&path);
	memset(stats, 0, sizeof(git_transfer_progress));

	/*
	 * If the remote doesn't support the side-band, we can feed
	 * the data directly to the indexer. Otherwise, we need to
	 * check which one belongs there.
	 */
	if (!t->caps.side_band && !t->caps.side_band_64k) {
		if (no_sideband(t, idx, buf, stats) < 0)
			goto on_error;

		git_indexer_stream_free(idx);
		return 0;
	}

	do {
		git_pkt *pkt;

		if (t->cancelled.val) {
			giterr_set(GITERR_NET, "The fetch was cancelled by the user");
			error = GIT_EUSER;
			goto on_error;
		}

		if (recv_pkt(&pkt, buf) < 0)
			goto on_error;

		if (pkt->type == GIT_PKT_PROGRESS) {
			if (t->progress_cb) {
				git_pkt_progress *p = (git_pkt_progress *) pkt;
				t->progress_cb(p->data, p->len, t->message_cb_payload);
			}
			git__free(pkt);
		} else if (pkt->type == GIT_PKT_DATA) {
			git_pkt_data *p = (git_pkt_data *) pkt;
			if (git_indexer_stream_add(idx, p->data, p->len, stats) < 0)
				goto on_error;

			git__free(pkt);
		} else if (pkt->type == GIT_PKT_FLUSH) {
			/* A flush indicates the end of the packfile */
			git__free(pkt);
			break;
		}
	} while (1);

	if (git_indexer_stream_finalize(idx, stats) < 0)
		goto on_error;

	git_indexer_stream_free(idx);
	return 0;

on_error:
	git_buf_free(&path);
	git_indexer_stream_free(idx);
	return error;
}
