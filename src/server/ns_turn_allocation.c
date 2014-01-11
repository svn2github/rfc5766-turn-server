/*
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "ns_turn_allocation.h"

/////////////// Permission forward declarations /////////////////

static void init_turn_permission_hashtable(turn_permission_hashtable *map);
static void free_turn_permission_hashtable(turn_permission_hashtable *map);
static turn_permission_info* get_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr *addr);
static void remove_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr *addr);

/////////////// ALLOCATION //////////////////////////////////////

void init_allocation(void *owner, allocation* a, ur_map *tcp_connections) {
  if(a) {
    ns_bzero(a,sizeof(allocation));
    a->owner = owner;
    a->channel_to_ch_info=ur_map_create();
    a->tcp_connections = tcp_connections;
    init_turn_permission_hashtable(&(a->addr_to_perm));
  }
}

void clear_allocation(allocation *a)
{
	if (a) {

		if(a->is_valid)
			turn_report_allocation_delete(a);

		while(a->tcl.next) {
			tcp_connection *tc = (tcp_connection*)(a->tcl.next);
			delete_tcp_connection(tc);
		}

		clear_ioa_socket_session_if(a->relay_session.s, a->owner);
		clear_ts_ur_session_data(&(a->relay_session));

		IOA_EVENT_DEL(a->lifetime_ev);

		/* The order is important here: */
		free_turn_permission_hashtable(&(a->addr_to_perm));
		ur_map_free(&(a->channel_to_ch_info));

		a->is_valid=0;
	}
}

ts_ur_session *get_relay_session(allocation *a)
{
	return &(a->relay_session);
}

ioa_socket_handle get_relay_socket(allocation *a)
{
	return a->relay_session.s;
}

void set_allocation_lifetime_ev(allocation *a, turn_time_t exp_time, ioa_timer_handle ev)
{
	if (a) {
		IOA_EVENT_DEL(a->lifetime_ev);
		a->expiration_time = exp_time;
		a->lifetime_ev = ev;
	}
}

int is_allocation_valid(const allocation* a) {
  if(a) return a->is_valid;
  else return 0;
}

void set_allocation_valid(allocation* a, int value) {
  if(a) a->is_valid=value;
}

turn_permission_info* allocation_get_permission(allocation* a, const ioa_addr *addr) {
  if(a) {
    return get_from_turn_permission_hashtable(&(a->addr_to_perm), addr);
  }
  return NULL;
}

///////////////////////////// TURN_PERMISSION /////////////////////////////////

static int delete_channel_info_from_allocation_map(ur_map_key_type key, ur_map_value_type value);

static void turn_permission_clean(turn_permission_info* tinfo)
{
	if (tinfo) {
		ur_map_foreach(tinfo->channels, (foreachcb_type) delete_channel_info_from_allocation_map);
		ur_map_free(&(tinfo->channels));
		IOA_EVENT_DEL(tinfo->lifetime_ev);
		ns_bzero(tinfo,sizeof(turn_permission_info));
	}
}

static void init_turn_permission_hashtable(turn_permission_hashtable *map)
{
	if (map)
		ns_bzero(map,sizeof(turn_permission_hashtable));
}

static void free_turn_permission_hashtable(turn_permission_hashtable *map)
{
	if(map) {
		size_t i;
		for(i=0;i<TURN_PERMISSION_HASHTABLE_SIZE;++i) {
			if(map->table[i].slots) {
				size_t j;
				for(j=0;j<map->table[i].sz;++j) {
					if(map->table[i].slots[j].allocated)
						turn_permission_clean(&(map->table[i].slots[j].info));
				}
				turn_free(map->table[i].slots, map->table[i].sz * sizeof(turn_permission_slot));
				map->table[i].slots = NULL;
			}
			map->table[i].sz = 0;
		}
	}
}

