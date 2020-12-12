/**
 * @file feed_parser.c  parsing of different feed formats
 *
 * Copyright (C) 2008-2020 Lars Windolf <lars.windolf@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>

#include "common.h"
#include "debug.h"
#include "html.h"
#include "metadata.h"
#include "xml.h"
#include "parsers/atom10.h"
#include "parsers/html5_feed.h"
#include "parsers/ldjson_feed.h"
#include "parsers/rss_channel.h"

static GSList *feedHandlers = NULL;	/**< list of available parser implementations */

struct feed_type {
	gint id_num;
	gchar *id_str;
};

static GSList *
feed_parsers_get_list (void)
{
	if (feedHandlers)
		return feedHandlers;

	feedHandlers = g_slist_append (feedHandlers, rss_init_feed_handler ());
	feedHandlers = g_slist_append (feedHandlers, atom10_init_feed_handler ());

	/* Order is important ! */
	feedHandlers = g_slist_append (feedHandlers, ldjson_init_feed_handler ());
	feedHandlers = g_slist_append (feedHandlers, html5_init_feed_handler ());

	return feedHandlers;
}

const gchar *
feed_type_fhp_to_str (feedHandlerPtr fhp)
{
	if (!fhp)
		return NULL;
	return fhp->typeStr;
}

feedHandlerPtr
feed_type_str_to_fhp (const gchar *str)
{
	GSList *iter;
	feedHandlerPtr fhp = NULL;

	if (!str)
		return NULL;

	for (iter = feed_parsers_get_list (); iter != NULL; iter = iter->next) {
		fhp = (feedHandlerPtr)iter->data;
		if (!strcmp(str, fhp->typeStr))
			return fhp;
	}

	return NULL;
}

feedParserCtxtPtr
feed_create_parser_ctxt (void)
{
	feedParserCtxtPtr ctxt;

	ctxt = g_new0 (struct feedParserCtxt, 1);
	ctxt->tmpdata = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
	return ctxt;
}

void
feed_free_parser_ctxt (feedParserCtxtPtr ctxt)
{
	if (ctxt) {
		/* Don't free the itemset! */
		g_hash_table_destroy (ctxt->tmpdata);
		g_free (ctxt->title);
		g_free (ctxt);
	}
}

/**
 * This function tries to find a feed link for a given HTTP URI. It
 * tries to download it. If it finds a valid feed source it parses
 * this source instead into the given feed parsing context. It also
 * replaces the HTTP URI with the found feed source.
 *
 * Mutates ctxt->failed and ctxt->feed->parseErrors
 *
 * Returns: TRUE on success
 */
static gboolean
feed_parser_auto_discover (feedParserCtxtPtr ctxt)
{
	gchar	*source = NULL;
	GSList	*links;

	if (ctxt->feed->parseErrors)
		g_string_truncate (ctxt->feed->parseErrors, 0);
	else
		ctxt->feed->parseErrors = g_string_new(NULL);

	debug1 (DEBUG_UPDATE, "Starting feed auto discovery (%s)", subscription_get_source (ctxt->subscription));

	links = html_auto_discover_feed (ctxt->data, subscription_get_source (ctxt->subscription));
	if (links)
		source = links->data;	// FIXME: let user choose feed!

	/* FIXME: we only need the !g_str_equal as a workaround after a 404 */
	if (source && !g_str_equal (source, subscription_get_source (ctxt->subscription))) {
		debug1 (DEBUG_UPDATE, "Discovered link: %s", source);
		ctxt->failed = FALSE;
		subscription_set_source (ctxt->subscription, source);

		/* The feed that was processed wasn't the correct one, we need to redownload it.
		 * Cancel the update in case there's one in progress */
		subscription_cancel_update (ctxt->subscription);
		subscription_update (ctxt->subscription, FEED_REQ_RESET_TITLE);
		g_free (source);
		return TRUE;
	}

	debug0 (DEBUG_UPDATE, "No feed link found!");
	g_string_append (ctxt->feed->parseErrors, _("The URL you want Liferea to subscribe to points to a webpage and the auto discovery found no feeds on this page. Maybe this webpage just does not support feed auto discovery."));
	return FALSE;
}

static void
feed_parser_ctxt_cleanup (feedParserCtxtPtr ctxt)
{
	/* free old temp. parsing data, don't free right after parsing because
	   it can be used until the last feed request is finished */
	g_hash_table_destroy (ctxt->tmpdata);
	ctxt->tmpdata = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);

	/* we always drop old metadata */
	// FIXME: this is bad, doesn't belong here at all
	metadata_list_free (ctxt->subscription->metadata);

	ctxt->subscription->metadata = NULL;
	ctxt->failed = FALSE;
}

