/*	$OpenBSD: ruleset.c,v 1.26 2012/11/12 14:58:53 eric Exp $ */

/*
 * Copyright (c) 2009 Gilles Chehade <gilles@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/param.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <stdio.h>
#include <string.h>

#include "smtpd.h"
#include "log.h"


static int ruleset_check_source(struct table *,
    const struct sockaddr_storage *);
static int ruleset_match_mask(struct sockaddr_storage *, struct netaddr *);
static int ruleset_inet4_match(struct sockaddr_in *, struct netaddr *);
static int ruleset_inet6_match(struct sockaddr_in6 *, struct netaddr *);


struct rule *
ruleset_match(const struct envelope *evp)
{
	const struct mailaddr *maddr = &evp->dest;
	const struct sockaddr_storage *ss = &evp->ss;
	struct rule	*r;
	struct table	*table;
	struct mapel	*me;
	int		 v;

	if (evp->flags & DF_INTERNAL)
		ss = NULL;

	TAILQ_FOREACH(r, env->sc_rules, r_entry) {

		if (r->r_tag[0] != '\0' && strcmp(r->r_tag, evp->tag) != 0)
			continue;

		if (ss != NULL && !(evp->flags & DF_AUTHENTICATED)) {
			v = ruleset_check_source(r->r_sources, ss);
			if (v == -1) {
				errno = EAGAIN;
				return (NULL);
			}
			if (v == 0)
				continue;
		}

		if (r->r_condition.c_type == COND_ANY)
			return r;

		if (r->r_condition.c_type == COND_DOM) {
			table = table_find(r->r_condition.c_table);
			if (table == NULL)
				fatal("failed to lookup table.");

			if (! strcmp(table->t_src, "static")) {
				TAILQ_FOREACH(me, &table->t_contents,
				    me_entry) {
					if (hostname_match(maddr->domain,
						me->me_key))
						return r;
				}
			}
			else if (table_lookup(table->t_id, maddr->domain,
			    K_VIRTUAL) != NULL) {
				return (r);
			} else if (errno) {
				errno = EAGAIN;
				return (NULL);
			}
		}

		if (r->r_condition.c_type == COND_VDOM) {
			v = aliases_vdomain_exists(r->r_condition.c_table,
			    maddr->domain);
			if (v == -1) {
				errno = EAGAIN;
				return (NULL);
			}
			if (v)
				return (r);
		}
	}

	errno = 0;
	return (NULL);
}

static int
ruleset_cmp_source(const char *s1, const char *s2)
{
	struct netaddr n1;
	struct netaddr n2;

	if (! text_to_netaddr(&n1, s1))
		return 0;

	if (! text_to_netaddr(&n2, s2))
		return 0;

	if (n1.ss.ss_family != n2.ss.ss_family)
		return 0;
	if (n1.ss.ss_len != n2.ss.ss_len)
		return 0;

	return ruleset_match_mask(&n1.ss, &n2);
}

static int
ruleset_check_source(struct table *table, const struct sockaddr_storage *ss)
{
	struct mapel *me;

	if (ss == NULL) {
		/* This happens when caller is part of an internal
		 * lookup (ie: alias resolved to a remote address)
		 */
		return 1;
	}

	if (! strcmp(table->t_src, "static")) {
		TAILQ_FOREACH(me, &table->t_contents, me_entry) {
			if (ss->ss_family == AF_LOCAL) {
				if (!strcmp(me->me_key, "local"))
					return 1;
				continue;
			}
			if (ruleset_cmp_source(ss_to_text(ss), me->me_key))
				return 1;
		}
	}
	else {
		if (table_compare(table->t_id, ss_to_text(ss), K_NETADDR,
		    ruleset_cmp_source))
			return (1);
		if (errno)
			return (-1);
	}

	return 0;
}

static int
ruleset_match_mask(struct sockaddr_storage *ss, struct netaddr *ssmask)
{
	if (ss->ss_family == AF_INET)
		return ruleset_inet4_match((struct sockaddr_in *)ss, ssmask);

	if (ss->ss_family == AF_INET6)
		return ruleset_inet6_match((struct sockaddr_in6 *)ss, ssmask);

	return (0);
}

static int
ruleset_inet4_match(struct sockaddr_in *ss, struct netaddr *ssmask)
{
	in_addr_t mask;
	int i;

	/* a.b.c.d/8 -> htonl(0xff000000) */
	mask = 0;
	for (i = 0; i < ssmask->bits; ++i)
		mask = (mask >> 1) | 0x80000000;
	mask = htonl(mask);

	/* (addr & mask) == (net & mask) */
	if ((ss->sin_addr.s_addr & mask) ==
	    (((struct sockaddr_in *)ssmask)->sin_addr.s_addr & mask))
		return 1;

	return 0;
}

static int
ruleset_inet6_match(struct sockaddr_in6 *ss, struct netaddr *ssmask)
{
	struct in6_addr	*in;
	struct in6_addr	*inmask;
	struct in6_addr	 mask;
	int		 i;

	bzero(&mask, sizeof(mask));
	for (i = 0; i < ssmask->bits / 8; i++)
		mask.s6_addr[i] = 0xff;
	i = ssmask->bits % 8;
	if (i)
		mask.s6_addr[ssmask->bits / 8] = 0xff00 >> i;

	in = &ss->sin6_addr;
	inmask = &((struct sockaddr_in6 *)&ssmask->ss)->sin6_addr;

	for (i = 0; i < 16; i++) {
		if ((in->s6_addr[i] & mask.s6_addr[i]) !=
		    (inmask->s6_addr[i] & mask.s6_addr[i]))
			return (0);
	}

	return (1);
}
