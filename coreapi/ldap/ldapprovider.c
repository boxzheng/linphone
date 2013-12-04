/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "ldapprovider.h"
#include "linphonecore.h"
#include "linphonecore_utils.h"
#include "lpconfig.h"

#include <ldap.h>


#define MAX_RUNNING_REQUESTS 10
#define FILTER_MAX_SIZE      512

typedef enum {
	ANONYMOUS,
	PLAIN,
	SASL
} LDAPAuthMethod;

struct LDAPFriendData {
	char* name;
	char* sip;
};

struct _LinphoneLDAPContactProvider
{
	LinphoneContactProvider base;
	LDAP*   ld;
	MSList* requests;
	uint    req_count;

	// bind transaction
	uint bind_msgid;
	bool_t connected;

	// config
	int use_tls;
	LDAPAuthMethod auth_method;
	char*  username;
	char*  password;
	char*  server;

	char*  base_object;
	char** attributes;
	char*  sip_attr;
	char*  name_attr;

	char*  filter;
	int    timeout;
	int    deref_aliases;
	int    max_results;
};

struct _LinphoneLDAPContactSearch
{
	LinphoneContactSearch base;
	LDAP*   ld;
	int     msgid;
	char*   filter;
	bool_t  complete;
	MSList* found_entries;
	int     found_count;
};


/* *************************
 * LinphoneLDAPContactSearch
 * *************************/

LinphoneLDAPContactSearch* linphone_ldap_contact_search_create(LinphoneLDAPContactProvider* cp, const char* predicate, ContactSearchCallback cb, void* cb_data)

{
	LinphoneLDAPContactSearch* search = belle_sip_object_new(LinphoneLDAPContactSearch);
	LinphoneContactSearch* base = LINPHONE_CONTACT_SEARCH(search);
	struct timeval timeout = { cp->timeout, 0 };

	linphone_contact_search_init(base, predicate, cb, cb_data);

	search->ld = cp->ld;

	search->filter = ms_malloc(FILTER_MAX_SIZE);
	snprintf(search->filter, FILTER_MAX_SIZE-1, cp->filter, predicate);
	search->filter[FILTER_MAX_SIZE-1] = 0;

	ms_message("Calling ldap_search_ext with predicate '%s' on base %s", search->filter, cp->base_object);

	int ret = ldap_search_ext(search->ld,
					cp->base_object, // base from which to start
					LDAP_SCOPE_SUBTREE,
					search->filter, // search predicate
					cp->attributes, // which attributes to get
					0,              // 0 = get attrs AND value, 1 = get attrs only
					NULL,
					NULL,
					&timeout,       // server timeout for the search
					cp->max_results,// max result number
					&search->msgid );
	if( ret != LDAP_SUCCESS ){
		ms_error("Error ldap_search_ext returned %d (%s)", ret, ldap_err2string(ret));
		belle_sip_object_unref(search);
		return NULL;
	} else {
		ms_message("LinphoneLDAPContactSearch created @%p : msgid %d", search, search->msgid);
	}
	return search;
}

void linphone_ldap_contact_search_destroy_friend( void* entry )
{
	linphone_friend_destroy((LinphoneFriend*)entry);
}

static void linphone_ldap_contact_search_destroy( LinphoneLDAPContactSearch* obj )
{
	ms_message("~LinphoneLDAPContactSearch(%p)", obj);
	ms_list_for_each(obj->found_entries, linphone_ldap_contact_search_destroy_friend);
	obj->found_entries = ms_list_free(obj->found_entries);
	if( obj->filter ) ms_free(obj->filter);
}

BELLE_SIP_DECLARE_NO_IMPLEMENTED_INTERFACES(LinphoneLDAPContactSearch);
BELLE_SIP_INSTANCIATE_VPTR(LinphoneLDAPContactSearch,LinphoneContactSearch,
						   (belle_sip_object_destroy_t)linphone_ldap_contact_search_destroy,
						   NULL,
						   NULL,
						   TRUE
);


/* ***************************
 * LinphoneLDAPContactProvider
 * ***************************/
static inline LinphoneLDAPContactSearch* linphone_ldap_request_entry_search( LinphoneLDAPContactProvider* obj, int msgid );
static unsigned int linphone_ldap_contact_provider_cancel_search(LinphoneContactProvider* obj, LinphoneContactSearch *req);
static void linphone_ldap_contact_provider_conf_destroy(LinphoneLDAPContactProvider* obj );

/* Authentication methods */
struct AuthMethodDescription{
	LDAPAuthMethod method;
	const char* description;
};

