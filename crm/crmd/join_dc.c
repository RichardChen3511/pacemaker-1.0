/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <portability.h>

#include <heartbeat.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <crmd_fsa.h>
#include <crmd_messages.h>

#include <crm/dmalloc_wrapper.h>

int num_join_invites = 0;
GHashTable *join_offers         = NULL;
GHashTable *join_requests       = NULL;
GHashTable *confirmed_nodes     = NULL;
xmlNodePtr our_generation       = NULL;
const char *max_generation_from = NULL;

void initialize_join(gboolean before);
void finalize_join_for(gpointer key, gpointer value, gpointer user_data);
void join_send_offer(gpointer key, gpointer value, gpointer user_data);

/*	 A_DC_JOIN_OFFER_ALL	*/
enum crmd_fsa_input
do_dc_join_offer_all(long long action,
		     enum crmd_fsa_cause cause,
		     enum crmd_fsa_state cur_state,
		     enum crmd_fsa_input current_input,
		     fsa_data_t *msg_data)
{
	/* reset everyones status back to down or in_ccm in the CIB */
	xmlNodePtr update     = NULL;
	xmlNodePtr cib_copy   = get_cib_copy(fsa_cib_conn);
	xmlNodePtr tmp1       = get_object_root(XML_CIB_TAG_STATUS, cib_copy);
	xmlNodePtr tmp2       = NULL;

	initialize_join(TRUE);
	
	/* catch any nodes that are active in the CIB but not in the CCM list*/
	xml_child_iter(
		tmp1, node_entry, XML_CIB_TAG_STATE,

		const char *node_id = xmlGetProp(node_entry, XML_ATTR_UNAME);
		gpointer a_node = g_hash_table_lookup(
			fsa_membership_copy->members, node_id);

		if(a_node != NULL || (safe_str_eq(fsa_our_uname, node_id))) {
			/* handled by do_update_cib_node() */
			continue;
		}

		tmp2 = create_node_state(
			node_id, node_id, NULL,
			XML_BOOLEAN_NO, NULL, CRMD_JOINSTATE_PENDING, NULL);

		if(update == NULL) {
			update = tmp2;
		} else {
			update = xmlAddSibling(update, tmp2);
		}
		);

	/* now process the CCM data */
	free_xml(do_update_cib_nodes(update, TRUE));
	free_xml(cib_copy);

#if 0
	/* Avoid ordered message delays caused when the CRMd proc
	 * isnt running yet (ie. send as a broadcast msg which are never
	 * sent ordered.
	 */
	send_request(NULL, NULL, CRM_OP_WELCOME,
		     NULL, CRM_SYSTEM_CRMD, NULL);	
#else

	crm_debug("Offering membership to %d clients",
		  fsa_membership_copy->members_size);
	
	g_hash_table_foreach(fsa_membership_copy->members,
			     join_send_offer, NULL);
	
#endif
/* No point hanging around in S_INTEGRATION if we're the only ones here! */
	if(g_hash_table_size(join_requests)
	   >= fsa_membership_copy->members_size) {
		crm_info("Not expecting any join acks");
		register_fsa_input(C_FSA_INTERNAL, I_INTEGRATED, NULL);
		return I_NULL;
	}

	/* dont waste time by invoking the pe yet; */
	crm_debug("Still waiting on %d outstanding join acks",
		  fsa_membership_copy->members_size
		  - g_hash_table_size(join_requests));

	return I_NULL;
}

/*	 A_DC_JOIN_OFFER_ONE	*/
enum crmd_fsa_input
do_dc_join_offer_one(long long action,
		     enum crmd_fsa_cause cause,
		     enum crmd_fsa_state cur_state,
		     enum crmd_fsa_input current_input,
		     fsa_data_t *msg_data)
{
	gpointer a_node = NULL;
	xmlNodePtr welcome = NULL;
	const char *join_to = NULL;

	if(msg_data->data == NULL) {
		crm_err("Attempt to send welcome message "
			"without a message to reply to!");
		return I_NULL;
	}

	welcome = (xmlNodePtr)msg_data->data;
	
	join_to = xmlGetProp(welcome, XML_ATTR_HOSTFROM);

	a_node = g_hash_table_lookup(join_requests, join_to);
	if(a_node != NULL
	   && (cur_state == S_INTEGRATION || cur_state == S_FINALIZE_JOIN)) {
		/* note: it _is_ possible that a node will have been
		 *  sick or starting up when the original offer was made.
		 *  however, it will either re-announce itself in due course
		 *  _or_ we can re-store the original offer on the client.
		 */
		crm_warn("Already offered membership to %s... discarding",
			 join_to);

		/* Make sure we end up in the correct state again */
		if(g_hash_table_size(join_requests)
		   >= fsa_membership_copy->members_size) {

			crm_info("False alarm, returning to %s",
				 fsa_state2string(S_FINALIZE_JOIN));
			
			register_fsa_input(C_FSA_INTERNAL, I_INTEGRATED, NULL);
			return I_NULL;
		}
			
		crm_debug("Still waiting on %d outstanding join acks",
			  fsa_membership_copy->members_size
			  - g_hash_table_size(join_requests));
		
	} else {
		oc_node_t member;

		crm_debug("Processing annouce request from %s in state %s",
			  join_to, fsa_state2string(cur_state));
		
		member.node_uname = crm_strdup(join_to);
		join_send_offer(NULL, &member, NULL);
		crm_free(member.node_uname);

		/* this was a genuine join request, cancel any existing
		 * transition and invoke the PE
		 */
		register_fsa_input_w_actions(
			msg_data->fsa_cause, I_NULL, NULL, A_TE_CANCEL);
	}
	
	return I_NULL;
}