static turn_permission_info* get_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr *addr)
{
	if (!addr || !map)
		return NULL;

	u32bits index = addr_hash_no_port(addr) & TURN_PERMISSION_HASHTABLE_SIZE;
	turn_permission_array *parray = &(map->table[index]);

	if(!(parray->slots))
		return NULL;

	size_t i;
	size_t sz = parray->sz;
	for (i = 0; i < sz; ++i) {
		if (parray->slots[i].allocated && addr_eq_no_port(&(parray->slots[i].info.addr), addr)) {
			return &(parray->slots[i].info);
		}
	}

	return NULL;
}

static void remove_from_turn_permission_hashtable(turn_permission_hashtable *map, const ioa_addr* addr)
{
	if (!addr || !map)
		return;

	u32bits index = addr_hash_no_port(addr) & TURN_PERMISSION_HASHTABLE_SIZE;
	turn_permission_array *parray = &(map->table[index]);

	if(!(parray->slots))
		return;

	size_t i;
	size_t sz = parray->sz;
	for (i = 0; i < sz; ++i) {
		if (parray->slots[i].allocated && addr_eq_no_port(&(parray->slots[i].info.addr), addr)) {
			turn_permission_clean(&(parray->slots[i].info));
			parray->slots[i].allocated = 0;
		}
	}
}

static void ch_info_clean(ur_map_value_type value) {
  if(value) {
    ch_info* c = (ch_info*)value;
    IOA_EVENT_DEL(c->lifetime_ev);
    ns_bzero(c,sizeof(ch_info));
  }
}

static int delete_channel_info_from_allocation_map(ur_map_key_type key, ur_map_value_type value)
{
	UNUSED_ARG(key);

	if(value) {
		ch_info* chn = (ch_info*)value;
		turn_permission_info* tinfo = (turn_permission_info*)chn->owner;
		if(tinfo) {
			allocation* a = (allocation*)tinfo->owner;
			if(a) {
				ur_map_del(a->channel_to_ch_info, chn->chnum, ch_info_clean);
			}
		}
		turn_free(chn,sizeof(ch_info));
	}

	return 0;
}

void turn_channel_delete(ch_info* chn)
{
	if(chn) {
	  turn_permission_info* tinfo = (turn_permission_info*)chn->owner;
		if(tinfo) {
			ur_map_del(tinfo->channels, (ur_map_key_type)addr_get_port(&(chn->peer_addr)),NULL);
			delete_channel_info_from_allocation_map((ur_map_key_type)addr_get_port(&(chn->peer_addr)),(ur_map_value_type)chn);
		}
	}
}

void allocation_remove_turn_permission(allocation* a, turn_permission_info* tinfo)
{
	if (a && tinfo) {
		remove_from_turn_permission_hashtable(&(a->addr_to_perm), &(tinfo->addr));
	}
}

ch_info* allocation_get_new_ch_info(allocation* a, u16bits chnum, ioa_addr* peer_addr)
{

	turn_permission_info* tinfo = get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr);

	if (!tinfo)
		tinfo = allocation_add_permission(a, peer_addr);

	ch_info* chn = (ch_info*)turn_malloc(sizeof(ch_info));

	ns_bzero(chn,sizeof(ch_info));

	chn->chnum = chnum;
	chn->port = addr_get_port(peer_addr);
	addr_cpy(&(chn->peer_addr), peer_addr);
	chn->owner = tinfo;
	ur_map_put(a->channel_to_ch_info, chnum, chn);

	ur_map_put(tinfo->channels, (ur_map_key_type) addr_get_port(peer_addr), (ur_map_value_type) chn);

	return chn;
}

ch_info* allocation_get_ch_info(allocation* a, u16bits chnum) {
	void* vchn = NULL;
	if (ur_map_get(a->channel_to_ch_info, chnum, &vchn) && vchn) {
		return (ch_info*) vchn;
	}
	return NULL;
}

ch_info* allocation_get_ch_info_by_peer_addr(allocation* a, ioa_addr* peer_addr) {
	turn_permission_info* tinfo = get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr);
	if(tinfo) {
		return get_turn_channel(tinfo,peer_addr);
	}
	return NULL;
}

