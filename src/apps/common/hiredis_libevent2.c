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

#include <stdlib.h>
#include <stdarg.h>

#if !defined(TURN_NO_HIREDIS)

#include "hiredis_libevent2.h"
#include "ns_turn_utils.h"

#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include <hiredis/hiredis.h>
#include <hiredis/async.h>

//////////////// Libevent context ///////////////////////

struct redisLibeventEvents
{
	redisAsyncContext *context;
	int invalid;
	int allocated;
	struct event_base *base;
	struct event *rev, *wev;
	int rev_set, wev_set;
	char *ip;
	int port;
	char *pwd;
	int db;
};

///////////// Messages ////////////////////////////

struct redis_message
{
	char format[513];
	char arg[513];
};

/////////////////// forward declarations ///////////////

static void redis_reconnect(struct redisLibeventEvents *e);

/////////////////// Callbacks ////////////////////////////

static void redisLibeventReadEvent(int fd, short event, void *arg) {
  ((void)fd); ((void)event);
  struct redisLibeventEvents *e = (struct redisLibeventEvents*)arg;
  if(e && !(e->invalid)) {
	  {
		  char buf[8];
		  int len = 0;
		  do {
			  len = recv(fd,buf,sizeof(buf),MSG_PEEK);
		  } while((len<0)&&(errno == EINTR));
		  if(len<1) {
			  e->invalid = 1;
			  TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Redis connection broken: e=0x%lx\n", __FUNCTION__, (unsigned long)e);
		  }
	  }
	  if(!(e->invalid)) {
		  redisAsyncHandleRead(e->context);
	  }
  }
}

static void redisLibeventWriteEvent(int fd, short event, void *arg) {
  ((void)fd); ((void)event);
  struct redisLibeventEvents *e = (struct redisLibeventEvents*)arg;
  if(e && !(e->invalid)) {
    redisAsyncHandleWrite(e->context);
  }
}

static void redisLibeventAddRead(void *privdata) {
  struct redisLibeventEvents *e = (struct redisLibeventEvents*)privdata;
  if(e && (e->rev)) {
    event_add(e->rev,NULL);
    e->rev_set = 1;
  }
}

static void redisLibeventDelRead(void *privdata) {
    struct redisLibeventEvents *e = (struct redisLibeventEvents*)privdata;
    if(e && e->rev) {
      event_del(e->rev);
      e->rev_set = 0;
    }
}

static void redisLibeventAddWrite(void *privdata) {
    struct redisLibeventEvents *e = (struct redisLibeventEvents*)privdata;
    if(e && (e->wev)) {
      event_add(e->wev,NULL);
      e->wev_set = 1;
    }
}

static void redisLibeventDelWrite(void *privdata) {
  struct redisLibeventEvents *e = (struct redisLibeventEvents*)privdata;
  if(e && e->wev) {
    event_del(e->wev);
    e->wev_set = 0;
  }
}

static void redisLibeventCleanup(void *privdata)
{

	if (privdata) {

		struct redisLibeventEvents *e = (struct redisLibeventEvents *) privdata;
		if (e->allocated) {
			e->allocated = 0;
			if (e->rev) {
				if(e->rev_set)
					event_del(e->rev);
				event_free(e->rev);
				e->rev = NULL;
			}
			if (e->wev) {
				if(e->wev_set)
					event_del(e->wev);
				event_free(e->wev);
				e->wev = NULL;
			}
			turn_free(privdata, sizeof(struct redisLibeventEvents));
		}
	}
}

///////////////////////// Send-receive ///////////////////////////

void redis_async_init(void)
{
	;
}

int is_redis_asyncconn_good(redis_context_handle rch)
{
	if(rch) {
		struct redisLibeventEvents *e = (struct redisLibeventEvents*)rch;
		if(!(e->invalid))
			return 1;
	}
	return 0;
}