/*	 A_DC_JOIN_PROCESS_REQ	*/
enum crmd_fsa_input
do_dc_join_req(long long action,
	       enum crmd_fsa_cause cause,
	       enum crmd_fsa_state cur_state,
	       enum crmd_fsa_input current_input,
	       fsa_data_t *msg_data)
{
	xmlNodePtr generation;
	xmlNodePtr join_ack = (xmlNodePtr)msg_data->data;
	const char *ack_nack = CRMD_JOINSTATE_MEMBER;

	gboolean is_a_member  = FALSE;
	const char *join_from = xmlGetProp(join_ack, XML_ATTR_HOSTFROM);
	const char *ref       = xmlGetProp(join_ack, XML_ATTR_REFERENCE);

	xmlNodePtr options = find_xml_node(join_ack, XML_TAG_OPTIONS);
	const char *op	   = xmlGetProp(options, XML_ATTR_OP);

	gpointer join_node =
		g_hash_table_lookup(fsa_membership_copy->members, join_from);

	if(safe_str_neq(op, CRM_OP_WELCOME)) {
		crm_warn("Ignoring op=%s message", op);
		return I_NULL;
	}

	crm_debug("Processing req from %s", join_from);
	
	if(join_node != NULL) {
		is_a_member = TRUE;
	}
	
	generation = find_xml_node(join_ack, XML_CIB_TAG_GENERATION_TUPPLE);
	if(cib_compare_generation(our_generation, generation) < 0) {
		clear_bit_inplace(fsa_input_register, R_HAVE_CIB);
		crm_debug("%s has a better generation number than us",
			  join_from);
		crm_xml_debug(our_generation, "Our generation");
		crm_xml_debug(generation, "Their generation");
		max_generation_from = join_from;
	}
	
	crm_debug("Welcoming node %s after ACK (ref %s)", join_from, ref);
	
	if(is_a_member == FALSE) {
		/* nack them now so they are not counted towards the
		 * expected responses
		 */
		char *local_from = crm_strdup(join_from);
		char *local_down = crm_strdup(CRMD_JOINSTATE_DOWN);

		crm_err("Node %s is not known to us (ref %s)", join_from, ref);
		finalize_join_for(local_from, local_down, NULL);
		crm_free(local_from);
		crm_free(local_down);
		
		return I_FAIL;

	} else if(/* some reason */ 0) {
		/* NACK this client */
		ack_nack = CRMD_JOINSTATE_DOWN;
	}
	
	/* add them to our list of CRMD_STATE_ACTIVE nodes
	   TODO: check its not already there
	*/
	g_hash_table_insert(
		join_requests, crm_strdup(join_from), crm_strdup(ack_nack));

	if(g_hash_table_size(join_requests)
	   >= fsa_membership_copy->members_size) {
		crm_info("That was the last outstanding join ack");
		register_fsa_input(C_FSA_INTERNAL, I_INTEGRATED, NULL);
		return I_NULL;
	}

	/* dont waste time by invoking the PE yet; */
	crm_debug("Still waiting on %d (of %d) outstanding join acks",
		  fsa_membership_copy->members_size
		  - g_hash_table_size(join_requests),
		  fsa_membership_copy->members_size);
	
	return I_NULL;
}



