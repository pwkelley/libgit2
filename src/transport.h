/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_transport_h__
#define INCLUDE_transport_h__

#include "git2/net.h"
#include "git2/indexer.h"
#include "vector.h"
#include "posix.h"
#include "common.h"
#include "netops.h"

/*
 *** Begin base transport interface ***
 */

typedef enum {
	GIT_TRANSPORTFLAGS_NONE = 0,
	/* If the connection is secured with SSL/TLS, the authenticity
	 * of the server certificate should not be verified. */
	GIT_TRANSPORTFLAGS_NO_CHECK_CERT = 1
} git_transport_flags_t;

typedef struct git_transport {
	/* The URI of the repository */
	char *url;

	/* The direction in which the transport is being used: GIT_DIR_FETCH or GIT_DIR_PUSH */
	int direction;

	/* Various flags which control transport behavior */
	git_transport_flags_t flags;

	/* True if the transport is connected; false otherwise */
	int connected : 1;

	/* An opaque value which is passed to callback functions */
	void *cb_data;

	/* Indicates whether the transport action has been cancelled */
	git_atomic cancel;

	/* Connect the transport to the remote repository, using the given direction. */
	int (*connect)(
		struct git_transport *transport,
		int direction);

	/* This function may be called after a successful call to connect(). The provided callback is
	 * invoked for each ref discovered on the remote end. */
	int (*ls)(
		struct git_transport *transport,
		git_headlist_cb list_cb,
		void *payload);

	/* Reserved until push is implemented. */
	int (*push)(struct git_transport *transport);

	/* This function may be called after a successful call to connect(), when the direction is FETCH. The function
	 * performs a negotiation to calculate the wants list for the fetch. */
	int (*negotiate_fetch)(struct git_transport *transport,
		git_repository *repo,
		const git_vector *wants);

	/* This function may be called after a successful call to negotiate_fetch(), when the direction is FETCH. This function
	 * retrieves the pack file for the fetch from the remote end. */
	int (*download_pack)(struct git_transport *transport,
		git_repository *repo,
		git_transfer_progress *stats,
		git_transfer_progress_callback progress_cb,
		void *progress_payload);

	/* This function is the reverse of connect() -- it terminates the connection to the remote end. */
	int (*close)(struct git_transport *transport);

	/* Frees/destructs the git_transport object. */
	void (*free)(struct git_transport *transport);

	/* Optional callback function to be invoked during the download_pack operation. */
	void (*progress_cb)(const char *str, int len, void *data);

	/* Optional callback function to be invoked in the event of an error. */
	void (*error_cb)(const char *str, int len, void *data);
} git_transport;

/* Function to use to create a transport from a URL. The transport database is scanned
 * to find a transport that implements the scheme of the URI (i.e. git:// or http://)
 * and a transport object is returned to the caller. */
int git_transport_new(git_transport **transport, const char *url);

/* Function which checks to see if a transport could be created for the given URL (i.e.
 * checks to see if libgit2 has a transport that supports the given URL's scheme) */
int git_transport_valid_url(const char *url);

/* Signature of a function which creates a transport */
typedef int (*git_transport_cb)(git_transport **transport, void *param);

/* Transports which come with libgit2 (match git_transport_cb) */
int git_transport_dummy(git_transport **transport, void *param);
int git_transport_local(git_transport **transport, void *param);
int git_transport_smart(git_transport **transport, void *param);

/*
 *** End of base transport interface ***
 *** Begin interface for subtransports for the smart transport ***
 */

/* Actions that the smart transport can ask
 * a subtransport to perform */
typedef enum {
	GIT_SERVICE_UPLOADPACK_LS = 1,
	GIT_SERVICE_UPLOADPACK = 2,
} git_smart_service_t;

struct git_smart_subtransport;

/* A stream used by the smart transport to read and write data
 * from a subtransport */
typedef struct git_smart_subtransport_stream {
	struct git_smart_subtransport *subtransport;

	int (*read)(
			struct git_smart_subtransport_stream *stream,
			char *buffer,
			size_t buf_size,
			size_t *bytes_read);

	int (*write)(
			struct git_smart_subtransport_stream *stream,
			const char *buffer,
			size_t len);

	void (*free)(
			struct git_smart_subtransport_stream *stream);
} git_smart_subtransport_stream;

/* An implementation of a subtransport which carries data for the
 * smart transport */
typedef struct git_smart_subtransport {
	int (* action)(
			git_smart_subtransport_stream **out,
			struct git_smart_subtransport *transport,
			const char *url,
			git_smart_service_t action);

	void (* free)(struct git_smart_subtransport *transport);
} git_smart_subtransport;

/* A function which creates a new subtransport for the smart transport */
typedef int (*git_smart_subtransport_cb)(git_smart_subtransport **out, git_transport* parent);

typedef struct git_smart_subtransport_definition {
	/* The function to use to create the git_smart_subtransport */
	git_smart_subtransport_cb callback;
	/* True if the protocol is stateless; false otherwise. For example,
	 * http:// is stateless, but git:// is not. */
	unsigned rpc : 1;
} git_smart_subtransport_definition;

/* Smart transport subtransports that come with libgit2 */
int git_smart_subtransport_winhttp(git_smart_subtransport **out, git_transport* parent);
int git_smart_subtransport_http(git_smart_subtransport **out, git_transport* parent);
int git_smart_subtransport_git(git_smart_subtransport **out, git_transport* parent);

/*
 *** End interface for subtransports for the smart transport ***
 */

#endif