u16bits get_turn_channel_number(turn_permission_info* tinfo, ioa_addr *addr)
{
	if (tinfo) {
		ur_map_value_type t = 0;
		if (ur_map_get(tinfo->channels, (ur_map_key_type)addr_get_port(addr), &t) && t) {
			ch_info* chn = (ch_info*) t;
			if (STUN_VALID_CHANNEL(chn->chnum)) {
				return chn->chnum;
			}
		}
	}

	return 0;
}

ch_info *get_turn_channel(turn_permission_info* tinfo, ioa_addr *addr)
{
	if (tinfo) {
		ur_map_value_type t = 0;
		if (ur_map_get(tinfo->channels, (ur_map_key_type)addr_get_port(addr), &t) && t) {
			ch_info* chn = (ch_info*) t;
			if (STUN_VALID_CHANNEL(chn->chnum)) {
				return chn;
			}
		}
	}

	return NULL;
}

turn_permission_hashtable *allocation_get_turn_permission_hashtable(allocation *a)
{
  return &(a->addr_to_perm);
}

turn_permission_info* allocation_add_permission(allocation *a, const ioa_addr* addr)
{
	if (a && addr) {

		turn_permission_hashtable *map = &(a->addr_to_perm);
		u32bits hash = addr_hash_no_port(addr);
		size_t fds = (size_t) (hash & TURN_PERMISSION_HASHTABLE_SIZE);

		size_t old_sz = map->table[fds].sz * sizeof(turn_permission_slot);
		map->table[fds].slots = (turn_permission_slot *) turn_realloc(map->table[fds].slots,
						old_sz, old_sz + sizeof(turn_permission_slot));

		turn_permission_info *elem = &(map->table[fds].slots[old_sz].info);
		map->table[fds].sz = old_sz + 1;

		ns_bzero(elem,sizeof(turn_permission_info));
		elem->channels = ur_map_create();
		addr_cpy(&elem->addr, addr);

		elem->owner = a;

		map->table[fds].slots[old_sz].allocated = 1;

		return elem;
	} else {
		return NULL;
	}
}

////////////////// TCP connections ///////////////////////////////

static void set_new_tc_id(u08bits server_id, tcp_connection *tc) {
	allocation *a = (allocation*)(tc->owner);
	ur_map *map = a->tcp_connections;
	u32bits newid = 0;
	u32bits sid = server_id;
	sid = sid<<24;
	do {
		while (!newid) {
			newid = (u32bits)random();
			if(!newid) {
				continue;
			}
			newid = newid & 0x00FFFFFF;
			if(!newid) {
				continue;
			}
			newid = newid | sid;
		}
	} while(ur_map_get(map, (ur_map_key_type)newid, NULL));
	tc->id = newid;
	ur_map_put(map, (ur_map_key_type)newid, (ur_map_value_type)tc);
}

tcp_connection *create_tcp_connection(u08bits server_id, allocation *a, stun_tid *tid, ioa_addr *peer_addr, int *err_code)
{
	tcp_connection_list *tcl = &(a->tcl);
	while(tcl->next) {
		tcp_connection *otc = (tcp_connection*)(tcl->next);
		if(addr_eq(&(otc->peer_addr),peer_addr)) {
			*err_code = 446;
			return NULL;
		}
		tcl=tcl->next;
	}
	tcp_connection *tc = (tcp_connection*)turn_malloc(sizeof(tcp_connection));
	ns_bzero(tc,sizeof(tcp_connection));
	tcl->next = &(tc->list);
	addr_cpy(&(tc->peer_addr),peer_addr);
	if(tid)
		ns_bcopy(tid,&(tc->tid),sizeof(stun_tid));
	tc->owner = a;
	set_new_tc_id(server_id, tc);
	return tc;
}

