/*
 * x99_rlm.c
 * $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Copyright 2000,2001,2002  The FreeRADIUS server project
 * Copyright 2001,2002  Google, Inc.
 */

/*
 * STRONG WARNING SECTION:
 *
 * ANSI X9.9 has been withdrawn as a standard, due to the weakness of DES.
 * An attacker can learn the token's secret by observing two
 * challenge/response pairs.  See ANSI document X9 TG-24-1999
 * <URL:http://www.x9.org/TG24_1999.pdf>.
 * 
 * Please read the accompanying docs.
 */

/*
 * TODO: support max usage limit (x iterations)
 * TODO: support soft PIN? ???
 * TODO: support other than ILP32 (for State)
 */


#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>	/* htonl() */

#ifdef FREERADIUS
#include "radiusd.h"
#include "modules.h"
#endif
#include "x99.h"

static const char rcsid[] = "$Id$";

/* Global data */
static int rnd_fd;			/* fd for random device           */
static unsigned char hmac_key[16];	/* to protect State attribute     */

/* A mapping of configuration file names to internal variables. */
static CONF_PARSER module_config[] = {
    { "pwdfile", PW_TYPE_STRING_PTR, offsetof(x99_token_t, pwdfile),
      NULL, PWDFILE },
    { "syncdir", PW_TYPE_STRING_PTR, offsetof(x99_token_t, syncdir),
      NULL, SYNCDIR },
    { "challenge_prompt", PW_TYPE_STRING_PTR, offsetof(x99_token_t,chal_prompt),
      NULL, CHALLENGE_PROMPT },
    { "challenge_length", PW_TYPE_INTEGER, offsetof(x99_token_t, chal_len),
      NULL, "6" },
    { "softfail", PW_TYPE_INTEGER, offsetof(x99_token_t, softfail),
      NULL, "5" },
    { "hardfail", PW_TYPE_INTEGER, offsetof(x99_token_t, hardfail),
      NULL, "0" },
    { "allow_sync", PW_TYPE_BOOLEAN, offsetof(x99_token_t, allow_sync),
      NULL, "yes" },
    { "fast_sync", PW_TYPE_BOOLEAN, offsetof(x99_token_t, fast_sync),
      NULL, "yes" },
    { "allow_async", PW_TYPE_BOOLEAN, offsetof(x99_token_t, allow_async),
      NULL, "no" },
    { "challenge_req", PW_TYPE_STRING_PTR, offsetof(x99_token_t, chal_req),
      NULL, CHALLENGE_REQ },
    { "resync_req", PW_TYPE_STRING_PTR, offsetof(x99_token_t, resync_req),
      NULL, RESYNC_REQ },
    { "ewindow_size", PW_TYPE_INTEGER, offsetof(x99_token_t, ewindow_size),
      NULL, "0" },
    { "maxdelay", PW_TYPE_INTEGER, offsetof(x99_token_t, maxdelay),
      NULL, "30" },
    { "mschapv2_mppe", PW_TYPE_INTEGER,
      offsetof(x99_token_t, mschapv2_mppe_policy), NULL, "2" },
    { "mschapv2_mppe_bits", PW_TYPE_INTEGER,
      offsetof(x99_token_t, mschapv2_mppe_types), NULL, "2" },
    { "mschap_mppe", PW_TYPE_INTEGER,
      offsetof(x99_token_t, mschap_mppe_policy), NULL, "2" },
    { "mschap_mppe_bits", PW_TYPE_INTEGER,
      offsetof(x99_token_t, mschap_mppe_types), NULL, "2" },
#if 0
    { "twindow_min", PW_TYPE_INTEGER, offsetof(x99_token_t, twindow_min),
      NULL, "0" },
    { "twindow_max", PW_TYPE_INTEGER, offsetof(x99_token_t, twindow_max),
      NULL, "0" },
#endif

    { NULL, -1, 0, NULL, NULL }		/* end the list */
};