void send_message_to_redis(redis_context_handle rch, const char *command, const char *key, const char *format,...)
{
	if(!rch) {
		return;
	} else {

		struct redisLibeventEvents *e = (struct redisLibeventEvents*)rch;

		if(e->invalid || !(e->context) || e->context->err || e->context->c.err)
			redis_reconnect(e);

		if(!(e->invalid)) {

			redisAsyncContext *ac=e->context;

			struct redis_message rm;

			snprintf(rm.format,sizeof(rm.format)-3,"%s %s ", command, key);
			strcpy(rm.format+strlen(rm.format),"%s");

			va_list args;
			va_start (args, format);
			vsnprintf(rm.arg, sizeof(rm.arg)-1, format, args);
			va_end (args);

			if((redisAsyncCommand(ac, NULL, e, rm.format, rm.arg)!=REDIS_OK) || (ac->err) || (ac->c.err)) {
				e->invalid = 1;
				TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Redis connection broken: ac=0x%lx, e=0x%x\n", __FUNCTION__,(unsigned long)ac,(unsigned long)e);
			}
		}
	}
}

static void deleteKeysCallback(redisAsyncContext *c, void *reply0, void *privdata)
{
	redisReply *reply = (redisReply*) reply0;

	if (reply) {

		if (reply->type == REDIS_REPLY_ERROR)
			fprintf(stderr,"Error: %s\n", reply->str);
		else if (reply->type != REDIS_REPLY_ARRAY)
			fprintf(stderr,"Unexpected type: %d\n", reply->type);
		else {
			size_t i;
			for (i = 0; (i < reply->elements) && !(c->err); ++i) {
				redisAsyncCommand(c, NULL, privdata, "del %s", reply->element[i]->str);
			}
		}
	}
}

static void delete_redis_keys(redis_context_handle rch, const char *key_pattern)
{
	struct redisLibeventEvents *e = (struct redisLibeventEvents*)rch;
	if(e && !(e->invalid)) {
		redisAsyncContext *ac=e->context;
		if(ac && !(ac->err) && !(ac->c.err)) {
			redisAsyncCommand(ac, deleteKeysCallback, ac->ev.data, "keys %s", key_pattern);
		}
	}
}

void turn_report_allocation_delete_all(redis_context_handle rch)
{
	delete_redis_keys(rch, "turn/user/*/allocation/*/status");
	delete_redis_keys(rch, "turn/realm/*/user/*/allocation/*/status");
}

///////////////////////// Attach /////////////////////////////////

static void redisDisconnectCallbackFunc(const struct redisAsyncContext* ac, int status)
{
	UNUSED_ARG(status);
	if(ac) {
		struct redisLibeventEvents *e = ac->ev.data;
		if(e) {
			e->invalid = 1;
		}
	}
}

redis_context_handle redisLibeventAttach(struct event_base *base, char *ip0, int port0, char *pwd, int db)
{

  struct redisLibeventEvents *e = NULL;
  redisAsyncContext *ac = NULL;

  char ip[256];
  if(ip0 && ip0[0])
	  STRCPY(ip,ip0);
  else
	  STRCPY(ip,"127.0.0.1");

  int port = DEFAULT_REDIS_PORT;
  if(port0>0)
	  port=port0;

  ac = redisAsyncConnect(ip, port);
  if (!ac || (ac->err) || (ac->c.err)) {
  	fprintf(stderr,"Error: %s:%s\n", ac->errstr, ac->c.errstr);
  	return NULL;
  }

  /* Create container for context and r/w events */
  e = (struct redisLibeventEvents*)turn_malloc(sizeof(struct redisLibeventEvents));
  ns_bzero(e,sizeof(struct redisLibeventEvents));

  e->allocated = 1;
  e->context = ac;
  e->base = base;
  e->ip = strdup(ip);
  e->port = port;
  if(pwd)
	  e->pwd = strdup(pwd);
  e->db = db;

  /* Register functions to start/stop listening for events */
  ac->ev.addRead = redisLibeventAddRead;
  ac->ev.delRead = redisLibeventDelRead;
  ac->ev.addWrite = redisLibeventAddWrite;
  ac->ev.delWrite = redisLibeventDelWrite;
  ac->ev.cleanup = redisLibeventCleanup;

  ac->ev.data = e;

  /* Initialize and install read/write events */
  e->rev = event_new(e->base,e->context->c.fd,
  		     EV_READ,redisLibeventReadEvent,
  		     e);

  e->wev = event_new(e->base,e->context->c.fd,
		     EV_WRITE,redisLibeventWriteEvent,
  		     e);

  if (e->rev == NULL || e->wev == NULL) {
	  turn_free(e, sizeof(struct redisLibeventEvents));
	  return NULL;
  }
  
  event_add(e->wev, NULL);
  e->wev_set = 1;

  redisAsyncSetDisconnectCallback(ac, redisDisconnectCallbackFunc);

  {
  	  redisContext* redisconnection = redisConnect(ip, port);
  	  if(!redisconnection) {
  		  e->invalid = 1;
  		  TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot connect to redis (async, 1)\n", __FUNCTION__);
  	  } else {
  		  void *reply = redisCommand(redisconnection, "keys turn/secret/*");
  		  if(reply) {
  			  freeReplyObject(reply);
  		  } else {
  			  e->invalid = 1;
  			  TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot connect to redis (async, 2)\n", __FUNCTION__);
  		  }
  		  redisFree(redisconnection);
  	  }
  }

  //Authentication
  if(!(e->invalid) && pwd) {
	  if((redisAsyncCommand(ac, NULL, e, "AUTH %s", pwd)<0) || (ac->err)|| (ac->c.err)) {
		  e->invalid = 1;
	  }
  }

  if(!(e->invalid)) {
	  if((redisAsyncCommand(ac, NULL, e, "SELECT %d", db)<0) || (ac->err)|| (ac->c.err)) {
		  e->invalid = 1;
	  }
  }

  return (redis_context_handle)e;
}

