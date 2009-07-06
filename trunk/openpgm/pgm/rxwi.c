/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * A basic receive window: pointer array implementation.
 *
 * Copyright (c) 2006-2009 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/uio.h>

//#define RXW_DEBUG

#ifndef RXW_DEBUG
#	define G_DISABLE_ASSERT
#endif

#include <glib.h>

#include <pgm/skbuff.h>
#include <pgm/rxwi.h>
#include <pgm/sn.h>
#include <pgm/timer.h>
#include <pgm/reed_solomon.h>

#ifndef RXW_DEBUG
#define g_trace(...)		while (0)
#else
#define g_trace(...)		g_debug(__VA_ARGS__)
#endif


/* testing function: is TSI null
 *
 * returns TRUE if null, returns FALSE if not null.
 */

static inline
gboolean
pgm_tsi_is_null (
	pgm_tsi_t* const	tsi
	)
{
	pgm_tsi_t nulltsi;

/* pre-conditions */
	g_assert (tsi);

	memset (&nulltsi, 0, sizeof(nulltsi));
	return 0 == memcmp (&nulltsi, tsi, sizeof(nulltsi));
}

static inline void pgm_rxw_define (pgm_rxw_t* const, const guint32);
static inline void pgm_rxw_update_trail (pgm_rxw_t* const, const guint32);
static inline guint32 pgm_rxw_update_lead (pgm_rxw_t* const, const guint32, const pgm_time_t);
static inline guint32 pgm_rxw_tg_sqn (pgm_rxw_t* const, const guint32);
static inline guint32 pgm_rxw_pkt_sqn (pgm_rxw_t* const, const guint32);
static inline gboolean pgm_rxw_is_first_of_tg_sqn (pgm_rxw_t* const, const guint32);
static inline gboolean pgm_rxw_is_last_of_tg_sqn (pgm_rxw_t* const, const guint32);
static inline void pgm_rxw_remove_tg_sqn (pgm_rxw_t* const, const guint32);
static inline int pgm_rxw_insert (pgm_rxw_t* const, struct pgm_sk_buff_t* const);
static inline int pgm_rxw_append (pgm_rxw_t* const, struct pgm_sk_buff_t* const);
static inline void pgm_rxw_add_placeholder_range (pgm_rxw_t* const, const guint32, const pgm_time_t);
static inline void _pgm_rxw_unlink (pgm_rxw_t* const, struct pgm_sk_buff_t*);
static inline guint _pgm_rxw_remove_trail (pgm_rxw_t* const);
static inline void _pgm_rxw_lost (pgm_rxw_t* const, const guint32);
static inline void _pgm_rxw_state (pgm_rxw_t*, struct pgm_sk_buff_t*, pgm_pkt_state_e);
static inline void pgm_rxw_shuffle_parity (pgm_rxw_t* const, struct pgm_sk_buff_t* const);
static inline gssize pgm_rxw_incoming_read (pgm_rxw_t* const, pgm_msgv_t**, guint);
static inline gboolean pgm_rxw_is_apdu_complete (pgm_rxw_t* const, const guint32, const guint);
static inline gssize pgm_rxw_incoming_read_apdu (pgm_rxw_t* const, pgm_msgv_t**, guint);
static inline int pgm_rxw_recovery_update (pgm_rxw_t* const, const guint32, const pgm_time_t);
static inline int pgm_rxw_recovery_append (pgm_rxw_t* const, const pgm_time_t);


/* returns the pointer at the given index of the window.
 */

static inline
struct pgm_sk_buff_t*
_pgm_rxw_peek (
	const pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
	struct pgm_sk_buff_t* skb;

/* pre-conditions */
	g_assert (window);

	if (pgm_rxw_is_empty (window))
		return NULL;

	if (pgm_uint32_gte (sequence, window->trail) && pgm_uint32_lte (sequence, window->lead))
	{
		const guint32 index_ = sequence % pgm_rxw_max_length (window);
		skb = window->pdata[index_];
/* availability only guaranteed inside commit window */
		if (pgm_uint32_lte (sequence, window->commit_lead)) {
			g_assert (skb);
			g_assert (pgm_skb_is_valid (skb));
			g_assert (pgm_tsi_is_null (&skb->tsi));
		}
	}
	else
		skb = NULL;

	return skb;
}

/* sections of the receive window:
 * 
 *  |     Commit       |   Incoming   |
 *  |<---------------->|<------------>|
 *  |                  |              |
 * trail         commit-lead        lead
 *
 * commit buffers are currently held by the application, the window trail
 * cannot be advanced if packets remain in the commit buffer.
 *
 * incoming buffers are waiting to be passed to the application.
 */

static inline
guint32
pgm_rxw_commit_length (
	const pgm_rxw_t* const	window
	)
{
	g_assert (window);
	return window->commit_lead - window->trail;
}

static inline
gboolean
pgm_rxw_commit_is_empty (
	const pgm_rxw_t* const	window
	)
{
	g_assert (window);
	return pgm_rxw_commit_length (window) == 0;
}

static inline
guint32
pgm_rxw_incoming_length (
	const pgm_rxw_t* const	window
	)
{
	g_assert (window);
	return ( 1 + window->lead ) - window->commit_lead;
}

static inline
gboolean
pgm_rxw_incoming_is_empty (
	const pgm_rxw_t* const	window
	)
{
	g_assert (window);
	return pgm_rxw_incoming_length (window) == 0;
}

/* constructor for receive window.  zero-length windows are not permitted.
 *
 * returns pointer to window.
 */

pgm_rxw_t*
pgm_rxw_init (
	const pgm_tsi_t*	tsi,
	const guint16		tpdu_size,
	const guint32		sqns,		/* transmit window size in sequence numbers */
	const guint		secs,		/* size in seconds */
	const guint		max_rte		/* max bandwidth */
	)
{
	pgm_rxw_t* window;

/* pre-conditions */
	g_assert_cmpuint (tpdu_size, >, 0);
	if (sqns) {
		g_assert_cmpuint (sqns, >, 0);
		g_assert_cmpuint (sqns & PGM_UINT32_SIGN_BIT, ==, 0);
		g_assert_cmpuint (secs, ==, 0);
		g_assert_cmpuint (max_rte, ==, 0);
	} else {
		g_assert_cmpuint (secs, >, 0);
		g_assert_cmpuint (max_rte, >, 0);
	}

	g_trace ("init (tsi:%s max-tpdu:%" G_GUINT16_FORMAT " sqns:%" G_GUINT32_FORMAT  " secs %u max-rte %u).\n",
		pgm_print_tsi (tsi), tpdu_size, sqns, secs, max_rte);

/* calculate receive window parameters */
	guint32 alloc_sqns;

	if (sqns)
	{
		alloc_sqns = sqns;
	}
	else if (secs && max_rte)
	{
		alloc_sqns = (secs * max_rte) / tpdu_size;
	}
	else
	{
		g_assert_not_reached();
	}

	window = g_slice_alloc0 (sizeof(pgm_rxw_t) + ( alloc_sqns * sizeof(struct pgm_sk_buff_t*) ));

	window->tsi		= tsi;
	window->max_tpdu	= tpdu_size;

/* empty state:
 *
 * trail = 0, lead = -1
 * commit_trail = commit_lead = rxw_trail = rxw_trail_init = 0
 */
	window->lead = -1;
	window->trail = window->lead + 1;

/* limit retransmit requests on late session joining */
	window->is_constrained = TRUE;

/* pointer array */
	window->alloc = alloc_sqns;

/* post-conditions */
	g_assert_cmpuint (pgm_rxw_max_length (window), ==, alloc_sqns);
	g_assert_cmpuint (pgm_rxw_length (window), ==, 0);
	g_assert_cmpuint (pgm_rxw_size (window), ==, 0);
	g_assert (pgm_rxw_is_empty (window));
	g_assert (!pgm_rxw_is_full (window));

	return window;
}

/* destructor for receive window.  must not be called more than once for same window.
 */

