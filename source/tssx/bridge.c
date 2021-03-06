#define _XOPEN_SOURCE 500

#include <assert.h>
#include <signal.h>
#include <stdlib.h>

#include "tssx/bridge.h"
#include "tssx/connection.h"
#include "tssx/session.h"
#include "utility/utility.h"


/******************** GLOBAL DATA ********************/

Bridge bridge = BRIDGE_INITIALIZER;

signal_handler_t old_sigint_handler = NULL;
signal_handler_t old_sigterm_handler = NULL;
signal_handler_t old_sigabrt_handler = NULL;

/******************** INTERFACE ********************/

int bridge_setup(Bridge* bridge) {
	assert(!bridge_is_initialized(bridge));

	session_table_setup(&bridge->session_table);
	_setup_exit_handling();

	bridge->is_initialized = true;
	bridge->connection_count = 0;

	return SUCCESS;
}

int bridge_destroy(Bridge* bridge) {
	assert(bridge_is_initialized(bridge));
	session_table_destroy(&bridge->session_table);
	return SUCCESS;
}

bool bridge_is_initialized(const Bridge* bridge) {
	assert(bridge != NULL);
	return bridge->is_initialized;
}

int bridge_add_user(Bridge* bridge) {
	if (_lazy_bridge_setup(bridge) == ERROR) return ERROR;
	if (bridge->connection_count == 0) return SUCCESS;

	for (size_t index = 0; index < SESSION_TABLE_SIZE; ++index) {
		Session* session = session_table_get(&bridge->session_table, index);
		if (session_has_connection(session)) {
			connection_add_user(session->connection);
		}
	}

	return SUCCESS;
}

int bridge_insert(Bridge* bridge, int fd, Session* session) {
	if (_lazy_bridge_setup(bridge) == ERROR) return ERROR;

	session_table_assign(&bridge->session_table, fd, session);

	if (session_has_connection(session)) {
		bridge->connection_count++;
	}

	return SUCCESS;
}

int bridge_erase(Bridge* bridge, int fd) {
	Session* session;

	if (_lazy_bridge_setup(bridge) == ERROR) return ERROR;

	session = session_table_get(&bridge->session_table, fd);

	if (session_has_connection(session)) {
		bridge->connection_count--;
	}

	session_invalidate(session);

	return SUCCESS;
}

Session* bridge_lookup(Bridge* bridge, int fd) {
	if (_lazy_bridge_setup(bridge) == ERROR) return NULL;
	return session_table_get(&bridge->session_table, fd);
}

bool bridge_has_connection(Bridge* bridge, int fd) {
	const Session* session;

	if (_lazy_bridge_setup(bridge) == ERROR) return ERROR;

	session = session_table_get(&bridge->session_table, fd);

	return session_has_connection(session);
}

bool bridge_has_any_connections(const Bridge* bridge) {
	return bridge->connection_count > 0;
}

/******************** PRIVATE ********************/

int _lazy_bridge_setup(Bridge* bridge) {
	if (!bridge_is_initialized(bridge)) {
		if (bridge_setup(bridge) == ERROR) {
			return ERROR;
		}
	}

	return SUCCESS;
}

void _setup_exit_handling() {
	_setup_signal_handler(SIGINT);
	_setup_signal_handler(SIGTERM);
	_setup_signal_handler(SIGABRT);

	// With atexit we can register up to 32 functions that are
	// called at *normal* program termination. *Normal* means
	// either return from main or a call to exit().
	atexit(_bridge_exit_handler);
}

void _setup_signal_handler(int signal_number) {
	struct sigaction signal_action, old_action;

	// clang-format off
	assert(signal_number == SIGTERM ||
         signal_number == SIGINT  ||
				 signal_number == SIGABRT);
	// clang-format on

	// Set our function as the signal handling function
	signal_action.sa_handler = _bridge_signal_handler;

	// Don't block any other signals during our exception handler
	sigemptyset(&signal_action.sa_mask);

	// Attempt to restart syscalls after our signal handler
	// (useful only for non-terminating signals)
	signal_action.sa_flags = SA_RESTART;

	if (sigaction(signal_number, &signal_action, &old_action) == -1) {
		throw("Error setting signal handler in bridge");
	}

	if (signal_number == SIGINT) {
		old_sigint_handler = old_action.sa_handler;
	} else if (signal_number == SIGTERM) {
		old_sigterm_handler = old_action.sa_handler;
	} else {
		old_sigabrt_handler = old_action.sa_handler;
	}
}

void _bridge_signal_handler(int signal_number) {
	if (signal_number == SIGINT) {
		_bridge_signal_handler_for(SIGINT, old_sigint_handler);
	} else if (signal_number == SIGTERM) {
		_bridge_signal_handler_for(SIGTERM, old_sigterm_handler);
	} else if (signal_number == SIGABRT) {
		_bridge_signal_handler_for(SIGABRT, old_sigabrt_handler);
	}
}

void _bridge_signal_handler_for(int signal_number,
																signal_handler_t old_handler) {
	// There are five cases interesting here:
	// 1) The user has no handler set. Then we decide to exit (and destroy the
	// bridge) by calling exit().
	// 2) The user has a handler set and ignores the signal. Then we ignore it too
	// and everything can go on normally.
	// 3) The user has a handler set and calls exit() at the end. Then our bridge
	// will be destroyed via the function we registered with atexit() (all good).
	// 4) The signal is SIGABRT (abort) and the user has a handler set and exits
	// the program via exit(). Then our handler will be called and we're good.
	// 5) The signal is SIGABRT (abort) and the user has a handler set and returns
	// from his handler. According to the man pages, when a signal handler for
	// SIGABRT returns, it replaces the current handler with the default handler
	// (which does a core dump) and re-raises the SIGABRT signal. So when we
	// call the user's handler for SIGABRT and he returns, we kill the bridge
	// because we know (and hopefully the user knew) that the process will be
	// terminated anyway.

	// Note: PostGres seems to ignore SIGINT at one point. SQLite exit()s after
	// three SIGINTs are sent (maybe to prevent accidental ^C from killing it...)

	if (old_handler != NULL) {
		old_handler(signal_number);
	} else {
		exit(EXIT_FAILURE);
	}

	if (signal_number == SIGABRT) {
		bridge_destroy(&bridge);
	}
}

void _bridge_exit_handler() {
	bridge_destroy(&bridge);
}
