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
#include <crm/common/crm.h>
#include <crmd_fsa.h>
#include <libxml/tree.h>
#include <crm/common/xmltags.h>
#include <crm/common/xmlvalues.h>
#include <crm/common/xmlutils.h>
#include <crm/common/ipcutils.h>
#include <crm/common/msgutils.h>
#include <string.h>

/*	A_ELECTION_VOTE	*/
enum crmd_fsa_input
do_election_vote(long long action,
		 enum crmd_fsa_state cur_state,
		 enum crmd_fsa_input current_input,
		 void *data)
{
	const char *node_uname = NULL;
	enum crmd_fsa_input election_result = I_NULL;
	FNIN();

	node_uname = "";//fsa_membership_copy->members[0].node_uname;
		
	if (strcmp(fsa_our_uname, node_uname) == 0) {
		// bypass the election
		election_result = I_ELECTION_DC;
	} else {
		// set the "we won" timer
		startTimer(election_timeout);
	}
	
	// send our vote unless we want to shut down

	xmlNodePtr msg_options = create_xml_node(NULL, XML_TAG_OPTIONS);
	set_xml_property_copy(msg_options, XML_ATTR_OP, "vote");

	
	/* host_to = NULL (for broadcast) */
	send_ha_request(fsa_cluster_connection,
			msg_options, NULL,
			NULL, CRM_SYSTEM_CRMD,
			CRM_SYSTEM_CRMD, NULL,
			NULL);

	free_xml(msg_options);
	
	FNRET(I_NULL);
}

gboolean
timer_popped(gpointer data)
{
	fsa_timer_t *timer = (fsa_timer_t *)data;

	stopTimer(timer); // dont make it go off again
	
	s_crmd_fsa(C_TIMER_POPPED, timer->fsa_input, NULL);
	
	return TRUE;
}

/*	A_ELECTION_COUNT	*/
enum crmd_fsa_input
do_election_count_vote(long long action,
		       enum crmd_fsa_state cur_state,
		       enum crmd_fsa_input current_input,
		       void *data)
{
	xmlNodePtr vote = (xmlNodePtr)data;
	const char *vote_from = xmlGetProp(vote, XML_ATTR_HOSTFROM);
	int lpc = 0, my_index = -1, your_index = -1;
	enum crmd_fsa_input election_result = I_NULL;
	
	FNIN();

	if(vote_from == NULL) {
		FNRET(election_result);
	}
	
	for(; (your_index < 0 && my_index < 0)
		    || lpc < fsa_membership_copy->members_size; lpc++) {
		
		const char *node_uname = "";
		//fsa_membership_copy->members[lpc].node_uname;
		
		if(node_uname != NULL) {
			if(strcmp(vote_from, node_uname) == 0)
				your_index = lpc;
			else if (strcmp(fsa_our_uname,
					node_uname) == 0)
				my_index = lpc;
		}
	}
	
	if(your_index > my_index) {
		if(fsa_input_register & R_THE_DC) {
			election_result = I_ELECTION_RELEASE_DC;
		} else {
			election_result = I_NOT_DC;
		}
	} else if(my_index == 0) {
		election_result = I_ELECTION_DC;
	}
	
	if(election_result != I_NULL) {
		// cancel timer, we lost the election
		stopTimer(election_timeout);

		// set a new timer for when the DC goes bad
		startTimer(election_trigger);
	}
	
	FNRET(election_result);
}

/*	A_ELECTION_TIMEOUT	*/
// we won
enum crmd_fsa_input
do_election_timeout(long long action,
		    enum crmd_fsa_state cur_state,
		    enum crmd_fsa_input current_input,
		    void *data)
{
	FNIN();
	
	// cleanup timer
	stopTimer(election_timeout);
	
	FNRET(I_ELECTION_DC);
}

/*	A_TICKLE_DC_TIMER	*/
/* We saw something from the DC, so we know its alive */
enum crmd_fsa_input
do_tickle_dc_timer(long long action,
		enum crmd_fsa_state cur_state,
		enum crmd_fsa_input current_input,
		void *data)
{
	FNIN();

	if(election_trigger->source_id != 0) {
		stopTimer(election_trigger);
		startTimer(election_trigger);
	}
	
	FNRET(I_NULL);
}