void
pgm_rxw_shutdown (
	pgm_rxw_t* const	window
	)
{
/* pre-conditions */
	g_assert (window);
	g_assert_cmpuint (window->alloc, >, 0);

	g_trace ("shutdown (window:%p)", (gpointer)window);

/* contents of window */
	while (!pgm_rxw_is_empty (window)) {
#ifndef G_DISABLE_ASSERT
		int e = _pgm_rxw_remove_trail (window);
		g_assert (0 == e);
#else
		_pgm_rxw_remove_trail (window);
#endif
	}

/* window must now be empty */
	g_assert_cmpuint (pgm_rxw_length (window), ==, 0);
	g_assert_cmpuint (pgm_rxw_size (window), ==, 0);
	g_assert (pgm_rxw_is_empty (window));
	g_assert (!pgm_rxw_is_full (window));

/* window */
	g_slice_free1 (sizeof(pgm_rxw_t) + ( window->alloc * sizeof(struct pgm_sk_buff_t*) ), window);
}

/* add skb to receive window.  window has fixed size and will not grow.
 * PGM skbuff data/tail pointers must point to the PGM payload, and hence skb->len
 * is allowed to be zero.
 *
 * if the skb sequence number indicates lost packets placeholders will be defined
 * for each missing entry in the window.
 *
 * side effects:
 *
 * 1) sequence number is set in skb from PGM header value.
 * 2) window may be updated with new skb.
 * 3) placeholders may be created for detected lost packets.
 * 4) parity skbs may be shuffled to accomodate original data.
 *
 * returns:
 * PGM_RXW_INSERTED - packet filled a waiting placeholder, skb consumed.
 * PGM_RXW_APPENDED - packet advanced window lead, skb consumed.
 * PGM_RXW_MISSING - missing packets detected whilst window lead was adanced, skb consumed.
 * PGM_RXW_DUPLICATE - re-transmission of previously seen packet.
 * PGM_RXW_MALFORMED - corrupted or invalid packet.
 * PGM_RXW_BOUNDS - packet out of window.
 *
 * it is an error to try to free the skb after adding to the window.
 */

int
pgm_rxw_add (
	pgm_rxw_t* const		window,
	struct pgm_sk_buff_t* const	skb,
	const pgm_time_t		nak_rb_expiry	/* calculated expiry time for this skb */
	)
{
	pgm_rxw_state_t* const state = (pgm_rxw_state_t*)&skb->cb;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);
	g_assert_cmpuint (nak_rb_expiry, >, 0);
	g_assert_cmpuint (pgm_rxw_max_length (window), >, 0);
	g_assert (pgm_skb_is_valid (skb));
	g_assert (((const GList*)skb)->next == NULL);
	g_assert (((const GList*)skb)->prev == NULL);
	g_assert (!pgm_tsi_is_null (&skb->tsi));
	g_assert (sizeof(struct pgm_header) + sizeof(struct pgm_data) <= (guint8*)skb->data - (guint8*)skb->head);
	g_assert (skb->len == (guint8*)skb->tail - (guint8*)skb->data);

	g_trace ("add (window:%p skb:%p, nak_rb_expiry:%" PGM_TIME_FORMAT ")",
		(gpointer)window, (gpointer)skb, nak_rb_expiry);

	skb->sequence = g_ntohl (skb->pgm_data->data_sqn);

/* verify fragment header for original data, parity packets include a
 * parity fragment header
 */
	if (!(skb->pgm_header->pgm_options & PGM_OPT_PARITY) &&
	    skb->pgm_opt_fragment)
	{
/* protocol sanity check: single fragment APDU */
		if (g_ntohl (skb->of_apdu_len) == skb->len)
			skb->pgm_opt_fragment = NULL;

/* protocol sanity check: minimum APDU length */
		if (g_ntohl (skb->of_apdu_len) < skb->len)
			return PGM_RXW_MALFORMED;

/* protocol sanity check: sequential ordering */
		if (pgm_uint32_gt (g_ntohl (skb->of_apdu_first_sqn), skb->sequence))
			return PGM_RXW_MALFORMED;

/* protocol sanity check: maximum APDU length */
		if (g_ntohl (skb->of_apdu_len) > PGM_MAX_APDU)
			return PGM_RXW_MALFORMED;
	}

/* first packet of a session defines the window */
	if (!window->is_defined)
		pgm_rxw_define (window, skb->sequence);
	else
		pgm_rxw_update_trail (window, g_ntohl (skb->pgm_data->data_trail));

/* bounds checking for parity data occurs at the transmission group sequence number */
	if (skb->pgm_header->pgm_options & PGM_OPT_PARITY)
	{
		if (pgm_uint32_lt (pgm_rxw_tg_sqn (window, skb->sequence), pgm_rxw_tg_sqn (window, window->commit_lead)))
			return PGM_RXW_DUPLICATE;

		if (pgm_uint32_lt (pgm_rxw_tg_sqn (window, skb->sequence), pgm_rxw_tg_sqn (window, window->lead)))
			return pgm_rxw_insert (window, skb);

		const struct pgm_sk_buff_t* const first_skb = _pgm_rxw_peek (window, pgm_rxw_tg_sqn (window, skb->sequence));
		const pgm_rxw_state_t* const first_state = (pgm_rxw_state_t*)&first_skb->cb;

		if (pgm_rxw_tg_sqn (window, skb->sequence) == pgm_rxw_tg_sqn (window, window->lead)) {
			if (NULL == first_state || first_state->is_contiguous) {
				state->is_contiguous = 1;
				return pgm_rxw_append (window, skb);
			} else
				return pgm_rxw_insert (window, skb);
		}

		g_assert (first_state);
		pgm_rxw_add_placeholder_range (window, pgm_rxw_tg_sqn (window, skb->sequence), nak_rb_expiry);
		return pgm_rxw_append (window, skb);
	}
	else
	{
		if (pgm_uint32_lte (skb->sequence, window->commit_lead))
			return PGM_RXW_DUPLICATE;

		if (pgm_uint32_lte (skb->sequence, window->lead))
			return pgm_rxw_insert (window, skb);

		if (skb->sequence == window->lead) {
			if (pgm_rxw_is_first_of_tg_sqn (window, skb->sequence))
				state->is_contiguous = 1;
			return pgm_rxw_append (window, skb);
		}

		pgm_rxw_add_placeholder_range (window, skb->sequence, nak_rb_expiry);
		return pgm_rxw_append (window, skb);
	}

	g_assert_not_reached();
}

/* trail is the next packet to commit upstream, lead is the leading edge
 * of the receive window with possible gaps inside, rxw_trail is the transmit
 * window trail for retransmit requests.
 */

/* define window by parameters of first data packet.
 */

static inline
void
pgm_rxw_define (
	pgm_rxw_t* const	window,
	const guint32		lead
	)
{
/* pre-conditions */
	g_assert (window);
	g_assert (pgm_rxw_is_empty (window));
	g_assert (pgm_rxw_commit_is_empty (window));
	g_assert (pgm_rxw_incoming_is_empty (window));
	g_assert (!window->is_defined);

	window->lead = lead - 1;
	window->commit_lead = window->rxw_trail = window->rxw_trail_init = window->trail = window->lead + 1;
	window->is_constrained = window->is_defined = TRUE;

/* post-conditions */
	g_assert (pgm_rxw_is_empty (window));
	g_assert (pgm_rxw_commit_is_empty (window));
	g_assert (pgm_rxw_incoming_is_empty (window));
	g_assert (window->is_defined);
	g_assert (window->is_constrained);
}

/* update window with latest transmitted parameters.
 *
 * returns count of placeholders added into window, used to start sending naks.
 */

guint32
pgm_rxw_update (
	pgm_rxw_t* const	window,
	const guint32		txw_trail,
	const guint32		txw_lead,
	const pgm_time_t	nak_rb_expiry		/* packet expiration time */
	)
{
/* pre-conditions */
	g_assert (window);
	g_assert_cmpuint (nak_rb_expiry, >, 0);

	if (!window->is_defined)
		pgm_rxw_define (window, txw_lead);

	pgm_rxw_update_trail (window, txw_trail);
	return pgm_rxw_update_lead (window, txw_lead, nak_rb_expiry);
}