static struct AuthMethodDescription ldap_auth_method_description[] = {
	{ANONYMOUS, "anonymous"},
	{PLAIN,     "plain"},
	{SASL,      "sasl"},
	{0,         NULL}
};

static LDAPAuthMethod linphone_ldap_contact_provider_auth_method( const char* description )
{
	struct AuthMethodDescription* desc = ldap_auth_method_description;
	while( desc && desc->description ){
		if( strcmp(description, desc->description) == 0)
			return desc->method;
		desc++;
	}
	return ANONYMOUS;
}

static void linphone_ldap_contact_provider_destroy_request(void *req)
{
	belle_sip_object_unref(req);
}

static void linphone_ldap_contact_provider_destroy( LinphoneLDAPContactProvider* obj )
{
	// clean pending requests
	ms_list_for_each(obj->requests, linphone_ldap_contact_provider_destroy_request);

	if (obj->ld) ldap_unbind_ext(obj->ld, NULL, NULL);
	obj->ld = NULL;

	linphone_ldap_contact_provider_conf_destroy(obj);
}

static int linphone_ldap_contact_provider_parse_bind_results( LinphoneLDAPContactProvider* obj, LDAPMessage* results )
{
	int ret = ldap_parse_sasl_bind_result(obj->ld, results, NULL, 0);
	if( ret != LDAP_SUCCESS ){
		ms_error("ldap_parse_sasl_bind_result failed");
	} else {
		obj->connected = TRUE;
	}
	return ret;
}

static int linphone_ldap_contact_provider_complete_contact( LinphoneLDAPContactProvider* obj, struct LDAPFriendData* lf, const char* attr_name, const char* attr_value)
{
	if( strcmp(attr_name, obj->name_attr ) == 0 ){
		lf->name = ms_strdup(attr_value);
	} else if( strcmp(attr_name, obj->sip_attr) == 0 ) {
		lf->sip = ms_strdup(attr_value);
	}

	// return 1 if the structure has enough data to create a linphone friend
	if( lf->name && lf->sip )
		return 1;
	else
		return 0;

}

static void linphone_ldap_contact_provider_handle_search_result( LinphoneLDAPContactProvider* obj, LinphoneLDAPContactSearch* req, LDAPMessage* message )
{
	int msgtype = ldap_msgtype(message);

	switch(msgtype){

	case LDAP_RES_SEARCH_ENTRY:
	case LDAP_RES_EXTENDED:
	{
		LDAPMessage *entry = ldap_first_entry(obj->ld, message);
		LinphoneCore*   lc = LINPHONE_CONTACT_PROVIDER(obj)->lc;

		while( entry != NULL ){

			struct LDAPFriendData ldap_data = {0};
			bool_t contact_complete = FALSE;
			BerElement*  ber = NULL;
			char*       attr = ldap_first_attribute(obj->ld, entry, &ber);
			char*         dn = ldap_get_dn(obj->ld, entry);


			if( dn ){
				ms_message("search result: dn: %s", dn);
				ldap_memfree(dn);
			}

			while( attr ){
				struct berval** values = ldap_get_values_len(obj->ld, entry, attr);
				struct berval**     it = values;

				while( values && *it && (*it)->bv_val && (*it)->bv_len )
				{
					ms_message("%s -> %s", attr, (*it)->bv_val);

					contact_complete = linphone_ldap_contact_provider_complete_contact(obj, &ldap_data, attr, (*it)->bv_val);
					if( contact_complete ) break;

					it++;
				}

				if( values ) ldap_value_free_len(values);
				ldap_memfree(attr);

				if( contact_complete ) break;

				attr = ldap_next_attribute(obj->ld, entry, ber);
			}

			if( contact_complete ) {
				LinphoneAddress* la = linphone_core_interpret_url(lc, ldap_data.sip);
				if( la ){
					LinphoneFriend* lf = linphone_core_create_friend(lc);
					linphone_friend_set_address(lf, la);
					linphone_friend_set_name(lf, ldap_data.name);
					req->found_entries = ms_list_append(req->found_entries, lf);
					req->found_count++;
					ms_message("Added friend %s / %s", ldap_data.name, ldap_data.sip);
					ms_free(ldap_data.sip);
					ms_free(ldap_data.name);
				}
			}

			if( ber ) ber_free(ber, 0);

			entry = ldap_next_entry(obj->ld, entry);
		}
	}
	break;

	case LDAP_RES_SEARCH_RESULT:
	{
		// this one is received when a request is finished
		req->complete = TRUE;
		linphone_contact_search_invoke_cb(LINPHONE_CONTACT_SEARCH(req), req->found_entries);
	}
	break;


	default: ms_message("Unhandled message type %x", msgtype); break;
	}
}