void delete_tcp_connection(tcp_connection *tc)
{
	if(tc) {
		if(tc->done) {
			TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "!!! %s: check on already closed tcp data connection: 0x%lx\n",__FUNCTION__);
			return;
		}
		tc->done = 1;

		clear_unsent_buffer(&(tc->ub_to_client));

		IOA_EVENT_DEL(tc->peer_conn_timeout);
		IOA_EVENT_DEL(tc->conn_bind_timeout);
		allocation *a = (allocation*)(tc->owner);
		if(a) {
			ur_map *map = a->tcp_connections;
			if(map) {
				ur_map_del(map, (ur_map_key_type)(tc->id),NULL);
			}
			tcp_connection_list *tcl = &(a->tcl);
			while(tcl->next) {
				if((void*)(tcl->next) == (void*)tc) {
					tcl->next = tc->list.next;
					break;
				} else {
					tcl=tcl->next;
				}
			}
		}
		set_ioa_socket_sub_session(tc->client_s,NULL);
		IOA_CLOSE_SOCKET(tc->client_s);
		set_ioa_socket_sub_session(tc->peer_s,NULL);
		IOA_CLOSE_SOCKET(tc->peer_s);
		turn_free(tc,sizeof(tcp_connection));
	}
}

tcp_connection *get_and_clean_tcp_connection_by_id(ur_map *map, tcp_connection_id id)
{
	if(map) {
		ur_map_value_type t = 0;
		if (ur_map_get(map, (ur_map_key_type)id, &t) && t) {
			ur_map_del(map, (ur_map_key_type)id,NULL);
			return (tcp_connection*)t;
		}
	}
	return NULL;
}

tcp_connection *get_tcp_connection_by_peer(allocation *a, ioa_addr *peer_addr)
{
	if(a && peer_addr) {
		tcp_connection_list *tcl = &(a->tcl);
		while(tcl->next) {
			tcp_connection *tc = (tcp_connection*)(tcl->next);
			if(addr_eq(&(tc->peer_addr),peer_addr)) {
				return tc;
			}
			tcl=tcl->next;
		}
	}
	return NULL;
}

int can_accept_tcp_connection_from_peer(allocation *a, ioa_addr *peer_addr, int server_relay)
{
	if(server_relay)
		return 1;

	if(a && peer_addr) {
		return (get_from_turn_permission_hashtable(&(a->addr_to_perm), peer_addr) != NULL);
	}

	return 0;
}

//////////////// Unsent buffers //////////////////////

void clear_unsent_buffer(unsent_buffer *ub)
{
	if(ub) {
		if(ub->bufs) {
			size_t sz;
			for(sz = 0; sz<ub->sz; sz++) {
				ioa_network_buffer_handle nbh = ub->bufs[sz];
				if(nbh) {
					ioa_network_buffer_delete(NULL, nbh);
					ub->bufs[sz] = NULL;
				}
			}
			turn_free(ub->bufs,sizeof(ioa_network_buffer_handle) * ub->sz);
			ub->bufs = NULL;
		}
		ub->sz = 0;
	}
}

void add_unsent_buffer(unsent_buffer *ub, ioa_network_buffer_handle nbh)
{
	if(!ub || (ub->sz >= MAX_UNSENT_BUFFER_SIZE)) {
		ioa_network_buffer_delete(NULL, nbh);
	} else {
		ub->bufs = (ioa_network_buffer_handle*)turn_realloc(ub->bufs, sizeof(ioa_network_buffer_handle) * ub->sz, sizeof(ioa_network_buffer_handle) * (ub->sz+1));
		ub->bufs[ub->sz] = nbh;
		ub->sz +=1;
	}
}

ioa_network_buffer_handle top_unsent_buffer(unsent_buffer *ub)
{
	ioa_network_buffer_handle ret = NULL;
	if(ub && ub->bufs && ub->sz) {
		size_t sz;
		for(sz=0; sz<ub->sz; ++sz) {
			if(ub->bufs[sz]) {
				ret = ub->bufs[sz];
				break;
			}
		}
	}
	return ret;
}

void pop_unsent_buffer(unsent_buffer *ub)
{
	if(ub && ub->bufs && ub->sz) {
		size_t sz;
		for(sz=0; sz<ub->sz; ++sz) {
			if(ub->bufs[sz]) {
				ub->bufs[sz] = NULL;
				break;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////