/* update trailing edge of receive window
 */

static inline
void
pgm_rxw_update_trail (
	pgm_rxw_t* const	window,
	const guint32		txw_trail
	)
{
/* pre-conditions */
	g_assert (window);

/* retransmissions requests are constrained on startup until the advertised trail advances
 * beyond the first data sequence number.
 */
	if (window->is_constrained)
	{
		if (pgm_uint32_gt (txw_trail, window->rxw_trail_init))
			window->is_constrained = FALSE;
		else
			return;
	}

/* advertised trail is less than the current value */
	if (pgm_uint32_lte (txw_trail, window->rxw_trail))
		return;

	window->rxw_trail = txw_trail;

/* new value doesn't affect window */
	if (pgm_uint32_lte (window->rxw_trail, window->trail))
		return;

/* jump remaining sequence numbers if window is empty */
	if (pgm_rxw_is_empty (window))
	{
		const guint32 distance = (gint32)(window->rxw_trail) - (gint32)(window->trail);
		window->commit_lead = window->trail += distance;
		window->lead += distance;
		window->cumulative_losses += distance;
		g_assert (pgm_rxw_is_empty (window));
		g_assert (pgm_rxw_commit_is_empty (window));
		g_assert (pgm_rxw_incoming_is_empty (window));
		return;
	}

/* remove all buffers between commit lead and advertised rxw_trail */
	for (guint32 sequence = window->commit_lead;
	     pgm_uint32_gt (window->rxw_trail, sequence);
	     sequence++)
	{
		_pgm_rxw_lost (window, sequence);
	}

/* post-conditions */
	g_assert (!pgm_rxw_is_full (window));
}

/* add one placeholder to leading edge due to detected lost packet.
 */

static
void
pgm_rxw_add_placeholder (
	pgm_rxw_t* const	window,
	const pgm_time_t	nak_rb_expiry
	)
{
	struct pgm_sk_buff_t* skb;

/* pre-conditions */
	g_assert (window);
	g_assert (!pgm_rxw_is_full (window));

	skb			= pgm_alloc_skb (window->max_tpdu);
	pgm_rxw_state_t* state	= (pgm_rxw_state_t*)&skb->cb;
	skb->tstamp		= pgm_time_now;
	skb->sequence		= ++(window->lead);
	state->nak_rb_expiry	= nak_rb_expiry;

	if (!pgm_rxw_is_first_of_tg_sqn (window, skb->sequence))
	{
		struct pgm_sk_buff_t* first_skb = _pgm_rxw_peek (window, pgm_rxw_tg_sqn (window, skb->sequence));
		if (first_skb) {
			pgm_rxw_state_t* first_state = (pgm_rxw_state_t*)&first_skb->cb;
			first_state->is_contiguous = 0;
		}
	}

/* add skb to window */
	const guint32 index_	= skb->sequence % pgm_rxw_max_length (window);
	window->pdata[index_]	= skb;

	pgm_rxw_state (window, skb, PGM_PKT_BACK_OFF_STATE);

/* post-conditions */
	g_assert_cmpuint (pgm_rxw_length (window), >, 0);
	g_assert_cmpuint (pgm_rxw_length (window), <=, pgm_rxw_max_length (window));
	g_assert_cmpuint (pgm_rxw_incoming_length (window), >, 0);
}

/* add a range of placeholders to the window.
 */

static
void
pgm_rxw_add_placeholder_range (
	pgm_rxw_t* const	window,
	const guint32		sequence,
	const pgm_time_t	nak_rb_expiry
	)
{
/* pre-conditions */
	g_assert (window);
	g_assert (pgm_uint32_gt (sequence, pgm_rxw_lead (window)));

/* check bounds of commit window */
	const guint32 new_commit_sqns = ( 1 + sequence ) - window->trail;
        if ( !pgm_rxw_commit_is_empty (window) &&
	     (new_commit_sqns >= pgm_rxw_length (window)) )
        {
		pgm_rxw_update_lead (window, sequence, nak_rb_expiry);
		return;		/* effectively a slow consumer */
        }

	if (pgm_rxw_is_full (window))
	{
		_pgm_rxw_remove_trail (window);
	}

/* if packet is non-contiguous to current leading edge add place holders
 * TODO: can be rather inefficient on packet loss looping through dropped sequence numbers
 */
	while (pgm_rxw_next_lead (window) != sequence)
	{
		pgm_rxw_add_placeholder (window, nak_rb_expiry);
		if (pgm_rxw_is_full (window))
		{
			_pgm_rxw_remove_trail (window);
		}
	}

/* post-conditions */
	g_assert (!pgm_rxw_is_full (window));
}

/* update leading edge of receive window.
 *
 * returns number of place holders added.
 */

static
guint32
pgm_rxw_update_lead (
	pgm_rxw_t* const	window,
	const guint32		txw_lead,
	const pgm_time_t	nak_rb_expiry
	)
{
/* pre-conditions */
	g_assert (window);

/* advertised lead is less than the current value */
	if (pgm_uint32_lte (txw_lead, window->lead))
		return 0;

	guint32 lead;

/* committed packets limit constrain the lead until they are released */
	if (!pgm_rxw_commit_is_empty (window) &&
	    (txw_lead - window->trail) >= pgm_rxw_max_length (window))
	{
		lead = window->trail + pgm_rxw_max_length (window) - 1;
		if (lead == window->lead)
			return 0;
	}
	else
		lead = txw_lead;

	guint32 lost = 0;

	while (window->lead != lead)
	{
/* slow consumer or fast producer */
		if (pgm_rxw_is_full (window))
		{
			_pgm_rxw_remove_trail (window);
		}
		pgm_rxw_add_placeholder (window, nak_rb_expiry);
		lost++;
	}

	return lost;
}

/* checks whether an APDU is unrecoverable due to lost TPDUs.
 */
static inline
gboolean
pgm_rxw_is_apdu_lost (
	pgm_rxw_t* const		window,
	struct pgm_sk_buff_t* const	skb
	)
{
	const pgm_rxw_state_t* state = (pgm_rxw_state_t*)&skb->cb;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);

/* lost is lost */
	if (PGM_PKT_LOST_DATA_STATE == state->state)
		return TRUE;

/* by definition, a single-TPDU APDU is complete */
	if (!skb->pgm_opt_fragment)
		return FALSE;

	const guint32 apdu_first_sqn = g_ntohl (skb->of_apdu_first_sqn);

/* by definition, first fragment indicates APDU is available */
	if (apdu_first_sqn == skb->sequence)
		return FALSE;

	const struct pgm_sk_buff_t* const first_skb = _pgm_rxw_peek (window, apdu_first_sqn);
/* first fragment out-of-bounds */
	if (NULL == first_skb)
		return TRUE;

	const pgm_rxw_state_t* first_state = (pgm_rxw_state_t*)&first_skb->cb;
	if (PGM_PKT_LOST_DATA_STATE == first_state->state)
		return TRUE;

	return FALSE;
}

/* return the first missing packet sequence in the specified transmission
 * group or NULL if not required.
 */

static inline
struct pgm_sk_buff_t*
pgm_rxw_find_missing (
	pgm_rxw_t* const		window,
	const guint32			tg_sqn		/* tg_sqn | pkt_sqn */
	)
{
	struct pgm_sk_buff_t* skb;
	pgm_rxw_state_t* state;

/* pre-conditions */
	g_assert (window);

	for (guint32 i = tg_sqn, j = 0; j < window->tg_size; i++, j++)
	{
		skb = _pgm_rxw_peek (window, i);
		g_assert (skb);
		state = (pgm_rxw_state_t*)&skb->cb;
		switch (state->state) {
		case PGM_PKT_BACK_OFF_STATE:
		case PGM_PKT_WAIT_NCF_STATE:
		case PGM_PKT_WAIT_DATA_STATE:
		case PGM_PKT_LOST_DATA_STATE:
			return skb;

		case PGM_PKT_HAVE_DATA_STATE:
		case PGM_PKT_HAVE_PARITY_STATE:
			break;

		default: g_assert_not_reached(); break;
		}
	}

	return NULL;
}

