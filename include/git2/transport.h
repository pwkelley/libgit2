/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_transport_h__
#define INCLUDE_git_transport_h__

#include "indexer.h"
#include "net.h"

/**
 * @file git2/transport.h
 * @brief Git transport interfaces and functions
 * @defgroup git_transport interfaces and functions
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

/*
 *** Begin base transport interface ***
 */

typedef enum {
	GIT_TRANSPORTFLAGS_NONE = 0,
	/* If the connection is secured with SSL/TLS, the authenticity
	 * of the server certificate should not be verified. */
	GIT_TRANSPORTFLAGS_NO_CHECK_CERT = 1
} git_transport_flags_t;

typedef void (*git_transport_message_cb)(const char *str, int len, void *data);

typedef struct git_transport {
	/* Set progress and error callbacks */
	int (*set_callbacks)(struct git_transport *transport,
		git_transport_message_cb progress_cb,
		git_transport_message_cb error_cb,
		void *payload);

	/* Connect the transport to the remote repository, using the given
	 * direction. */
	int (*connect)(struct git_transport *transport,
		const char *url,
		int direction,
		int flags);

	/* This function may be called after a successful call to connect(). The
	 * provided callback is invoked for each ref discovered on the remote
	 * end. */
	int (*ls)(struct git_transport *transport,
		git_headlist_cb list_cb,
		void *payload);

	/* Reserved until push is implemented. */
	int (*push)(struct git_transport *transport);

	/* This function may be called after a successful call to connect(), when
	 * the direction is FETCH. The function performs a negotiation to calculate
	 * the wants list for the fetch. */
	int (*negotiate_fetch)(struct git_transport *transport,
		git_repository *repo,
		const git_remote_head * const *refs,
		size_t count);

	/* This function may be called after a successful call to negotiate_fetch(),
	 * when the direction is FETCH. This function retrieves the pack file for
	 * the fetch from the remote end. */
	int (*download_pack)(struct git_transport *transport,
		git_repository *repo,
		git_transfer_progress *stats,
		git_transfer_progress_callback progress_cb,
		void *progress_payload);

	/* Checks to see if the transport is connected */
	int (*is_connected)(struct git_transport *transport, int *connected);

	/* Reads the flags value previously passed into connect() */
	int (*read_flags)(struct git_transport *transport, int *flags);

	/* Cancels any outstanding transport operation */
	void (*cancel)(struct git_transport *transport);

	/* This function is the reverse of connect() -- it terminates the
	 * connection to the remote end. */
	int (*close)(struct git_transport *transport);

	/* Frees/destructs the git_transport object. */
	void (*free)(struct git_transport *transport);
} git_transport;

/**
 * Function to use to create a transport from a URL. The transport database
 * is scanned to find a transport that implements the scheme of the URI (i.e.
 * git:// or http://) and a transport object is returned to the caller.
 *
 * @param transport The newly created transport (out)
 * @param url The URL to connect to
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_transport_new(git_transport **transport, const char *url);

/**
 * Function which checks to see if a transport could be created for the
 * given URL (i.e. checks to see if libgit2 has a transport that supports
 * the given URL's scheme)
 *
 * @param url The URL to check
 * @return Zero if the URL is not valid; nonzero otherwise
 */
GIT_EXTERN(int) git_transport_valid_url(const char *url);

/* Signature of a function which creates a transport */
typedef int (*git_transport_cb)(git_transport **transport, void *param);

/* Transports which come with libgit2 (match git_transport_cb). The expected
 * value for "param" is listed in-line below. */

/**
 * Create an instance of the dummy transport.
 *
 * @param transport The newly created transport (out)
 * @param param You must pass NULL for this parameter.
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_transport_dummy(
	git_transport **transport,
	/* NULL */ void *param);

/**
 * Create an instance of the local transport.
 *
 * @param transport The newly created transport (out)
 * @param param You must pass NULL for this parameter.
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_transport_local(
	git_transport **transport,
	/* NULL */ void *param);

/**
 * Create an instance of the smart transport.
 *
 * @param transport The newly created transport (out)
 * @param param A pointer to a git_smart_subtransport_definition
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_transport_smart(
	git_transport **transport,
	/* (git_smart_subtransport_definition *) */ void *param);

/*
 *** End of base transport interface ***
 *** Begin interface for subtransports for the smart transport ***
 */

/* The smart transport knows how to speak the git protocol, but it has no
 * knowledge of how to establish a connection between it and another endpoint,
 * or how to move data back and forth. For this, a subtransport interface is
 * declared, and the smart transport delegates this work to the subtransports.
 * Three subtransports are implemented: git, http, and winhttp. (The http and
 * winhttp transports each implement both http and https.) */

/* Subtransports can either be RPC = 0 (persistent connection) or RPC = 1
 * (request/response). The smart transport handles the differences in its own
 * logic. The git subtransport is RPC = 0, while http and winhttp are both
 * RPC = 1. */

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
	/* The owning subtransport */
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
typedef int (*git_smart_subtransport_cb)(
	git_smart_subtransport **out,
	git_transport* owner);

typedef struct git_smart_subtransport_definition {
	/* The function to use to create the git_smart_subtransport */
	git_smart_subtransport_cb callback;
	/* True if the protocol is stateless; false otherwise. For example,
	 * http:// is stateless, but git:// is not. */
	unsigned rpc : 1;
} git_smart_subtransport_definition;

/* Smart transport subtransports that come with libgit2 */

/**
 * Create an instance of the http subtransport. This subtransport
 * also supports https. On Win32, this subtransport may be implemented
 * using the WinHTTP library.
 *
 * @param out The newly created subtransport
 * @param owner The smart transport to own this subtransport
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_smart_subtransport_http(
	git_smart_subtransport **out,
	git_transport* owner);

/**
 * Create an instance of the git subtransport.
 *
 * @param out The newly created subtransport
 * @param owner The smart transport to own this subtransport
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_smart_subtransport_git(
	git_smart_subtransport **out,
	git_transport* owner);

/*
 *** End interface for subtransports for the smart transport ***
 */

/** @} */
GIT_END_DECL
#endif