/* per-module initialization */
static int
x99_token_init(void)
{
    if ((rnd_fd = open(DEVURANDOM, O_RDONLY)) == -1) {
	x99_log(X99_LOG_ERR, "init: error opening %s: %s", DEVURANDOM,
		strerror(errno));
	return -1;
    }

    /* Generate a random key, used to protect the State attribute. */
    if (x99_get_random(rnd_fd, hmac_key, sizeof(hmac_key)) == -1) {
	x99_log(X99_LOG_ERR, "init: failed to obtain random data for hmac_key");
	return -1;
    }

    /* Initialize the password encoding/checking functions. */
    x99_pwe_init();

    return 0;
}


/* per-instance initialization */
static int
x99_token_instantiate(CONF_SECTION *conf, void **instance)
{
    x99_token_t *data;
    char *p;
    struct stat st;

    /* Set up a storage area for instance data. */
    data = rad_malloc(sizeof(*data));

    /* If the configuration parameters can't be parsed, then fail. */
    if (cf_section_parse(conf, data, module_config) < 0) {
	free(data);
	return -1;
    }

    /* Verify ranges for those vars that are limited. */
    if (data->chal_len < 5 || data->chal_len > MAX_CHALLENGE_LEN) {
	data->chal_len = 6;
	x99_log(X99_LOG_ERR,
		"invalid challenge length, range 5-%d, using default of 6",
		MAX_CHALLENGE_LEN);

    }

    /* Enforce a single "%" sequence, which must be "%s" */
    p = strchr(data->chal_prompt, '%');
    if (p == NULL || p != strrchr(data->chal_prompt, '%') || strncmp(p,"%s",2)){
	free(data->chal_prompt);
	data->chal_prompt = strdup(CHALLENGE_PROMPT);
	x99_log(X99_LOG_ERR,
		"invalid challenge prompt, using default of \"%s\"",
		CHALLENGE_PROMPT);
    }

    if (data->softfail < 1 ) {
	data->softfail = 5;
	x99_log(X99_LOG_ERR, "softfail must be at least 1, using default of 5");
    }

    if (data->hardfail < 0 ) {
	data->hardfail = 0;
	x99_log(X99_LOG_ERR, "hardfail must be at least 1 "
		"(or 0 == infinite), using default of 0");
    }

    if (data->fast_sync && !data->allow_sync) {
	data->fast_sync = 0;
	x99_log(X99_LOG_INFO,
		"fast_sync is yes, but allow_sync is no; disabling fast_sync");
    }

    if (data->ewindow_size > MAX_EWINDOW_SIZE || data->ewindow_size < 0) {
	data->ewindow_size = 0;
	x99_log(X99_LOG_ERR, "max event window size is %d, using default of 0",
		MAX_EWINDOW_SIZE);
    }

    if (data->mschapv2_mppe_policy > 2 || data->mschapv2_mppe_policy < 0) {
	data->mschapv2_mppe_policy = 2;
	x99_log(X99_LOG_ERR,
		"invalid value for mschapv2_mppe, using default of 2");
    }

    if (data->mschapv2_mppe_types > 2 || data->mschapv2_mppe_types < 0) {
	data->mschapv2_mppe_types = 2;
	x99_log(X99_LOG_ERR,
		"invalid value for mschapv2_mppe_bits, using default of 2");
    }

    if (data->mschap_mppe_policy > 2 || data->mschap_mppe_policy < 0) {
	data->mschap_mppe_policy = 2;
	x99_log(X99_LOG_ERR,
		"invalid value for mschap_mppe, using default of 2");
    }

    if (data->mschap_mppe_types != 2) {
	data->mschap_mppe_types = 2;
	x99_log(X99_LOG_ERR,
		"invalid value for mschap_mppe_bits, using default of 2");
    }

#if 0
    if (data->twindow_max - data->twindow_min > MAX_TWINDOW_SIZE) {
	data->twindow_min = data->twindow_max = 0;
	x99_log(X99_LOG_ERR, "max time window size is %d, using default of 0",
		MAX_TWINDOW_SIZE);
    }
    if (data->twindow_min > 0 || data->twindow_max < 0 ||
	data->twindow_max < data->twindow_min) {
	data->twindow_min = data->twindow_max = 0;
	x99_log(X99_LOG_ERR,
		"invalid values for time window, using default of 0");
    }
#endif

    if (stat(data->syncdir, &st) != 0) {
	x99_log(X99_LOG_ERR, "syncdir %s error: %s",
		data->syncdir, strerror(errno));
	return -1;
    }
    if (st.st_mode != (S_IFDIR|S_IRWXU)) {
	x99_log(X99_LOG_ERR, "syncdir %s has loose permissions", data->syncdir);
	return -1;
    }

    *instance = data;
    return 0;
}