/* returns TRUE if skb is a parity packet with packet length not
 * matching the transmission group length without the variable-packet-length
 * flag set.
 */

static inline
gboolean
pgm_rxw_is_invalid_var_pktlen (
	pgm_rxw_t* const			window,
	const struct pgm_sk_buff_t* const	skb
	)
{
	const struct pgm_sk_buff_t* first_skb;

/* pre-conditions */
	g_assert (window);

	if (!window->is_fec_available)
		return FALSE;

	if (skb->pgm_header->pgm_options & PGM_OPT_VAR_PKTLEN)
		return FALSE;

	const guint32 tg_sqn = pgm_rxw_tg_sqn (window, skb->sequence);
	if (tg_sqn == skb->sequence)
		return FALSE;

	first_skb = _pgm_rxw_peek (window, tg_sqn);
	if (NULL == first_skb)
		return TRUE;	/* transmission group unrecoverable */

	if (first_skb->len == skb->len)
		return FALSE;

	return TRUE;
}

static inline
gboolean
pgm_rxw_has_payload_op (
	const struct pgm_sk_buff_t* const	skb
	)
{
/* pre-conditions */
	g_assert (skb);
	g_assert (skb->pgm_header);

	return skb->pgm_opt_fragment || skb->pgm_header->pgm_options & PGM_OP_ENCODED;
}

/* returns TRUE is skb options are invalid when compared to the transmission group
 */

static inline
gboolean
pgm_rxw_is_invalid_payload_op (
	pgm_rxw_t* const			window,
	const struct pgm_sk_buff_t* const	skb
	)
{
	const struct pgm_sk_buff_t* first_skb;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);

	if (!window->is_fec_available)
		return FALSE;

	const guint32 tg_sqn = pgm_rxw_tg_sqn (window, skb->sequence);
	if (tg_sqn == skb->sequence)
		return FALSE;

	first_skb = _pgm_rxw_peek (window, tg_sqn);
	if (NULL == first_skb)
		return TRUE;	/* transmission group unrecoverable */

	if (pgm_rxw_has_payload_op (first_skb) == pgm_rxw_has_payload_op (skb))
		return FALSE;

	return TRUE;
}

/* insert skb into window range, discard if duplicate.  window will have placeholder,
 * parity, or data packet already matching sequence.
 *
 * returns:
 * PGM_RXW_INSERTED - packet filled a waiting placeholder, skb consumed.
 * PGM_RXW_DUPLICATE - re-transmission of previously seen packet.
 * PGM_RXW_MALFORMED - corrupted or invalid packet.
 * PGM_RXW_BOUNDS - packet out of window.
 */

static inline
int
pgm_rxw_insert (
	pgm_rxw_t* const		window,
	struct pgm_sk_buff_t* const	new_skb
	)
{
	struct pgm_sk_buff_t* skb;
	pgm_rxw_state_t* state;

/* pre-conditions */
	g_assert (window);
	g_assert (new_skb);
	g_assert (!pgm_rxw_incoming_is_empty (window));

	if (pgm_rxw_is_invalid_var_pktlen (window, new_skb) ||
	    pgm_rxw_is_invalid_payload_op (window, new_skb))
		return PGM_RXW_MALFORMED;

	if (new_skb->pgm_header->pgm_options & PGM_OPT_PARITY)
	{
		skb = pgm_rxw_find_missing (window, new_skb->sequence);
		if (NULL == skb)
			return PGM_RXW_DUPLICATE;
		state = (pgm_rxw_state_t*)&skb->cb;
	}
	else
	{
		skb = _pgm_rxw_peek (window, new_skb->sequence);
		g_assert (skb);
		state = (pgm_rxw_state_t*)&skb->cb;

		if (state->state == PGM_PKT_HAVE_DATA_STATE)
			return PGM_RXW_DUPLICATE;
	}

/* APDU fragments are already declared lost */
	if (new_skb->pgm_opt_fragment &&
	    pgm_rxw_is_apdu_lost (window, new_skb))
	{
		_pgm_rxw_lost (window, skb->sequence);
		return PGM_RXW_BOUNDS;
	}

/* verify placeholder state */
	switch (state->state) {
	case PGM_PKT_BACK_OFF_STATE:
	case PGM_PKT_WAIT_NCF_STATE:
	case PGM_PKT_WAIT_DATA_STATE:
	case PGM_PKT_LOST_DATA_STATE:
		_pgm_rxw_unlink (window, skb);
		break;

	case PGM_PKT_HAVE_PARITY_STATE:
		pgm_rxw_shuffle_parity (window, skb);
		break;

	default: g_assert_not_reached(); break;
	}

/* statistics */
	const guint32 fill_time = skb->tstamp - new_skb->tstamp;
	if (!window->max_fill_time) {
		window->max_fill_time = window->min_fill_time = fill_time;
	}
	else
	{
		if (fill_time > window->max_fill_time)
			window->max_fill_time = fill_time;
		else if (fill_time < window->min_fill_time)
			window->min_fill_time = fill_time;

		if (!window->max_nak_transmit_count) {
			window->max_nak_transmit_count = window->min_nak_transmit_count = state->nak_transmit_count;
		} else {
			if (state->nak_transmit_count > window->max_nak_transmit_count)
				window->max_nak_transmit_count = state->nak_transmit_count;
			else if (state->nak_transmit_count < window->min_nak_transmit_count)
				window->min_nak_transmit_count = state->nak_transmit_count;
		}
	}

/* replace place holder skb with incoming skb */
	memcpy (new_skb->cb, skb->cb, sizeof(skb->cb));
	pgm_free_skb (skb);
	const guint32 index_ = new_skb->sequence % pgm_rxw_max_length (window);
	window->pdata[index_] = new_skb;
	if (new_skb->pgm_header->pgm_options & PGM_OPT_PARITY)
		_pgm_rxw_state (window, new_skb, PGM_PKT_HAVE_PARITY_STATE);
	else
		_pgm_rxw_state (window, new_skb, PGM_PKT_HAVE_DATA_STATE);

	return PGM_RXW_INSERTED;
}

/* shuffle parity packet at skb->sequence to any other needed spot.
 */

static inline
void
pgm_rxw_shuffle_parity (
	pgm_rxw_t* const		window,
	struct pgm_sk_buff_t* const	skb
	)
{
	guint32 index_;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);

	struct pgm_sk_buff_t* missing = pgm_rxw_find_missing (window, skb->sequence);
	if (NULL == missing)
		return;

/* replace place holder skb with parity skb */
	char cb[48];
	_pgm_rxw_unlink (window, skb);
	_pgm_rxw_unlink (window, missing);
	memcpy (cb, skb->cb, sizeof(skb->cb));
	memcpy (skb->cb, missing->cb, sizeof(skb->cb));
	memcpy (missing->cb, cb, sizeof(skb->cb));
	index_ = skb->sequence % pgm_rxw_max_length (window);
	window->pdata[index_] = skb;
	index_ = missing->sequence % pgm_rxw_max_length (window);
	window->pdata[index_] = missing;
	_pgm_rxw_state (window, missing, PGM_PKT_HAVE_PARITY_STATE);
}

/* skb advances the window lead.
 *
 * returns:
 * PGM_RXW_APPENDED - packet advanced window lead, skb consumed.
 * PGM_RXW_MALFORMED - corrupted or invalid packet.
 * PGM_RXW_BOUNDS - packet out of window.
 */