/*	A_DC_JOIN_FINALIZE	*/
enum crmd_fsa_input
do_dc_join_finalize(long long action,
		    enum crmd_fsa_cause cause,
		    enum crmd_fsa_state cur_state,
		    enum crmd_fsa_input current_input,
		    fsa_data_t *msg_data)
{
	if(max_generation_from == NULL) {
		crm_warn("There is no CIB to get..."
			 " how did R_HAVE_CIB get unset?");
		set_bit_inplace(fsa_input_register, R_HAVE_CIB);
	}
	
	if(! is_set(fsa_input_register, R_HAVE_CIB)) {
		/* ask for the agreed best CIB */
		enum cib_errors rc = cib_ok;
		crm_info("Asking %s for its copy of the CIB",
			 crm_str(max_generation_from));

		rc = fsa_cib_conn->cmds->sync_from(
			fsa_cib_conn, max_generation_from, NULL, cib_sync_call);
		if(rc != cib_ok) {
			return I_FAIL;
		}
	}

	fsa_cib_conn->cmds->bump_epoch(
		fsa_cib_conn, cib_scope_local|cib_sync_call);
	fsa_cib_conn->cmds->sync(fsa_cib_conn, NULL, cib_sync_call);
	
	num_join_invites = 0;
	crm_debug("Notifying %d clients of join results",
		  g_hash_table_size(join_requests));
	g_hash_table_foreach(join_requests, finalize_join_for, NULL);
	
	if(num_join_invites <= g_hash_table_size(confirmed_nodes)) {
		crm_info("Not expecting any join confirmations");
		
		register_fsa_input(C_FSA_INTERNAL, I_FINALIZED, NULL);
		return I_NULL;
	}

	/* dont waste time by invoking the PE yet; */
	crm_debug("Still waiting on %d outstanding join confirmations",
		  num_join_invites - g_hash_table_size(confirmed_nodes));

	return I_NULL;
}

/*	A_DC_JOIN_PROCESS_ACK	*/
enum crmd_fsa_input
do_dc_join_ack(long long action,
	       enum crmd_fsa_cause cause,
	       enum crmd_fsa_state cur_state,
	       enum crmd_fsa_input current_input,
	       fsa_data_t *msg_data)
{
	/* now update them to "member" */
	xmlNodePtr tmp1 = NULL, update = NULL;
	xmlNodePtr join_ack = (xmlNodePtr)msg_data->data;
	const char *join_from = xmlGetProp(join_ack, XML_ATTR_HOSTFROM);
	xmlNodePtr options = find_xml_node(join_ack, XML_TAG_OPTIONS);
	const char *op = xmlGetProp(options, XML_ATTR_OP);
	const char *join_state = NULL;


	if(safe_str_neq(op, CRM_OP_JOINACK)) {
		crm_warn("Ignoring op=%s message", op);
		return I_NULL;
	}
	
	crm_debug("Processing ack from %s", join_from);

	join_state = (const char *)
		g_hash_table_lookup(join_requests, join_from);
	
	if(join_state == NULL) {
		crm_err("Join not in progress: ignoring join from %s",
			join_from);
		return I_FAIL;
		
	} else if(safe_str_neq(join_state, CRMD_JOINSTATE_MEMBER)) {
		crm_err("Node %s wasnt invited to join the cluster",join_from);
		return I_NULL;
	}
	
	g_hash_table_insert(confirmed_nodes, crm_strdup(join_from),
			    crm_strdup(CRMD_JOINSTATE_MEMBER));

	/* update node entry in the status section  */
	crm_debug("Updating node state to %s for %s", join_state, join_from);
	update = create_node_state(
		join_from, join_from,
		ACTIVESTATUS, NULL, ONLINESTATUS, join_state, join_state);

	set_xml_property_copy(update,XML_CIB_ATTR_EXPSTATE, CRMD_STATE_ACTIVE);

	tmp1 = create_cib_fragment(update, NULL);

	update_local_cib(tmp1, TRUE);

	free_xml(tmp1);

	/* update CIB with the current LRM status from the node */
	tmp1 = find_xml_node(join_ack, XML_TAG_FRAGMENT);
	update_local_cib(tmp1, TRUE);

	if(num_join_invites <= g_hash_table_size(confirmed_nodes)) {
		crm_info("That was the last outstanding join confirmation");
		register_fsa_input_later(C_FSA_INTERNAL, I_FINALIZED, NULL);
		return I_NULL;
	}

	/* dont waste time by invoking the pe yet; */
	crm_debug("Still waiting on %d outstanding join confirmations",
		  num_join_invites - g_hash_table_size(confirmed_nodes));
	
	return I_NULL;
}