/* Generate a challenge to be presented to the user. */
static int
x99_token_authorize(void *instance, REQUEST *request)
{
    x99_token_t *inst = (x99_token_t *) instance;

    char challenge[MAX_CHALLENGE_LEN + 1];	/* +1 for '\0' terminator */
    char *state;
    int rc;

    x99_user_info_t user_info;
    int user_found, auth_type_found;
    int pwattr;
    int32_t sflags = 0; /* flags for state */
    VALUE_PAIR *vp;

    /* Early exit if Auth-Type != x99_token */
    auth_type_found = 0;
    if ((vp = pairfind(request->config_items, PW_AUTHTYPE)) != NULL) {
	auth_type_found = 1;
	if (strcmp(vp->strvalue, "x99_token")) {
	    return RLM_MODULE_NOOP;
	}
    }

    /* The State attribute will be present if this is a response. */
    if (pairfind(request->packet->vps, PW_STATE) != NULL) {
	DEBUG("rlm_x99_token: autz: Found response to access challenge");
	return RLM_MODULE_OK;
    }

    /* User-Name attribute required. */
    if (!request->username) {
	x99_log(X99_LOG_AUTH,
		"autz: Attribute \"User-Name\" required for authentication.");
	return RLM_MODULE_INVALID;
    }

    if ((pwattr = x99_pw_present(request)) == 0) {
	x99_log(X99_LOG_AUTH, "autz: Attribute \"User-Password\" "
		"or equivalent required for authentication.");
	return RLM_MODULE_INVALID;
    }

    /* Look up the user's info. */
    user_found = 1;
    if ((rc = x99_get_user_info(inst->pwdfile, request->username->strvalue,
				&user_info)) == -2) {
	x99_log(X99_LOG_ERR, "autz: error reading user [%s] info",
		request->username->strvalue);
	return RLM_MODULE_FAIL;
    }
    if (rc == -1) {
	/* x99_get_user_info() also logs, but we want to record the autz bit */
	x99_log(X99_LOG_AUTH, "autz: user [%s] not found",
		request->username->strvalue);
	memset(&user_info, 0, sizeof(user_info)); /* X99_CF_NONE */
	user_found = 0;
    }

    /* fast_sync mode (challenge only if requested) */
    if (inst->fast_sync &&
	((user_info.card_id & X99_CF_SM) || !user_found)) {

	if ((x99_pw_valid(request, inst, pwattr, inst->resync_req, NULL) &&
		/* Set a bit indicating resync */ (sflags |= htonl(1))) ||
	    x99_pw_valid(request, inst, pwattr, inst->chal_req, NULL)) {
	    /*
	     * Generate a challenge if requested.  We don't test for card
	     * support [for async] because it's tricky for unknown users.
	     * Some configurations would have a problem where known users
	     * cannot request a challenge, but unknown users can.  This
	     * reveals information.  The easiest fix seems to be to always
	     * hand out a challenge on request.
	     * We also don't test if the server allows async mode, this
	     * would also reveal information.
	     */
	    DEBUG("rlm_x99_token: autz: fast_sync challenge requested");
	    goto gen_challenge;

	} else {
	    /*
	     * Otherwise, this is the token sync response.  Signal
	     * the authenticate code to ignore State.  We don't need
	     * to set a value, /existence/ of the vp is the signal.
	     */
	    if ((vp = paircreate(PW_X99_FAST, PW_TYPE_INTEGER)) == NULL) {
		x99_log(X99_LOG_ERR, "autz: no memory");
		return RLM_MODULE_FAIL;
	    }
	    pairadd(&request->config_items, vp);
	    DEBUG("rlm_x99_token: autz: using fast_sync");

	    if (!auth_type_found)
		pairadd(&request->config_items,
			pairmake("Auth-Type", "x99_token", T_OP_EQ));
	    return RLM_MODULE_OK;

	}
    } /* if (fast_sync && card supports sync mode) */

gen_challenge:
    /* Set the resync bit by default if the user can't request it. */
    if (!inst->fast_sync)
	sflags |= htonl(1);

    /* Generate a random challenge. */
    if (x99_get_challenge(rnd_fd, challenge, inst->chal_len) == -1) {
	x99_log(X99_LOG_ERR, "autz: failed to obtain random challenge");
	return RLM_MODULE_FAIL;
    }

    /*
     * Create the State attribute, which will be returned to us along with
     * the response.  We will need this to verify the response.  Create
     * a strong state if the user will be able use this with their token.
     * Otherwise, we discard it anyway, so don't "waste" time with hmac.
     * We also don't do the hmac if the user wasn't found (mask won't match).
     * We always create at least a trivial state, so x99_token_authorize()
     * can easily pass on to x99_token_authenticate().
     */
    if (user_info.card_id & X99_CF_AM) {
	time_t now = time(NULL);

	if (sizeof(now) != 4 || sizeof(long) != 4) {
	    x99_log(X99_LOG_ERR, "autz: only ILP32 arch is supported");
	    return RLM_MODULE_FAIL;
	}
	now = htonl(now);

	if (x99_gen_state(&state, NULL, challenge, sflags, now, hmac_key) != 0){
	    x99_log(X99_LOG_ERR, "autz: failed to generate state");
	    return RLM_MODULE_FAIL;
	}
    } else {
	/* x2 b/c pairmake() string->octet needs even num of digits */
	state = rad_malloc(3 + inst->chal_len * 2);
	(void) sprintf(state, "0x%s%s", challenge, challenge);
    }
    pairadd(&request->reply->vps, pairmake("State", state, T_OP_EQ));
    free(state);

    /* Add the challenge to the reply. */
    {
	char *u_challenge;	/* challenge with addt'l presentation text */

	u_challenge = rad_malloc(strlen(inst->chal_prompt)+MAX_CHALLENGE_LEN+1);
	(void) sprintf(u_challenge, inst->chal_prompt, challenge);
	pairadd(&request->reply->vps,
		pairmake("Reply-Message", u_challenge, T_OP_EQ));
	free(u_challenge);
    }

    /*
     * Mark the packet as an Access-Challenge packet.
     * The server will take care of sending it to the user.
     */
    request->reply->code = PW_ACCESS_CHALLENGE;
    DEBUG("rlm_x99_token: Sending Access-Challenge.");

    if (!auth_type_found)
	pairadd(&request->config_items,
		pairmake("Auth-Type", "x99_token", T_OP_EQ));
    return RLM_MODULE_HANDLED;
}