static inline
int
pgm_rxw_append (
	pgm_rxw_t* const		window,
	struct pgm_sk_buff_t* const	skb
	)
{
/* pre-conditions */
	g_assert (window);
	g_assert (skb);
	if (skb->pgm_header->pgm_options & PGM_OPT_PARITY) {
		g_assert (pgm_rxw_tg_sqn (window, skb->sequence) == pgm_rxw_tg_sqn (window, pgm_rxw_lead (window)));
	} else {
		g_assert (skb->sequence == pgm_rxw_lead (window));
	}

	if (pgm_rxw_is_invalid_var_pktlen (window, skb) ||
	    pgm_rxw_is_invalid_payload_op (window, skb))
		return PGM_RXW_MALFORMED;

	if (pgm_rxw_is_full (window))
		_pgm_rxw_remove_trail (window);

/* APDU fragments are already declared lost */
	if (skb->pgm_opt_fragment &&
	    pgm_rxw_is_apdu_lost (window, skb))
	{
		const guint32 lost_sequence	= skb->sequence;
		struct pgm_sk_buff_t* lost_skb	= pgm_alloc_skb (window->max_tpdu);
		lost_skb->tstamp		= pgm_time_now;
		lost_skb->sequence		= lost_sequence;

/* add skb to window */
		const guint32 index_	= lost_skb->sequence % pgm_rxw_max_length (window);
		window->pdata[index_]	= lost_skb;

		_pgm_rxw_state (window, lost_skb, PGM_PKT_LOST_DATA_STATE);
		return PGM_RXW_BOUNDS;
	}

/* add skb to window */
	if (skb->pgm_header->pgm_options & PGM_OPT_PARITY)
	{
		const guint32 index_	= pgm_rxw_lead (window) % pgm_rxw_max_length (window);
		window->pdata[index_]	= skb;
		_pgm_rxw_state (window, skb, PGM_PKT_HAVE_PARITY_STATE);
	}
	else
	{
		const guint32 index_	= skb->sequence % pgm_rxw_max_length (window);
		window->pdata[index_]	= skb;
		_pgm_rxw_state (window, skb, PGM_PKT_HAVE_DATA_STATE);
	}

	return PGM_RXW_APPENDED;
}

/* flush packets but instead of calling on_data append the contiguous data packets
 * to the provided scatter/gather vector.
 *
 * when transmission groups are enabled, packets remain in the windows tagged committed
 * until the transmission group has been completely committed.  this allows the packet
 * data to be used in parity calculations to recover the missing packets.
 *
 * returns -1 on nothing read, returns length of bytes read, 0 is a valid read length.
 *
 * PGM skbuffs will have an increased reference count and must be unreferenced by the 
 * calling application.
 */

gssize
pgm_rxw_readv (
	pgm_rxw_t* const	window,
	pgm_msgv_t**		pmsg,		/* message array, updated as messages appended */
	const guint		msg_len		/* number of items in pmsg */
	)
{
	const pgm_msgv_t* msg_end = *pmsg + msg_len;
	struct pgm_sk_buff_t* skb;
	pgm_rxw_state_t* state;

/* pre-conditions */
	g_assert (window);
	g_assert (pmsg);
	g_assert_cmpuint (msg_len, >, 0);

	g_trace ("readv (window:%p pmsg:%p msg-len:%u)",
		window, pmsg, msg_len);

	window->pgm_sock_err.lost_count = 0;
	gssize bytes_read;

	if (pgm_rxw_incoming_is_empty (window))
		return -1;

	skb = _pgm_rxw_peek (window, window->commit_lead);
	g_assert (skb);

	state = (pgm_rxw_state_t*)&skb->cb;
	switch (state->state) {
	case PGM_PKT_HAVE_DATA_STATE:
		bytes_read = pgm_rxw_incoming_read (window, pmsg, msg_end - *pmsg);
		break;

	case PGM_PKT_LOST_DATA_STATE:
		window->pgm_sock_err.lost_count += _pgm_rxw_remove_trail (window);
/* fall through */
	case PGM_PKT_BACK_OFF_STATE:
	case PGM_PKT_WAIT_NCF_STATE:
	case PGM_PKT_WAIT_DATA_STATE:
	case PGM_PKT_HAVE_PARITY_STATE:
		return -1;

	case PGM_PKT_COMMIT_DATA_STATE:
	case PGM_PKT_ERROR_STATE:
	default:
		g_assert_not_reached();
		break;
	}

	return bytes_read;
}

/* remove lost sequences from the trailing edge of the window.  lost sequence
 * at lead of commit window invalidates all parity-data packets as any 
 * transmission group is now unrecoverable.
 *
 * returns number of sequences purged.
 */

static inline
guint
_pgm_rxw_remove_trail (
	pgm_rxw_t* const	window
	)
{
	struct pgm_sk_buff_t* skb;

/* pre-conditions */
	g_assert (window);
	g_assert (pgm_rxw_commit_is_empty (window));
	g_assert (!pgm_rxw_incoming_is_empty (window));

/* expunge lost transmission group parity-data */
	g_assert_cmpuint (window->trail, ==, window->commit_lead);

	guint dropped = 0;

	do {
		skb = _pgm_rxw_peek (window, window->trail);
		g_assert (skb);
		if (!pgm_rxw_is_apdu_lost (window, skb))
			break;
		_pgm_rxw_unlink (window, skb);
		pgm_free_skb (skb);
		window->commit_lead++;
		window->trail++;
		dropped++;
	} while (!pgm_rxw_incoming_is_empty (window));

/* statistics */
	window->cumulative_losses += dropped;

/* post-conditions */
	g_assert_cmpuint (dropped, >, 0);
	g_assert (pgm_rxw_commit_is_empty (window));

	return dropped;
}

guint
pgm_rxw_remove_trail (
	pgm_rxw_t* const	window
	)
{
	g_trace ("remove_trail (window:%p)", (gpointer)window);
	return _pgm_rxw_remove_trail (window);
}

/* read contiguous APDU-grouped sequences from the incoming window.
 *
 * side effects:
 *
 * 1) increments statics for window messages and bytes read.
 *
 * returns count of bytes read.
 */

static inline
gssize
pgm_rxw_incoming_read (
	pgm_rxw_t* const	window,
	pgm_msgv_t**		pmsg,		/* message array, updated as messages appended */
	guint			msg_len		/* number of items in pmsg */
	)
{
	const pgm_msgv_t* msg_end = *pmsg + msg_len;
	struct pgm_sk_buff_t* skb;

/* pre-conditions */
	g_assert (window);
	g_assert (pmsg);
	g_assert_cmpuint (msg_len, >, 0);
	g_assert (!pgm_rxw_incoming_is_empty (window));

	gssize bytes_read = 0;

	do {
		skb = _pgm_rxw_peek (window, window->commit_lead);
		g_assert (skb);
		if (pgm_rxw_is_apdu_complete (window,
					      skb->pgm_opt_fragment ?
							g_ntohl (skb->of_apdu_first_sqn) :
							skb->sequence,
					      msg_end - *pmsg))
		{
			bytes_read += pgm_rxw_incoming_read_apdu (window, pmsg, msg_end - *pmsg);
		}
		else break;
	} while (!pgm_rxw_incoming_is_empty (window));

	return bytes_read;
}

/* returns TRUE if transmission group is lost.
 *
 * checking is lightly limited to bounds.
 */

static inline
gboolean
pgm_rxw_is_tg_sqn_lost (
	pgm_rxw_t* const	window,
	const guint32		tg_sqn		/* transmission group sequence */
	)
{
/* pre-conditions */
	g_assert (window);
	g_assert_cmpuint (pgm_rxw_pkt_sqn (window, tg_sqn), ==, 0);

	if (pgm_rxw_is_empty (window))
		return TRUE;

	if (pgm_uint32_lt (tg_sqn, window->trail))
		return TRUE;

	return FALSE;
}

/* reconstruct missing sequences in a transmission group using embedded parity data.
 */

