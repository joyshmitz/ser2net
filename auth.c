/*
 *  ser2net - A program for allowing telnet connection to serial ports
 *  Copyright (C) 2001=2020  Corey Minyard <minyard@acm.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-only
 *
 *  In addition, as a special exception, the copyright holders of
 *  ser2net give you permission to combine ser2net with free software
 *  programs or libraries that are released under the GNU LGPL and
 *  with code included in the standard release of OpenSSL under the
 *  OpenSSL license (or modified versions of such code, with unchanged
 *  license). You may copy and distribute such a system following the
 *  terms of the GNU GPL for ser2net and the licenses of the other code
 *  concerned, provided that you include the source code of that
 *  other code when and as the GNU GPL requires distribution of source
 *  code.
 *
 *  Note that people who make modified versions of ser2net are not
 *  obligated to grant this special exception for their modified
 *  versions; it is their choice whether to do so. The GNU General
 *  Public License gives permission to release a modified version
 *  without this exception; this exception also makes it possible to
 *  release a modified version which carries forward this exception.
 */

#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <gensio/gensio.h>
#include "ser2net.h"

struct user {
    struct gensio_link link;
    char *name;
};

int
add_allowed_users(struct gensio_list **users, const char *istr,
		  struct absout *eout)
{
    char *name, *strtok_data, *str = NULL;

    if (!*users) {
	*users = malloc(sizeof(**users));
	if (!*users)
	    goto out_nomem;
	gensio_list_init(*users);
    }

    str = strdup(istr);
    if (!str)
	goto out_nomem;

    name = strtok_r(str, " \t", &strtok_data);
    while (name) {
	struct user *user = malloc(sizeof(*user));

	if (!user)
	    goto out_nomem;
	memset(user, 0, sizeof(*user));
	user->name = strdup(name);
	if (!user->name) {
	    free(user);
	    goto out_nomem;
	}
	gensio_list_add_tail(*users, &user->link);

	name = strtok_r(NULL, " \t", &strtok_data);
    }
    free(str);
    return 0;

 out_nomem:
    if (str)
	free(str);
    eout->out(eout, "Out of memory allocating allowed user list");
    return ENOMEM;
}

static bool
is_user_present(const struct gensio_list *users, char *name)
{
    struct gensio_link *u;

    if (!users)
	return true;

    gensio_list_for_each(users, u) {
	struct user *user = gensio_container_of(u, struct user, link);

	if (strcmp(user->name, name) == 0)
	    return true;
    }
    return false;
}

void
free_user_list(struct gensio_list *users)
{
    struct gensio_link *u, *u2;

    if (!users)
	return;
    gensio_list_for_each_safe(users, u, u2) {
	struct user *user = gensio_container_of(u, struct user, link);

	gensio_list_rm(users, u);
	free(user->name);
	free(user);
    }
    free(users);
}

/*
 * The next few functions are for authentication handling.
 */
static int
handle_auth_begin(struct gensio *net, const char *authdir,
		  const struct gensio_list *allowed_users)
{
    gensiods len;
    char username[100];
    int err;

    len = sizeof(username);
    err = gensio_control(net, 0, true, GENSIO_CONTROL_USERNAME, username,
			 &len);
    if (err) {
	syslog(LOG_ERR, "No username provided by remote: %s",
	       gensio_err_to_str(err));
	return GE_AUTHREJECT;
    }
    if (!is_user_present(allowed_users, username)) {
	syslog(LOG_ERR, "Username not allowed for this connection: %s",
	       username);
	return GE_AUTHREJECT;
    }

    return GE_NOTSUP;
}

static int
handle_precert(struct gensio *net, const char *authdir)
{
    gensiods len;
    char username[100];
    char filename[PATH_MAX];
    int err;
    char *s = username;

    len = sizeof(username);
    err = gensio_control(net, 0, true, GENSIO_CONTROL_USERNAME, username,
			 &len);
    if (err) {
	/* Try to get the username from the cert common name. */
	snprintf(username, sizeof(username), "-1,CN");
	len = sizeof(username);
	err = gensio_control(net, 0, true, GENSIO_CONTROL_GET_PEER_CERT_NAME,
			     username, &len);
	if (err) {
	    syslog(LOG_ERR, "No username provided by remote or cert: %s",
		   gensio_err_to_str(err));
	    return GE_AUTHREJECT;
	}
	/* Skip over the <n>,CN, in the username output. */
	s = strchr(username, ',');
	if (s)
	    s = strchr(s + 1, ',');
	if (!s) {
	    syslog(LOG_ERR, "Got invalid username: %s", username);
	    return GE_AUTHREJECT;
	}
	s++;

	/* Set the username so it's available later. */
	err = gensio_control(net, 0, false, GENSIO_CONTROL_USERNAME, s,
			     NULL);
	if (err) {
	    syslog(LOG_ERR, "Unable to set username to %s: %s", s,
		   gensio_err_to_str(err));
	    return GE_AUTHREJECT;
	}
    }

    snprintf(filename, sizeof(filename), "%s/%s/allowed_certs/",
	     authdir, s);
    err = gensio_control(net, 0, false, GENSIO_CONTROL_CERT_AUTH,
			 filename, &len);
    if (err && err != GE_CERTNOTFOUND) {
	syslog(LOG_ERR, "Unable to set authdir to %s: %s", filename,
	       gensio_err_to_str(err));
    }
    return GE_NOTSUP;
}

static int
handle_password(struct gensio *net, const char *authdir, const char *password)
{
    gensiods len;
    char username[100];
    char filename[PATH_MAX];
    FILE *pwfile;
    char readpw[100], *s;
    int err;

    len = sizeof(username);
    err = gensio_control(net, 0, true, GENSIO_CONTROL_USERNAME, username,
			 &len);
    if (err) {
	syslog(LOG_ERR, "No username provided by remote: %s",
	       gensio_err_to_str(err));
	return GE_AUTHREJECT;
    }

    snprintf(filename, sizeof(filename), "%s/%s/password",
	     authdir, username);
    pwfile = fopen(filename, "r");
    if (!pwfile) {
	syslog(LOG_ERR, "Can't open password file %s: %s", filename,
	       strerror(errno));
	return GE_AUTHREJECT;
    }
    s = fgets(readpw, sizeof(readpw), pwfile);
    fclose(pwfile);
    if (!s) {
	syslog(LOG_ERR, "Can't read password file %s: %s", filename,
	       strerror(errno));
	return GE_AUTHREJECT;
    }
    s = strchr(readpw, '\n');
    if (s)
	*s = '\0';
    if (strcmp(readpw, password) == 0)
	return 0;
    return GE_NOTSUP;
}

int
handle_acc_auth_event(const char *authdir,
		      const struct gensio_list *allowed_users,
		      int event, void *data)
{
    switch (event) {
    case GENSIO_ACC_EVENT_AUTH_BEGIN:
	if (!authdir)
	    return 0;
	return handle_auth_begin(data, authdir, allowed_users);

    case GENSIO_ACC_EVENT_PRECERT_VERIFY:
	if (!authdir)
	    return 0;
	return handle_precert(data, authdir);

    case GENSIO_ACC_EVENT_PASSWORD_VERIFY: {
	struct gensio_acc_password_verify_data *pwdata;
	if (!authdir)
	    return 0;
	pwdata = (struct gensio_acc_password_verify_data *) data;
	return handle_password(pwdata->io, authdir, pwdata->password);
    }

    default:
	return GE_NOTSUP;
    }
}