/**
 * General feed source parsing function. Parses the passed feed source
 * and tries to determine the source type. If all feed handlers fail
 * tries to do HTML5 feed extraction. If this also fails starts feed
 * link auto-discovery.
 *
 * @param ctxt		feed parsing context
 *
 * @returns FALSE if auto discovery is indicated,
 *          TRUE if feed type was recognized and parsing was successful
 */
gboolean
feed_parse (feedParserCtxtPtr ctxt)
{
	xmlNodePtr	xmlNode = NULL, htmlNode = NULL;
	xmlDocPtr	xmlDoc, htmlDoc;
	gboolean	success = FALSE;
	gboolean	htmlParsing = FALSE;

	debug_enter ("feed_parse");

	g_assert (NULL == ctxt->items);

	ctxt->failed = TRUE;	/* reset on success ... */

	if (ctxt->feed->parseErrors)
		g_string_truncate (ctxt->feed->parseErrors, 0);
	else
		ctxt->feed->parseErrors = g_string_new (NULL);

	/* Prepare two documents, one parse as XML and one as XHTML.
	   Depending on the feed parser being an HTML parser the one
	   or the other is used. */

	/* 1.) try to parse downloaded data as XML */
	do {
		if (NULL == (xmlDoc = xml_parse_feed (ctxt))) {
			ctxt->feed->error = FEED_FETCH_ERROR_XML;
			break;
		}

		if (NULL == (xmlNode = xmlDocGetRootElement (xmlDoc))) {
			ctxt->feed->error = FEED_FETCH_ERROR_XML;
			g_string_append (ctxt->feed->parseErrors, _("Empty document!"));
			break;
		}

		while (xmlNode && xmlIsBlankNode (xmlNode)) {
			xmlNode = xmlNode->next;
		}

		if (!xmlNode->name) {
			g_string_append (ctxt->feed->parseErrors, _("Invalid XML!"));
			break;
		}
	} while (0);

	/* 2.) Prepare data as XHTML */
	do {
		if (NULL == (htmlDoc = xhtml_parse (ctxt->data, ctxt->dataLength)))
			break;

		if (NULL == (htmlNode = xmlDocGetRootElement (htmlDoc))) {
			//g_string_append (ctxt->feed->parseErrors, _("Empty document!"));
			break;
		}
	} while (0);

	/* determine the syndication format and start parser with either XML or XHTML doc */
	GSList *handlerIter = feed_parsers_get_list ();
	while (handlerIter) {
		feedHandlerPtr handler = (feedHandlerPtr)(handlerIter->data);
		xmlNodePtr node = handler->html?htmlNode:xmlNode;

		if (node && handler && handler->checkFormat && (*(handler->checkFormat))(node->doc, node)) {
			ctxt->feed->fhp = handler;
			feed_parser_ctxt_cleanup (ctxt);
			(*(handler->feedParser)) (ctxt, handler->html?htmlNode:xmlNode);
			break;
		}
		handlerIter = handlerIter->next;
	}

	/* 2.) None of the feed formats did work, chance is high that we are
	       working on a HTML documents. Let's look for feed links inside it! */
	if (ctxt->failed)
		feed_parser_auto_discover (ctxt);

	if (htmlDoc)
		xmlFreeDoc (htmlDoc);
	if (xmlDoc)
		xmlFreeDoc (xmlDoc);

	/* 4.) We give up and inform the user */
	if (ctxt->failed) {
		ctxt->feed->error = FEED_FETCH_ERROR_DISCOVER;
		// FIXME: revise this

		/* test if we have a HTML page */
		if ((strstr (ctxt->data, "<html>") || strstr (ctxt->data, "<HTML>") ||
		     strstr (ctxt->data, "<html ") || strstr (ctxt->data, "<HTML "))) {
			debug0 (DEBUG_UPDATE, "HTML document detected!");
			g_string_append (ctxt->feed->parseErrors, _("Source points to HTML document."));
		} else {
			debug0 (DEBUG_UPDATE, "neither a known feed type nor a HTML document!");
			g_string_append (ctxt->feed->parseErrors, _("Could not determine the feed type."));
		}
	} else {
		debug1 (DEBUG_UPDATE, "discovered feed format: %s", feed_type_fhp_to_str (ctxt->feed->fhp));
		success = TRUE;
		ctxt->feed->error = FEED_FETCH_ERROR_NONE;
	}

	debug_exit ("feed_parse");

	return success;
}