static inline
void
pgm_rxw_reconstruct (
	pgm_rxw_t* const	window,
	const guint32		tg_sqn		/* transmission group sequence */
	)
{
	struct pgm_sk_buff_t* skb;
	pgm_rxw_state_t* state;

/* pre-conditions */
	g_assert (window);
	g_assert_cmpuint (pgm_rxw_pkt_sqn (window, tg_sqn), ==, 0);

	skb = _pgm_rxw_peek (window, tg_sqn);
	g_assert (skb);

	const gboolean is_var_pktlen = skb->pgm_header->pgm_options & PGM_OPT_VAR_PKTLEN;
	const gboolean is_op_encoded = skb->pgm_header->pgm_options & PGM_OPT_PRESENT;
	const gsize parity_length = g_ntohs (skb->pgm_header->pgm_tsdu_length);
	struct pgm_sk_buff_t* tg_skbs[ window->rs_n ];
	guint8* tg_data[ window->rs_n ];
	guint8* tg_opts[ window->rs_n ];
	guint32 offsets[ window->rs_k ];
	guint rs_h = 0;

	for (guint32 i = tg_sqn, j = 0; i != (tg_sqn + window->rs_k); i++, j++)
	{
		skb = _pgm_rxw_peek (window, i);
		g_assert (skb);
		state = (pgm_rxw_state_t*)&skb->cb;
		switch (state->state) {
		case PGM_PKT_HAVE_DATA_STATE:
			tg_skbs[ j ] = skb;
			tg_data[ j ] = skb->data;
			tg_opts[ j ] = (gpointer)skb->pgm_opt_fragment;
			offsets[ j ] = j;
			break;

		case PGM_PKT_HAVE_PARITY_STATE:
			tg_skbs[ window->rs_k + rs_h ] = skb;
			tg_data[ window->rs_k + rs_h ] = skb->data;
			tg_opts[ window->rs_k + rs_h ] = (gpointer)skb->pgm_opt_fragment;
			offsets[ j ] = window->rs_k + rs_h;
			++rs_h;
/* fall through and alloc new skb for reconstructed data */
		case PGM_PKT_BACK_OFF_STATE:
		case PGM_PKT_WAIT_NCF_STATE:
		case PGM_PKT_WAIT_DATA_STATE:
		case PGM_PKT_LOST_DATA_STATE:
			skb = pgm_alloc_skb (window->max_tpdu);
			pgm_skb_reserve (skb, sizeof(struct pgm_header) + sizeof(struct pgm_data));
			skb->pgm_header = skb->head;
			skb->pgm_data = (gpointer)( skb->pgm_header + 1 );
			if (is_op_encoded) {
				const guint16 opt_total_length = sizeof(struct pgm_opt_length) +
								 sizeof(struct pgm_opt_header) +
								 sizeof(struct pgm_opt_fragment);
				pgm_skb_reserve (skb, opt_total_length);
				skb->pgm_opt_fragment = (gpointer)( skb->pgm_data + 1 );
				pgm_skb_put (skb, parity_length);
				memset (skb->pgm_opt_fragment, 0, opt_total_length + parity_length);
			} else {
				pgm_skb_put (skb, parity_length);
				memset (skb->data, 0, parity_length);
			}
			tg_skbs[ j ] = skb;
			tg_data[ j ] = skb->data;
			tg_opts[ j ] = (gpointer)skb->pgm_opt_fragment;
			break;

		default: g_assert_not_reached(); break;
		}

		if (!skb->zero_padded) {
			memset (skb->tail, 0, parity_length - skb->len);
			skb->zero_padded = 1;
		}

	}

/* reconstruct payload */
	pgm_rs_decode_parity_appended (window->rs,
				       (void**)(void*)tg_data,
				       offsets,
				       parity_length);

/* reconstruct opt_fragment option */
	if (is_op_encoded)
		pgm_rs_decode_parity_appended (window->rs,
					       (void**)(void*)tg_opts,
					       offsets,
					       sizeof(struct pgm_opt_fragment));

/* swap parity skbs with reconstructed skbs */
	for (guint32 i = 0; i < window->rs_k; i++)
	{
		if (offsets[i] < window->rs_k)
			continue;

		struct pgm_sk_buff_t* repair_skb = tg_skbs[i];

		if (is_var_pktlen)
		{
			const guint16 pktlen = *(guint16*)( (guint8*)repair_skb->tail - sizeof(guint16));
			if (pktlen > parity_length) {
				g_warning ("Invalid encoded variable packet length in reconstructed packet, dropping entire transmission group.");
				pgm_free_skb (repair_skb);
				for (guint32 j = i; j < window->rs_k; j++)
				{
					if (offsets[j] < window->rs_k)
						continue;
					pgm_rxw_lost (window, tg_skbs[offsets[j]]->sequence);
				}
				break;
			}
			const guint padding = parity_length - pktlen;
			repair_skb->len -= padding;
			repair_skb->tail = (guint8*)repair_skb->tail - padding;
		}

#ifdef G_DISABLE_ASSERT
		pgm_rxw_insert (window, repair_skb);
#else
		g_assert_cmpint (pgm_rxw_insert (window, repair_skb), ==, PGM_RXW_INSERTED);
#endif
	}
}

/* check every TPDU in an APDU and verify that the data has arrived
 * and is available to commit to the application.
 *
 * if APDU sits in a transmission group that can be reconstructed use parity
 * data then the entire group will be decoded and any missing data packets
 * replaced by the recovery calculation.
 *
 * packets with single fragment fragment headers must be normalised as regular
 * packets before calling.
 *
 * returns FALSE if APDU is incomplete or longer than max_len sequences.
 */

static inline
gboolean
pgm_rxw_is_apdu_complete (
	pgm_rxw_t* const	window,
	const guint32		first_sequence,
	const guint		max_len			/* of TPDUs */
	)
{
	struct pgm_sk_buff_t* skb;
	pgm_rxw_state_t* state;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);
	g_assert_cmpuint (max_len, >, 0);

	skb = _pgm_rxw_peek (window, first_sequence);

	const gsize apdu_size = g_ntohl (skb->of_apdu_len);
	const guint32 tg_sqn = pgm_rxw_tg_sqn (window, first_sequence);
	guint32 sequence = first_sequence;
	guint contiguous_tpdus = 0;
	gsize contiguous_size = 0;
	gboolean check_parity = FALSE;

	g_assert_cmpuint (apdu_size, >, skb->len);

/* protocol sanity check: maximum length */
	if (g_ntohl (skb->of_apdu_len) > pgm_rxw_max_length (window)) {
		_pgm_rxw_lost (window, first_sequence);
		return FALSE;
	}

	do {
		state = (pgm_rxw_state_t*)&skb->cb;

		if (!check_parity &&
		    PGM_PKT_HAVE_DATA_STATE != state->state)
		{
			if (window->is_fec_available &&
			    !pgm_rxw_is_tg_sqn_lost (window, tg_sqn) )
			{
				check_parity = TRUE;
/* pre-seed committed sequence count */
				if (pgm_uint32_lte (tg_sqn, window->commit_lead))
					contiguous_tpdus += window->commit_lead - tg_sqn;
			}
			else
				return FALSE;
		}

		if (check_parity)
		{
			if (PGM_PKT_HAVE_DATA_STATE == state->state ||
			    PGM_PKT_HAVE_PARITY_STATE == state->state)
				++contiguous_tpdus;

/* have sufficient been received for reconstruction */
			if (contiguous_tpdus >= window->tg_size) {
				pgm_rxw_reconstruct (window, tg_sqn);
				return pgm_rxw_is_apdu_complete (window, first_sequence, max_len);
			}
		}
		else
		{
/* single packet APDU, already complete */
			if (PGM_PKT_HAVE_DATA_STATE == state->state &&
			    !skb->pgm_opt_fragment)
				return TRUE;

/* protocol sanity check: matching first sequence reference */
			if (g_ntohl (skb->of_apdu_first_sqn) != first_sequence) {
				_pgm_rxw_lost (window, first_sequence);
				return FALSE;
			}

/* protocol sanity check: matching apdu length */
			if (g_ntohl (skb->of_apdu_len) != apdu_size) {
				_pgm_rxw_lost (window, first_sequence);
				return FALSE;
			}

/* protocol sanity check: maximum number of fragments per apdu */
			if (++contiguous_tpdus > PGM_MAX_FRAGMENTS) {
				_pgm_rxw_lost (window, first_sequence);
				return FALSE;
			}

			if (contiguous_tpdus > max_len)
				return FALSE;

			contiguous_size += skb->len;
			if (apdu_size == contiguous_size)
				return TRUE;
			if (apdu_size < contiguous_size) {
				_pgm_rxw_lost (window, first_sequence);
				return FALSE;
			}
		}

		skb = _pgm_rxw_peek (window, ++sequence);
	} while (skb);