static bool_t linphone_ldap_contact_provider_iterate(void *data)
{
	LinphoneLDAPContactProvider* obj = LINPHONE_LDAP_CONTACT_PROVIDER(data);
	if( obj->ld && ((obj->req_count > 0) || (obj->bind_msgid != 0) )){

		// never block
		struct timeval timeout = {0,0};
		LDAPMessage* results = NULL;

		int ret = ldap_result(obj->ld, LDAP_RES_ANY, LDAP_MSG_ONE, &timeout, &results);

		if( ret != 0 && ret != -1) ms_message("ldap_result %x", ret);

		switch( ret ){
		case -1:
		{
			ms_warning("Error in ldap_result : returned -1 (req_count %d, bind_msgid %d): %s", obj->req_count, obj->bind_msgid, ldap_err2string(errno));
			break;
		}
		case 0: break; // nothing to do

		case LDAP_RES_BIND:
		{
			ms_message("iterate: LDAP_RES_BIND");
			if( ldap_msgid( results ) != obj->bind_msgid ) {
				ms_error("Bad msgid");
			} else {
				linphone_ldap_contact_provider_parse_bind_results( obj, results );
				obj->bind_msgid = 0; // we're bound now, don't bother checking again
			}
			break;
		}
		case LDAP_RES_EXTENDED:
		case LDAP_RES_SEARCH_ENTRY:
		case LDAP_RES_SEARCH_REFERENCE:
		case LDAP_RES_INTERMEDIATE:
		case LDAP_RES_SEARCH_RESULT:
		{
			LDAPMessage* message = ldap_first_message(obj->ld, results);
			LinphoneLDAPContactSearch* req = linphone_ldap_request_entry_search(obj, ldap_msgid(message));
			while( message != NULL ){
				ms_message("Message @%p:id %d / type %x / associated request: %p", message, ldap_msgid(message), ldap_msgtype(message), req);
				linphone_ldap_contact_provider_handle_search_result(obj, req, message );
				message = ldap_next_message(obj->ld, message);
			}
			if( req && ret == LDAP_RES_SEARCH_RESULT) linphone_ldap_contact_provider_cancel_search(LINPHONE_CONTACT_PROVIDER(obj), LINPHONE_CONTACT_SEARCH(req));
			break;
		}
		case LDAP_RES_MODIFY:
		case LDAP_RES_ADD:
		case LDAP_RES_DELETE:
		case LDAP_RES_MODDN:
		case LDAP_RES_COMPARE:
		default:
			ms_message("Unhandled LDAP result %x", ret);
			break;
		}

		if( results )
			ldap_msgfree(results);
	}
	return TRUE;
}

static void linphone_ldap_contact_provider_conf_destroy(LinphoneLDAPContactProvider* obj )
{
	if(obj->username)    ms_free(obj->username);
	if(obj->password)    ms_free(obj->password);
	if(obj->base_object) ms_free(obj->base_object);
	if(obj->filter)      ms_free(obj->filter);
	if(obj->sip_attr)    ms_free(obj->sip_attr);
	if(obj->name_attr)   ms_free(obj->name_attr);

	if(obj->attributes){
		int i=0;
		for( ; obj->attributes[i]; i++){
			ms_free(obj->attributes[i]);
		}
		ms_free(obj->attributes);
	}
}