/* Verify the response entered by the user. */
static int
x99_token_authenticate(void *instance, REQUEST *request)
{
    x99_token_t *inst = (x99_token_t *) instance;

    x99_user_info_t user_info;
    char *username;
    int i, pwattr, rc;
    int32_t sflags = 0; /* flags from state */
    time_t last_auth;

    char challenge[MAX_CHALLENGE_LEN + 1];
    char e_response[9];		/* expected response */
    VALUE_PAIR *add_vps = NULL;

    /* User-Name attribute required. */
    if (!request->username) {
	x99_log(X99_LOG_AUTH,
		"auth: Attribute \"User-Name\" required for authentication.");
	return RLM_MODULE_INVALID;
    }
    username = request->username->strvalue;

    if ((pwattr = x99_pw_present(request)) == 0) {
	x99_log(X99_LOG_AUTH, "auth: Attribute \"User-Password\" "
		"or equivalent required for authentication.");
	return RLM_MODULE_INVALID;
    }

    /* Add a message to the auth log. */
    pairadd(&request->packet->vps, pairmake("Module-Failure-Message",
					    X99_MODULE_NAME, T_OP_EQ));
    pairadd(&request->packet->vps, pairmake("Module-Success-Message",
					    X99_MODULE_NAME, T_OP_EQ));

    /* Look up the user's info. */
    if (x99_get_user_info(inst->pwdfile, username, &user_info) != 0) {
	x99_log(X99_LOG_AUTH, "auth: error reading user [%s] info", username);
	return RLM_MODULE_REJECT;
    }

    /* Retrieve the challenge (from State attribute), unless (fast_sync). */
    if (pairfind(request->config_items, PW_X99_FAST) == NULL) {
	VALUE_PAIR	*vp;
	unsigned char	*state;
	time_t		then;

	if ((vp = pairfind(request->packet->vps, PW_STATE)) != NULL) {
	    int e_length = inst->chal_len;

	    /* Extend expected length if state should have been protected. */
	    if (user_info.card_id & X99_CF_AM)
		e_length += 4 + 4 + 16; /* sflags + time + hmac */

	    if (vp->length != e_length) {
		x99_log(X99_LOG_AUTH,
			"auth: bad state for [%s]: length", username);
		return RLM_MODULE_INVALID;
	    }

	    /* Fast path if we didn't protect the state. */
	    if (!(user_info.card_id & X99_CF_AM))
		goto good_state;

	    /* Verify the state. */
	    (void) memset(challenge, 0, sizeof(challenge));
	    (void) memcpy(challenge, vp->strvalue, inst->chal_len);
	    (void) memcpy(&sflags, vp->strvalue + inst->chal_len, 4);
	    (void) memcpy(&then, vp->strvalue + inst->chal_len + 4, 4);
	    if (x99_gen_state(NULL,&state,challenge,sflags,then,hmac_key) != 0){
		x99_log(X99_LOG_ERR, "auth: failed to generate state");
		return RLM_MODULE_FAIL;
	    }
	    if (memcmp(state, vp->strvalue, vp->length)) {
		x99_log(X99_LOG_AUTH,
			"auth: bad state for [%s]: hmac", username);
		free(state);
		return RLM_MODULE_REJECT;
	    }
	    free(state);

	    /* State is valid, but check expiry. */
	    then = ntohl(then);
	    if (then + inst->maxdelay < time(NULL)) {
		x99_log(X99_LOG_AUTH,
			"auth: bad state for [%s]: expired", username);
		return RLM_MODULE_REJECT;
	    }
good_state:
	    /* State is good! */

	} else {
	    /* This should only happen if the authorize code didn't run. */
	    x99_log(X99_LOG_ERR, "auth: bad state for [%s]: missing "
		    "(is x99_token listed in radiusd.conf's authorize stanza?)",
		    username);
	    return RLM_MODULE_FAIL;
	}
    } /* if (!fast_sync) */

    /* Check failure count. */
    if (x99_check_failcount(username, inst))
	return RLM_MODULE_USERLOCK;

    /*
     * Don't bother to check async response if either
     * - the card doesn't support it, or
     * - we're doing fast_sync.
     */
    if (!(user_info.card_id & X99_CF_AM) ||
	pairfind(request->config_items, PW_X99_FAST)) {
	goto sync_response;
    }

    /* Perform any site-specific transforms of the challenge. */
    if (x99_challenge_transform(username, challenge) != 0) {
	x99_log(X99_LOG_ERR,
		"auth: challenge transform failed for [%s]", username);
	return RLM_MODULE_FAIL;
	/* NB: last_auth, failcount not updated. */
    }

    /* Calculate and test the async response. */
    if (x99_response(challenge, e_response, user_info.card_id,
		     user_info.keyblock) != 0) {
	x99_log(X99_LOG_ERR,
		"auth: unable to calculate async response for [%s], "
		"to challenge %s", username, challenge);
	return RLM_MODULE_FAIL;
	/* NB: last_auth, failcount not updated. */
    }
    DEBUG("rlm_x99_token: auth: [%s], async challenge %s, "
	  "expecting response %s", username, challenge, e_response);

    if (x99_pw_valid(request, inst, pwattr, e_response, &add_vps)) {
	/* Password matches.  Is this allowed? */
	if (!inst->allow_async) {
	    x99_log(X99_LOG_AUTH,
		    "auth: bad async for [%s]: disallowed by config", username);
	    rc = RLM_MODULE_REJECT;
	    goto return_pw_valid;
	    /* NB: last_auth, failcount not updated. */
	}

	/* Make sure this isn't a replay by forcing a delay. */
	if (x99_get_last_auth(inst->syncdir, username, &last_auth) != 0) {
	    x99_log(X99_LOG_ERR,
		    "auth: unable to get last auth time for [%s]", username);
	    return RLM_MODULE_FAIL;
	}
	if (last_auth + inst->maxdelay > time(NULL)) {
	    x99_log(X99_LOG_AUTH,
		    "auth: bad async for [%s]: too soon", username);
	    rc = RLM_MODULE_REJECT;
	    goto return_pw_valid;
	    /* NB: last_auth, failcount not updated. */
	}

	if (user_info.card_id & X99_CF_SM) {
	    x99_log(X99_LOG_INFO,
		    "auth: [%s] authenticated in async mode", username);
	}

	rc = RLM_MODULE_OK;
	if (ntohl(sflags) & 1) {
	    /*
	     * Resync the card.  The sync data doesn't mean anything for
	     * async-only cards, but we want the side effects of resetting
	     * the failcount and the last auth time.  We "fail-out" if we
	     * can't do this, because if we can't update the last auth time,
	     * we will be open to replay attacks over the lifetime of the
	     * State attribute (inst->maxdelay).
	     */
	    if (x99_get_sync_data(inst->syncdir, username, user_info.card_id,
				  1, 0, challenge, user_info.keyblock) != 0) {
		x99_log(X99_LOG_ERR, "auth: unable to get sync data "
			"e:%d t:%d for [%s] (for resync)", 1, 0, username);
		rc = RLM_MODULE_FAIL;
	    } else if (x99_set_sync_data(inst->syncdir, username, challenge,
					 user_info.keyblock) != 0) {
		x99_log(X99_LOG_ERR,
			"auth: unable to set sync data for [%s] (for resync)",
			username);
		rc = RLM_MODULE_FAIL;
	    }
	} else {
	    /* Just update last_auth, failcount. */
	    if (x99_reset_failcount(inst->syncdir, username) != 0) {
		x99_log(X99_LOG_ERR,
			"auth: unable to reset failcount for [%s]", username);
		rc = RLM_MODULE_FAIL;
	    }
	}
	goto return_pw_valid;
    } /* if (user authenticated async) */

sync_response:
    /*
     * Calculate and test sync responses in the window.
     * Note that we always accept a sync response, even
     * if a challenge or resync was explicitly requested.
     */
    if ((user_info.card_id & X99_CF_SM) && inst->allow_sync) {
	for (i = 0; i <= inst->ewindow_size; ++i) {
	    /* Get sync challenge and key. */
	    if (x99_get_sync_data(inst->syncdir, username, user_info.card_id,
				  i, 0, challenge, user_info.keyblock) != 0) {
		x99_log(X99_LOG_ERR,
			"auth: unable to get sync data e:%d t:%d for [%s]",
			i, 0, username);
		return RLM_MODULE_FAIL;
		/* NB: last_auth, failcount not updated. */
	    }

	    /* Calculate sync response. */
	    if (x99_response(challenge, e_response, user_info.card_id,
			     user_info.keyblock) != 0) {
		x99_log(X99_LOG_ERR, "auth: unable to calculate sync response "
			"e:%d t:%d for [%s], to challenge %s",
			i, 0, username, challenge);
		return RLM_MODULE_FAIL;
		/* NB: last_auth, failcount not updated. */
	    }
	    DEBUG("rlm_x99_token: auth: [%s], sync challenge %d %s, "
		  "expecting response %s", username, i, challenge, e_response);

	    /* Test user-supplied password. */
	    if (x99_pw_valid(request, inst, pwattr, e_response, &add_vps)) {
		/*
		 * Yay!  User authenticated via sync mode.  Resync.
		 *
		 * The same failure/replay issue applies here as in the
		 * identical code block in the async section, above, with
		 * the additional problem that a response can be reused
		 * indefinitely!  (until the sync data is updated)
		 */
		rc = RLM_MODULE_OK;
		if (x99_get_sync_data(inst->syncdir,username,user_info.card_id,
				      1, 0 ,challenge,user_info.keyblock) != 0){
		    x99_log(X99_LOG_ERR, "auth: unable to get sync data "
			    "e:%d t:%d for [%s] (for resync)", 1, 0, username);
		    rc = RLM_MODULE_FAIL;
		} else if (x99_set_sync_data(inst->syncdir, username, challenge,
					     user_info.keyblock) != 0) {
		    x99_log(X99_LOG_ERR,
			    "auth: unable to set sync data for [%s] "
			    "(for resync)", username);
		    rc = RLM_MODULE_FAIL;
		}
		goto return_pw_valid;
	    }

	} /* for (each slot in the window) */
    } /* if (card is in sync mode and sync mode allowed) */

    /* Both async and sync mode failed. */
    if (x99_incr_failcount(inst->syncdir, username) != 0) {
	x99_log(X99_LOG_ERR,
		"auth: unable to increment failure count for user [%s]",
		username);
    }
    return RLM_MODULE_REJECT;

    /* Must exit here after a successful return from x99_pw_valid(). */
return_pw_valid:

    /* Handle any vps returned from x99_pw_valid(). */
    if (rc == RLM_MODULE_OK) {
	pairadd(&request->reply->vps, add_vps);
    } else {
	pairfree(&add_vps);
    }
    return rc;
}


/* per-instance destruction */
static int
x99_token_detach(void *instance)
{
    x99_token_t *inst = (x99_token_t *) instance;

    free(inst->pwdfile);
    free(inst->syncdir);
    free(inst->chal_prompt);
    free(inst->chal_req);
    free(inst->resync_req);
    free(instance);
    return 0;
}


/* per-module destruction */
static int
x99_token_destroy(void)
{
    (void) memset(hmac_key, 0, sizeof(hmac_key));
    (void) close(rnd_fd);
    return 0;
}

/*
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
module_t rlm_x99_token = {
	"x99_token",	
	RLM_TYPE_THREAD_SAFE,		/* type */
	x99_token_init,			/* initialization */
	x99_token_instantiate,		/* instantiation */
	{
		x99_token_authenticate,	/* authentication */
		x99_token_authorize,	/* authorization */
		NULL,			/* preaccounting */
		NULL,			/* accounting */
		NULL			/* checksimul */
	},
	x99_token_detach,		/* detach */
	x99_token_destroy,		/* destroy */
};