/* pending */
	return FALSE;
}

/* read one APDU consisting of one or more TPDUs.  target array is guaranteed
 * to be big enough to store complete APDU.
 */

static inline
gssize
pgm_rxw_incoming_read_apdu (
	pgm_rxw_t* const	window,
	pgm_msgv_t**		pmsg,		/* message array, updated as messages appended */
	guint			msg_len		/* number of items in pmsg */
	)
{
	struct pgm_sk_buff_t *skb, **pskb;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);
	g_assert_cmpuint (msg_len, >, 0);

	skb = _pgm_rxw_peek (window, window->commit_lead);
	gsize contiguous_len = 0;
	const gsize apdu_len = skb->pgm_opt_fragment ? g_ntohl (skb->of_apdu_len) : skb->len;
	g_assert_cmpuint (apdu_len, >=, skb->len);
	pskb = (*pmsg)->msgv_skb;
	do {
		_pgm_rxw_state (window, skb, PGM_PKT_COMMIT_DATA_STATE);
		*pskb++ = skb;
		contiguous_len += skb->len;
		if (pgm_rxw_is_last_of_tg_sqn (window, window->commit_lead))
			pgm_rxw_remove_tg_sqn (window, pgm_rxw_tg_sqn (window, window->commit_lead));
		window->commit_lead++;
		if (apdu_len == contiguous_len)
			break;
		skb = _pgm_rxw_peek (window, window->commit_lead);
	} while (apdu_len > contiguous_len);

	(*pmsg)->msgv_len = contiguous_len;
	(*pmsg)++;

/* post-conditions */
	g_assert (!pgm_rxw_commit_is_empty (window));

	return contiguous_len;
}

/* returns transmission group sequence (TG_SQN) from sequence (SQN).
 */

static inline
guint32
pgm_rxw_tg_sqn (
	pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
/* pre-conditions */
	g_assert (window);

	const guint32 tg_sqn_mask = 0xffffffff << window->tg_sqn_shift;
	return sequence & tg_sqn_mask;
}

/* returns packet number (PKT_SQN) from sequence (SQN).
 */

static inline
guint32
pgm_rxw_pkt_sqn (
	pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
/* pre-conditions */
	g_assert (window);

	const guint32 tg_sqn_mask = 0xffffffff << window->tg_sqn_shift;
	return sequence & ~tg_sqn_mask;
}

/* returns TRUE when the sequence is the first of a transmission group.
 */

static inline
gboolean
pgm_rxw_is_first_of_tg_sqn (
	pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
/* pre-conditions */
	g_assert (window);

	return pgm_rxw_pkt_sqn (window, sequence) == 0;
}

/* returns TRUE when the sequence is the last of a transmission group
 */

static inline
gboolean
pgm_rxw_is_last_of_tg_sqn (
	pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
/* pre-conditions */
	g_assert (window);

	return pgm_rxw_pkt_sqn (window, sequence) == window->tg_size - 1;
}

static inline
void
pgm_rxw_remove_tg_sqn (
	pgm_rxw_t* const	window,
	const guint32		tg_sqn		/* transmission group sequence */
	)
{
/* pre-conditions */
	g_assert (window);
	g_assert_cmpuint (pgm_rxw_pkt_sqn (window, tg_sqn), ==, 0);

	while (!pgm_rxw_commit_is_empty (window) &&
		pgm_rxw_tg_sqn (window, window->trail) == tg_sqn)
	{
		_pgm_rxw_remove_trail (window);
	}
}

/* update PGM skbuff to new FSM state
 */

static inline
void
_pgm_rxw_state (
	pgm_rxw_t*		window,
	struct pgm_sk_buff_t*	skb,
	pgm_pkt_state_e		new_state
	)
{
	pgm_rxw_state_t* state;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);

	state = (pgm_rxw_state_t*)&skb->cb;

	switch (new_state) {
	case PGM_PKT_BACK_OFF_STATE:
		g_queue_push_head_link (&window->backoff_queue, (GList*)skb);
		break;

	case PGM_PKT_WAIT_NCF_STATE:
		g_queue_push_head_link (&window->wait_ncf_queue, (GList*)skb);
		break;

	case PGM_PKT_WAIT_DATA_STATE:
		g_queue_push_head_link (&window->wait_data_queue, (GList*)skb);
		break;

	case PGM_PKT_HAVE_DATA_STATE:
		window->fragment_count++;
		g_assert_cmpuint (window->fragment_count, <=, pgm_rxw_length (window));
		break;

	case PGM_PKT_HAVE_PARITY_STATE:
		window->parity_count++;
		g_assert_cmpuint (window->parity_count, <=, pgm_rxw_length (window));
		break;

	case PGM_PKT_COMMIT_DATA_STATE:
		window->committed_count++;
		g_assert_cmpuint (window->committed_count, <=, pgm_rxw_length (window));
		break;

	case PGM_PKT_LOST_DATA_STATE:
		window->lost_count++;
		window->cumulative_losses++;
		window->is_waiting = TRUE;
		g_assert_cmpuint (window->lost_count, <=, pgm_rxw_length (window));
		break;

	case PGM_PKT_ERROR_STATE:
		break;

	default: g_assert_not_reached(); break;
	}

	state->state = new_state;
}

void
pgm_rxw_state (
	pgm_rxw_t*		window,
	struct pgm_sk_buff_t*	skb,
	pgm_pkt_state_e		new_state
	)
{
	g_trace ("state (window:%p skb:%p new_state:%s)",
		(gpointer)window, (gpointer)skb, pgm_pkt_state_string (new_state));
	_pgm_rxw_state (window, skb, new_state);
}

static inline
void
_pgm_rxw_unlink (
	pgm_rxw_t*		window,
	struct pgm_sk_buff_t*	skb
	)
{
	GQueue* queue;

/* pre-conditions */
	g_assert (window);
	g_assert (skb);

	pgm_rxw_state_t* state = (pgm_rxw_state_t*)&skb->cb;

	switch (state->state) {
	case PGM_PKT_BACK_OFF_STATE:
		g_assert (!g_queue_is_empty (&window->backoff_queue));
		queue = &window->backoff_queue;
		goto unlink_queue;

	case PGM_PKT_WAIT_NCF_STATE:
		g_assert (!g_queue_is_empty (&window->wait_ncf_queue));
		queue = &window->wait_ncf_queue;
		goto unlink_queue;

	case PGM_PKT_WAIT_DATA_STATE:
		g_assert (!g_queue_is_empty (&window->wait_data_queue));
		queue = &window->wait_data_queue;
unlink_queue:
		g_queue_unlink (queue, (GList*)skb);
		break;

	case PGM_PKT_HAVE_DATA_STATE:
		g_assert_cmpuint (window->fragment_count, >, 0);
		window->fragment_count--;
		break;

	case PGM_PKT_HAVE_PARITY_STATE:
		g_assert_cmpuint (window->parity_count, >, 0);
		window->parity_count--;
		break;

	case PGM_PKT_COMMIT_DATA_STATE:
		g_assert_cmpuint (window->committed_count, >, 0);
		window->committed_count--;
		break;

	case PGM_PKT_LOST_DATA_STATE:
		g_assert_cmpuint (window->lost_count, >, 0);
		window->lost_count--;
		break;

	case PGM_PKT_ERROR_STATE:
		break;

	default: g_assert_not_reached(); break;
	}

	state->state = PGM_PKT_ERROR_STATE;
	g_assert (((GList*)skb)->next == NULL);
	g_assert (((GList*)skb)->prev == NULL);
}