static void redis_reconnect(struct redisLibeventEvents *e)
{
  if(!e || !(e->allocated))
	  return;

  redisAsyncContext *ac = NULL;

  ac = redisAsyncConnect(e->ip, e->port);
  if(!ac)
	  return;

  if (ac->err || (ac->c.err))
  	return;

  e->context = ac;

  /* Register functions to start/stop listening for events */
  ac->ev.addRead = redisLibeventAddRead;
  ac->ev.delRead = redisLibeventDelRead;
  ac->ev.addWrite = redisLibeventAddWrite;
  ac->ev.delWrite = redisLibeventDelWrite;
  ac->ev.cleanup = redisLibeventCleanup;

  ac->ev.data = e;

  if (e->rev) {
  	if(e->rev_set)
  		event_del(e->rev);
  	event_free(e->rev);
  	e->rev = NULL;
  }
  if (e->wev) {
  	if(e->wev_set)
  		event_del(e->wev);
  	event_free(e->wev);
  	e->wev = NULL;
  }

  /* Initialize and install read/write events */
  e->rev = event_new(e->base,e->context->c.fd,
  		     EV_READ,redisLibeventReadEvent,
  		     e);

  e->wev = event_new(e->base,e->context->c.fd,
		     EV_WRITE,redisLibeventWriteEvent,
  		     e);

  if (e->rev == NULL || e->wev == NULL) {
	  return;
  }

  event_add(e->wev, NULL);
  e->wev_set = 1;

  redisAsyncSetDisconnectCallback(ac, redisDisconnectCallbackFunc);

  {
  	  redisContext* redisconnection = redisConnect(e->ip, e->port);
  	  if(!redisconnection) {
  		  e->invalid = 1;
  		  TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot connect to redis (async, 1)\n", __FUNCTION__);
  	  } else {
  		  void *reply = redisCommand(redisconnection, "keys turn/secret/*");
  		  if(reply) {
  			  freeReplyObject(reply);
  			  e->invalid = 0;
  		  } else {
  			  e->invalid = 1;
  			  TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "Cannot connect to redis (async, 2)\n", __FUNCTION__);
  		  }
  		  redisFree(redisconnection);
  	  }
  }

  //Authentication
  if(!(e->invalid) && e->pwd) {
	  if((redisAsyncCommand(ac, NULL, e, "AUTH %s", e->pwd)<0) || (ac->err)|| (ac->c.err)) {
		  e->invalid = 1;
	  }
  }

  if(!(e->invalid)) {
	  if((redisAsyncCommand(ac, NULL, e, "SELECT %d", e->db)<0) || (ac->err)|| (ac->c.err)) {
		  e->invalid = 1;
	  }
  }

  if(!(e->invalid)) {
	  TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "Re-connected to redis, async\n", __FUNCTION__);
  }
}

/////////////////////////////////////////////////////////

#endif
/* TURN_NO_HIREDIS */