void
finalize_join_for(gpointer key, gpointer value, gpointer user_data)
{
	xmlNodePtr tmp1 = NULL;
	xmlNodePtr tmp2 = NULL;
	xmlNodePtr options     = NULL;

	const char *join_to = NULL;
	const char *join_state = NULL;

	if(key == NULL || value == NULL) {
		return;
	}

	options     = create_xml_node(NULL, XML_TAG_OPTIONS);
	join_to    = (const char *)key;
	join_state = (const char *)value;

	/* make sure the node exists in the config section */
	create_node_entry(join_to, join_to, CRMD_JOINSTATE_MEMBER);

	/* perhaps we shouldnt special case this... */
	if(safe_str_eq(join_to, fsa_our_uname)) {
		/* mark ourselves confirmed */
		g_hash_table_insert(confirmed_nodes, crm_strdup(fsa_our_uname),
				    crm_strdup(CRMD_JOINSTATE_MEMBER));

		/* make sure our cluster state is set correctly */
		tmp1 = create_node_state(
			join_to, join_to, ACTIVESTATUS,
			NULL, ONLINESTATUS, join_state, join_state);

		if(tmp1 != NULL) {
			tmp2 = create_cib_fragment(tmp1, NULL);
			update_local_cib(tmp2, TRUE);
			free_xml(tmp2);
			free_xml(tmp1);

		} else {
			crm_err("Could not create our node state");
			/* TODO: raise an error input */
		}

		/* update our LRM data */
		tmp1 = do_lrm_query(TRUE);
		if(tmp1 != NULL) {
			update_local_cib(tmp1, TRUE);
			free_xml(tmp1);

		} else {
			crm_err("Could not determin current LRM state");
			/* TODO: raise an error input */
		}

		num_join_invites++;
		crm_info("Completed local cluster membership");
		return;
	}
	
	/* create the ack/nack */
	if(safe_str_eq(join_state, CRMD_JOINSTATE_MEMBER)) {
		crm_info("ACK'ing join request from %s, state %s",
			 join_to, join_state);
		num_join_invites++;
		set_xml_property_copy(
			options, CRM_OP_JOINACK, XML_BOOLEAN_TRUE);

	} else {
		crm_warn("NACK'ing join request from %s, state %s",
			 join_to, join_state);
		
		set_xml_property_copy(
			options, CRM_OP_JOINACK, XML_BOOLEAN_FALSE);
	}

	/* send the ack/nack to the node */
	send_request(options, NULL, CRM_OP_JOINACK,
		     join_to, CRM_SYSTEM_CRMD, NULL);	
}

void
initialize_join(gboolean before)
{
	/* clear out/reset a bunch of stuff */
	if(join_offers != NULL) {
		g_hash_table_destroy(join_offers);
	}
	if(join_requests != NULL) {
		g_hash_table_destroy(join_requests);
	}
	if(confirmed_nodes != NULL) {
		g_hash_table_destroy(confirmed_nodes);
	}

	if(before) {
		free_xml(our_generation);
		our_generation  = cib_get_generation(fsa_cib_conn);
		
		max_generation_from = NULL;
		set_bit_inplace(fsa_input_register, R_HAVE_CIB);
		clear_bit_inplace(fsa_input_register, R_CIB_ASKED);
	}
	
	join_offers     = g_hash_table_new(&g_str_hash, &g_str_equal);
	join_requests   = g_hash_table_new(&g_str_hash, &g_str_equal);
	confirmed_nodes = g_hash_table_new(&g_str_hash, &g_str_equal);

	/* mark ourselves joined */
	g_hash_table_insert(join_requests, crm_strdup(fsa_our_uname),
			    crm_strdup(CRMD_JOINSTATE_MEMBER));
	
}


void
join_send_offer(gpointer key, gpointer value, gpointer user_data)
{
	const char *join_to = NULL;
	const oc_node_t *member = (const oc_node_t*)value;

	crm_debug("Sending %s offer", CRM_OP_WELCOME);
	if(member != NULL) {
		join_to = member->node_uname;
	}

	if(join_to == NULL) {
		crm_err("No recipient for welcome message");

	} else if(safe_str_eq(join_to, fsa_our_uname)) {
		crm_debug("Skipping %s msg for ourselves (%s)",
			  CRM_OP_WELCOME, join_to);

	} else {
		/* send the welcome */
		crm_debug("Sending %s to %s", CRM_OP_WELCOME, join_to);
			
		send_request(NULL, NULL, CRM_OP_WELCOME,
			     join_to, CRM_SYSTEM_CRMD, NULL);

		g_hash_table_insert(join_offers, crm_strdup(join_to),
				    crm_strdup(CRMD_JOINSTATE_PENDING));
		
	}
}