void
pgm_rxw_unlink (
	pgm_rxw_t*		window,
	struct pgm_sk_buff_t*	skb
	)
{
	g_trace ("unlink (window:%p skb:%p)", (gpointer)window, (gpointer)skb);
	_pgm_rxw_unlink (window, skb);
}

/* returns the pointer at the given index of the window.
 */

struct pgm_sk_buff_t*
pgm_rxw_peek (
	pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
	g_trace ("peek (window:%p sequence:%" G_GUINT32_FORMAT ")", (gpointer)window, sequence);
	return _pgm_rxw_peek (window, sequence);
}

/* mark an existing sequence lost due to failed recovery.
 */

static inline
void
_pgm_rxw_lost (
	pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
	struct pgm_sk_buff_t* skb;
	pgm_rxw_state_t* state;

/* pre-conditions */
	g_assert (window);
	g_assert (!pgm_rxw_is_empty (window));

	skb = _pgm_rxw_peek (window, sequence);
	g_assert (skb);

	state = (pgm_rxw_state_t*)&skb->cb;
	g_assert( state->state == PGM_PKT_BACK_OFF_STATE ||
		  state->state == PGM_PKT_WAIT_NCF_STATE ||
		  state->state == PGM_PKT_WAIT_DATA_STATE  );

	_pgm_rxw_state (window, skb, PGM_PKT_LOST_DATA_STATE);
}

void
pgm_rxw_lost (
	pgm_rxw_t* const	window,
	const guint32		sequence
	)
{
	g_trace ("lost (window:%p sequence:%" G_GUINT32_FORMAT ")",
		 (gpointer)window, sequence);
	_pgm_rxw_lost (window, sequence);
}

/* received a uni/multicast ncf, search for a matching nak & tag or extend window if
 * beyond lead
 *
 * returns.
 */

int
pgm_rxw_confirm (
	pgm_rxw_t* const	window,
	const guint32		sequence,
	const pgm_time_t	nak_rdata_expiry,		/* pre-calculated expiry times */
	const pgm_time_t	nak_rb_expiry
	)
{
/* pre-conditions */
	g_assert (window);

	g_trace ("confirm (window:%p sequence:%" G_GUINT32_FORMAT " nak_rdata_expiry:%" PGM_TIME_FORMAT " nak_rb_expiry:%" PGM_TIME_FORMAT ")",
		(gpointer)window, sequence, nak_rdata_expiry, nak_rb_expiry);

/* NCFs do not define the transmit window */
	if (!window->is_defined)
		return 0;

/* sequence already committed */
	if (pgm_uint32_lte (sequence, window->commit_lead))
		return 0;

	if (pgm_uint32_lte (sequence, window->lead))
		return pgm_rxw_recovery_update (window, sequence, nak_rdata_expiry);

	if (sequence == window->lead) 
		return pgm_rxw_recovery_append (window, nak_rdata_expiry);
	else {
		pgm_rxw_add_placeholder_range (window, sequence, nak_rb_expiry);
		return pgm_rxw_recovery_append (window, nak_rdata_expiry);
	}
}

/* update an incoming sequence with state transition to WAIT-DATA.
 */

static inline
int
pgm_rxw_recovery_update (
	pgm_rxw_t* const	window,
	const guint32		sequence,
	const pgm_time_t	nak_rdata_expiry		/* pre-calculated expiry times */
	)
{
/* pre-conditions */
	g_assert (window);

/* fetch skb from window and bump expiration times */
	struct pgm_sk_buff_t* skb = _pgm_rxw_peek (window, sequence);
	g_assert (skb);
	pgm_rxw_state_t* state = (pgm_rxw_state_t*)&skb->cb;
	switch (state->state) {
	case PGM_PKT_BACK_OFF_STATE:
	case PGM_PKT_WAIT_NCF_STATE:
		_pgm_rxw_unlink (window, skb);
		pgm_rxw_state (window, skb, PGM_PKT_WAIT_DATA_STATE);

/* fall through */
	case PGM_PKT_WAIT_DATA_STATE:
		state->nak_rdata_expiry = nak_rdata_expiry;
		return PGM_RXW_UPDATED;

	case PGM_PKT_HAVE_DATA_STATE:
	case PGM_PKT_HAVE_PARITY_STATE:
	case PGM_PKT_COMMIT_DATA_STATE:
	case PGM_PKT_LOST_DATA_STATE:
		break;

	default: g_assert_not_reached(); break;
	}

	return PGM_RXW_DUPLICATE;
}

/* append an skb to the incoming window with WAIT-DATA state.
 */

static inline
int
pgm_rxw_recovery_append (
	pgm_rxw_t* const	window,
	const pgm_time_t	nak_rdata_expiry		/* pre-calculated expiry times */
	)
{
	struct pgm_sk_buff_t* skb;

/* pre-conditions */
	g_assert (window);

	if (pgm_rxw_is_full (window))
		_pgm_rxw_remove_trail (window);

	skb			= pgm_alloc_skb (window->max_tpdu);
	pgm_rxw_state_t* state	= (pgm_rxw_state_t*)&skb->cb;
	skb->tstamp		= pgm_time_now;
	skb->sequence		= window->lead;
	state->nak_rdata_expiry	= nak_rdata_expiry;

	const guint32 index_	= pgm_rxw_lead (window) % pgm_rxw_max_length (window);
	window->pdata[index_]	= skb;
	_pgm_rxw_state (window, skb, PGM_PKT_WAIT_DATA_STATE);

	return PGM_RXW_APPENDED;
}

/* state string helper
 */

const char*
pgm_pkt_state_string (
	pgm_pkt_state_e		state
	)
{
	const char* c;

	switch (state) {
	case PGM_PKT_BACK_OFF_STATE:	c = "PGM_PKT_BACK_OFF_STATE"; break;
	case PGM_PKT_WAIT_NCF_STATE:	c = "PGM_PKT_WAIT_NCF_STATE"; break;
	case PGM_PKT_WAIT_DATA_STATE:	c = "PGM_PKT_WAIT_DATA_STATE"; break;
	case PGM_PKT_HAVE_DATA_STATE:	c = "PGM_PKT_HAVE_DATA_STATE"; break;
	case PGM_PKT_HAVE_PARITY_STATE:	c = "PGM_PKT_HAVE_PARITY_STATE"; break;
	case PGM_PKT_COMMIT_DATA_STATE: c = "PGM_PKT_COMMIT_DATA_STATE"; break;
	case PGM_PKT_LOST_DATA_STATE:	c = "PGM_PKT_LOST_DATA_STATE"; break;
	case PGM_PKT_ERROR_STATE:	c = "PGM_PKT_ERROR_STATE"; break;
	default: c = "(unknown)"; break;
	}

	return c;
}

const char*
pgm_rxw_returns_string (
	pgm_rxw_returns_e	retval
	)
{
	const char* c;

	switch (retval) {
	case PGM_RXW_OK:			c = "PGM_RXW_OK"; break;
	case PGM_RXW_INSERTED:			c = "PGM_RXW_INSERTED"; break;
	case PGM_RXW_APPENDED:			c = "PGM_RXW_APPENDED"; break;
	case PGM_RXW_UPDATED:			c = "PGM_RXW_UPDATED"; break;
	case PGM_RXW_MISSING:			c = "PGM_RXW_MISSING"; break;
	case PGM_RXW_DUPLICATE:			c = "PGM_RXW_DUPLICATE"; break;
	case PGM_RXW_MALFORMED:			c = "PGM_RXW_MALFORMED"; break;
	case PGM_RXW_BOUNDS:			c = "PGM_RXW_BOUNDS"; break;
	case PGM_RXW_SLOW_CONSUMER:		c = "PGM_RXW_SLOW_CONSUMER"; break;
	case PGM_RXW_UNKNOWN:			c = "PGM_RXW_UNKNOWN"; break;
	default: c = "(unknown)"; break;
	}

	return c;
}

/* eof */