static void linphone_ldap_contact_provider_loadconfig(LinphoneLDAPContactProvider* obj, LpConfig* config)
{
	const char* section="ldap";
	char* attributes_list, *saveptr, *attr;
	unsigned int attr_count = 0, attr_idx = 0, i;

	obj->use_tls       = lp_config_get_int(config, section, "use_tls",       0);
	obj->timeout       = lp_config_get_int(config, section, "timeout",       10);
	obj->deref_aliases = lp_config_get_int(config, section, "deref_aliases", 0);
	obj->max_results   = lp_config_get_int(config, section, "max_results",   50);
	obj->auth_method   = linphone_ldap_contact_provider_auth_method( lp_config_get_string(config, section, "auth_method", "anonymous"));

	// free any pre-existing char* conf values
	linphone_ldap_contact_provider_conf_destroy(obj);

	obj->username    = ms_strdup(lp_config_get_string(config, section, "username",       ""));
	obj->password    = ms_strdup(lp_config_get_string(config, section, "password",       ""));
	obj->base_object = ms_strdup(lp_config_get_string(config, section, "base_object",    "dc=example,dc=com"));
	obj->server      = ms_strdup(lp_config_get_string(config, section, "server",         "ldap://localhost:10389"));
	obj->filter      = ms_strdup(lp_config_get_string(config, section, "filter",         "uid=*%s*"));
	obj->name_attr   = ms_strdup(lp_config_get_string(config, section, "name_attribute", "givenName"));
	obj->sip_attr    = ms_strdup(lp_config_get_string(config, section, "sip_attribute",  "mobile"));

	/*
	 * parse the attributes list
	 */
	attributes_list = ms_strdup(lp_config_get_string(config, section,
													 "attributes",
													 "telephoneNumber,givenName,sn,mobile,homePhone"));

	// count attributes:
	for( i=0; attributes_list[i]; i++) {
		if( attributes_list[i] == ',') attr_count++;
	}

	// 1 more for the first attr without ',', the other for the null-finished list
	obj->attributes = ms_malloc0((attr_count+2) * sizeof(char*));

	attr = strtok_r( attributes_list, ",", &saveptr );
	while( attr != NULL ){
		obj->attributes[attr_idx] = ms_strdup(attr);
		attr_idx++;
		attr = strtok_r(NULL, ",", &saveptr);
	}
	if( attr_idx != attr_count+1) ms_error("Invalid attribute number!!! %d expected, got %d", attr_count+1, attr_idx);

	ms_free(attributes_list);
}

static int linphone_ldap_contact_provider_bind( LinphoneLDAPContactProvider* obj )
{
	struct berval password = { strlen( obj->password), obj->password };
	int ret;
	int bind_msgid = 0;

	switch( obj->auth_method ){
	case ANONYMOUS:
	default:
	{
		char *auth = NULL;
		ret = ldap_sasl_bind( obj->ld, obj->base_object, auth, &password, NULL, NULL, &bind_msgid);
		if( ret == LDAP_SUCCESS ) {
			obj->bind_msgid = bind_msgid;
		} else {
			int err;
			ldap_get_option(obj->ld, LDAP_OPT_RESULT_CODE, &err);
			ms_error("ldap_sasl_bind error %d (%s)", err, ldap_err2string(err) );
		}
		break;
	}
	case SASL:
	{
		break;
	}
	}
	return 0;
}

LinphoneLDAPContactProvider*linphone_ldap_contact_provider_create(LinphoneCore* lc)
{
	LinphoneLDAPContactProvider* obj = belle_sip_object_new(LinphoneLDAPContactProvider);
	int proto_version = LDAP_VERSION3;

	linphone_contact_provider_init((LinphoneContactProvider*)obj, lc);
	ms_message( "Constructed Contact provider '%s'", BELLE_SIP_OBJECT_VPTR(obj,LinphoneContactProvider)->name);

	linphone_ldap_contact_provider_loadconfig(obj, linphone_core_get_config(lc));

	int ret = ldap_initialize(&(obj->ld),obj->server);

	if( ret != LDAP_SUCCESS ){
		ms_error( "Problem initializing ldap on url '%s': %s", obj->server, ldap_err2string(ret));
		belle_sip_object_unref(obj);
		return NULL;
	} else if( (ret = ldap_set_option(obj->ld, LDAP_OPT_PROTOCOL_VERSION, &proto_version)) != LDAP_SUCCESS ){
		ms_error( "Problem setting protocol version %d: %s", proto_version, ldap_err2string(ret));
		belle_sip_object_unref(obj);
		return NULL;
	} else {
		// register our hook into iterate so that LDAP can do its magic asynchronously.
		linphone_ldap_contact_provider_bind(obj);
		linphone_core_add_iterate_hook(lc, linphone_ldap_contact_provider_iterate, obj);
	}
	return obj;
}

/**
 * Search an LDAP request in the list of current LDAP requests to serve, only taking
 * the msgid as a key to search.
 */
static int linphone_ldap_request_entry_compare_weak(const void*a, const void* b)
{
	const LinphoneLDAPContactSearch* ra = (const LinphoneLDAPContactSearch*)a;
	const LinphoneLDAPContactSearch* rb = (const LinphoneLDAPContactSearch*)b;
	return !(ra->msgid == rb->msgid); // 0 if equal
}

/**
 * Search an LDAP request in the list of current LDAP requests to serve, with strong search
 * comparing both msgid and request pointer
 */
static int linphone_ldap_request_entry_compare_strong(const void*a, const void* b)
{
	const LinphoneLDAPContactSearch* ra = (const LinphoneLDAPContactSearch*)a;
	const LinphoneLDAPContactSearch* rb = (const LinphoneLDAPContactSearch*)b;
	return !(ra->msgid == rb->msgid) && !(ra == rb);
}