/*	 A_DC_TAKEOVER	*/
enum crmd_fsa_input
do_dc_takeover(long long action,
	       enum crmd_fsa_state cur_state,
	       enum crmd_fsa_input current_input,
	       void *data)
{
	FNIN();

	set_bit_inplace(&fsa_input_register, R_THE_DC);

	CRM_DEBUG2("Am I the DC? %s", AM_I_DC?"yes":"no");
	
	set_bit_inplace(&fsa_input_register, R_JOIN_OK);
	set_bit_inplace(&fsa_input_register, R_INVOKE_PE);
	
	clear_bit_inplace(&fsa_input_register, R_CIB_DONE);
	clear_bit_inplace(&fsa_input_register, R_HAVE_CIB);

	FNRET(I_NULL);
}

/*	 A_DC_RELEASE	*/
enum crmd_fsa_input
do_dc_release(long long action,
	      enum crmd_fsa_state cur_state,
	      enum crmd_fsa_input current_input,
	      void *data)
{
	FNIN();

	clear_bit_inplace(&fsa_input_register, R_THE_DC);

	/* get a new CIB from the new DC */
	clear_bit_inplace(&fsa_input_register, R_HAVE_CIB);
	
	FNRET(I_NULL);
}

/*	 A_JOIN_WELCOME	*/
enum crmd_fsa_input
do_join_welcome(long long action,
		enum crmd_fsa_state cur_state,
		enum crmd_fsa_input current_input,
		void *data)
{
	int lpc = 0;
	oc_node_t *new_members;
	gboolean was_sent = TRUE;
	
	FNIN();

	new_members = fsa_membership_copy->new_members;
	
	xmlNodePtr msg_options = create_xml_node(NULL, XML_TAG_OPTIONS);
	set_xml_property_copy(msg_options, XML_ATTR_OP, "welcome");
	
	for(; lpc < fsa_membership_copy->new_members_size; lpc++) {
#ifdef CCM_UNAME
		was_sent = was_sent
			&& send_ha_request(fsa_cluster_connection,
					   msg_options, NULL,
					   new_members[lpc].node_uname, CRM_SYSTEM_CRMD,
					   CRM_SYSTEM_DC, NULL,
					   NULL);
#endif
	}
	free_xml(msg_options);

	if(was_sent == FALSE)
		FNRET(I_FAIL);
	
	FNRET(I_NULL);
}

/*	 A_JOIN_ACK	*/
enum crmd_fsa_input
do_join_ack(long long action,
	    enum crmd_fsa_state cur_state,
	    enum crmd_fsa_input current_input,
	    void *data)
{
	xmlNodePtr welcome = (xmlNodePtr)data;
	FNIN();

#if 0
	if(we are sick) {
		log error ;
		FNRET(I_NULL);
	} 
#endif
	xmlNodePtr msg_options = find_xml_node(welcome, XML_TAG_OPTIONS);
	
	set_xml_property_copy(msg_options, XML_ATTR_OP, "join_ack");
	
	send_ha_reply(fsa_cluster_connection,
		      welcome, NULL);
	
	FNRET(I_NULL);
}

/*	 A_PROCESS_JOIN_ACK	*/
enum crmd_fsa_input
do_process_join_ack(long long action,
		    enum crmd_fsa_state cur_state,
		    enum crmd_fsa_input current_input,
		    void *data)
{
	int lpc = 0;
	gboolean is_a_member = FALSE;
	xmlNodePtr join_ack = (xmlNodePtr)data;
	const char *join_from = xmlGetProp(join_ack, XML_ATTR_HOSTFROM);
	FNIN();
	
//	oc_node_t *members = fsa_membership_copy->members;
	
	for(; lpc < fsa_membership_copy->members_size; lpc++) {
#ifdef CCM_UNAME
		if(strcmp(join_from, members[lpc].node_uname) == 0) {
			is_a_member = TRUE;
		}
#endif
	}
	if(is_a_member == FALSE) {
		cl_log(LOG_ERR, "Node %s is not known to us", join_from);
		FNRET(I_NULL);
	}
	
	// update their copy of the CIB
	
	/*
	 * Send a message to the CIB asking what the contents are.
	 *
	 * Forward the ack so that the reply will be directed appropriatly
	 */

	xmlNodePtr msg_options = find_xml_node(join_ack, XML_TAG_OPTIONS);
	
	set_xml_property_copy(msg_options, XML_ATTR_OP, "forward");

	forward_ipc_request(cib_subsystem->ipc,
			    join_ack, NULL,
			    CRM_SYSTEM_DCIB, CRM_SYSTEM_CIB);

	FNRET(I_NULL);
}