static inline LinphoneLDAPContactSearch* linphone_ldap_request_entry_search( LinphoneLDAPContactProvider* obj, int msgid )
{
	LinphoneLDAPContactSearch dummy = {};
	dummy.msgid = msgid;

	MSList* list_entry = ms_list_find_custom(obj->requests, linphone_ldap_request_entry_compare_weak, &dummy);
	if( list_entry ) return list_entry->data;
	else return NULL;
}

static unsigned int linphone_ldap_contact_provider_cancel_search(LinphoneContactProvider* obj, LinphoneContactSearch *req)
{
	LinphoneLDAPContactSearch*  ldap_req = LINPHONE_LDAP_CONTACT_SEARCH(req);
	LinphoneLDAPContactProvider* ldap_cp = LINPHONE_LDAP_CONTACT_PROVIDER(obj);
	int ret = 1;

	MSList* list_entry = ms_list_find_custom(ldap_cp->requests, linphone_ldap_request_entry_compare_strong, req);
	if( list_entry ) {
		ldap_cp->requests = ms_list_remove_link(ldap_cp->requests, list_entry);
		ldap_cp->req_count--;
		ret = 0; // return OK if we found it in the monitored requests
	} else {
		ms_warning("Couldn't find ldap request %p (id %d) in monitoring.", ldap_req, ldap_req->msgid);
	}
	belle_sip_object_unref(req); // unref request even if not found
	return ret;
}

static LinphoneLDAPContactSearch* linphone_ldap_begin_search ( LinphoneLDAPContactProvider* obj,
		const char* predicate,
		ContactSearchCallback cb,
		void* cb_data )
{
	LinphoneLDAPContactSearch* request = linphone_ldap_contact_search_create ( obj, predicate, cb, cb_data );

	if ( request != NULL ) {
		ms_message ( "Created search %d for '%s', msgid %d, @%p", obj->req_count, predicate, request->msgid, request );

		obj->requests  = ms_list_append ( obj->requests, request );
		obj->req_count++;
	}
	return request;
}


static int linphone_ldap_marshal(LinphoneLDAPContactProvider* obj, char* buff, size_t buff_size, size_t *offset)
{
	belle_sip_error_code error = BELLE_SIP_OK;

	error = belle_sip_snprintf(buff, buff_size, offset, "ld:%p,\n", obj->ld);
	if(error!= BELLE_SIP_OK) return error;

	error = belle_sip_snprintf(buff, buff_size, offset, "req_count:%d,\n", obj->req_count);
	if(error!= BELLE_SIP_OK) return error;

	error = belle_sip_snprintf(buff, buff_size, offset, "bind_msgid:%d,\n", obj->bind_msgid);
	if(error!= BELLE_SIP_OK) return error;

	error = belle_sip_snprintf(buff, buff_size, offset,
							   "CONFIG:\n"
							   "tls: %d \n"
							   "auth: %d \n"
							   "user: %s \n"
							   "pass: %s \n"
							   "server: %s \n"
							   "base: %s \n"
							   "filter: %s \n"
							   "timeout: %d \n"
							   "deref: %d \n"
							   "max_res: %d \n"
							   "sip_attr:%s \n"
							   "name_attr:%s \n"
							   "attrs:\n",
							   obj->use_tls, obj->auth_method,
							   obj->username, obj->password, obj->server,
							   obj->base_object, obj->filter,
							   obj->timeout, obj->deref_aliases,
							   obj->max_results,
							   obj->sip_attr, obj->name_attr);
	if(error!= BELLE_SIP_OK) return error;

	char **attr = obj->attributes;
	while( *attr ){
		error = belle_sip_snprintf(buff, buff_size, offset, "- %s\n", *attr);
		if(error!= BELLE_SIP_OK) return error;
		else attr++;
	}

	return error;

}


BELLE_SIP_DECLARE_NO_IMPLEMENTED_INTERFACES(LinphoneLDAPContactProvider);

BELLE_SIP_INSTANCIATE_CUSTOM_VPTR(LinphoneLDAPContactProvider)=
{
	{
		{
			BELLE_SIP_VPTR_INIT(LinphoneLDAPContactProvider,LinphoneContactProvider,TRUE),
			(belle_sip_object_destroy_t)linphone_ldap_contact_provider_destroy,
			NULL,
			(belle_sip_object_marshal_t)linphone_ldap_marshal
		},
		"LDAP",
		(LinphoneContactProviderStartSearchMethod)linphone_ldap_begin_search,
		(LinphoneContactProviderCancelSearchMethod)linphone_ldap_contact_provider_cancel_search
	}
};

