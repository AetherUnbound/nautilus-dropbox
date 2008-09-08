/*
 * Copyright 2008 Evenflow, Inc.
 *
 * dropbox-command-client.c
 * Implements connection handling and C interface for the Dropbox command socket.
 *
 * This file is part of nautilus-dropbox.
 *
 * nautilus-dropbox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nautilus-dropbox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nautilus-dropbox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>

#include <stdarg.h>
#include <string.h>

#include <glib.h>

#include "g-util.h"
#include "dropbox-client-util.h"
#include "dropbox-command-client.h"
#include "nautilus-dropbox-common.h"
#include "nautilus-dropbox.h"
#include "nautilus-dropbox-tray.h"
#include "nautilus-dropbox-hooks.h"

/* TODO: make this asynchronous ;) */

/*
  this is a tiny hack, necessitated by the fact that
  finish_file info command is in nautilus_dropbox,
  this can be cleaned up once the file_info_command isn't a special
  case anylonger
*/
gboolean nautilus_dropbox_finish_file_info_command(DropboxFileInfoCommandResponse *);

typedef struct {
  DropboxCommandClient *dcc;
  guint connect_attempt;
} ConnectionAttempt;

typedef struct {
  DropboxCommandClientConnectionAttemptHook h;
  gpointer ud;
} DropboxCommandClientConnectionAttempt;

typedef struct {
  DropboxGeneralCommand *dgc;
  GHashTable *response;
} DropboxGeneralCommandResponse;

static gboolean
on_connect(DropboxCommandClient *dcc) {
  g_hook_list_invoke(&(dcc->onconnect_hooklist), FALSE);
  return FALSE;
}

static gboolean
on_disconnect(DropboxCommandClient *dcc) {
  g_hook_list_invoke(&(dcc->ondisconnect_hooklist), FALSE);
  return FALSE;
}

static gboolean
on_connection_attempt(ConnectionAttempt *ca) {
  GList *ll;

  for (ll = ca->dcc->ca_hooklist; ll != NULL; ll = g_list_next(ll)) {
    DropboxCommandClientConnectionAttempt *dccca =
      (DropboxCommandClientConnectionAttempt *)(ll->data);
    dccca->h(ca->connect_attempt, dccca->ud);
  }

  g_free(ca);

  return FALSE;
}

static gboolean
receive_args_until_done(GIOChannel *chan, GHashTable *return_table,
			GError **err) {
  GIOStatus iostat;
  GError *tmp_error = NULL;
  guint numargs = 0;

  while (1) {
    gchar *line;
    gsize term_pos;

    /* if we are getting too many args, connection could be malicious */
    if (numargs >= 20) {
      g_set_error(err,
		  g_quark_from_static_string("malicious connection"),
		  0, "malicious connection");
      return FALSE;
    }
    
    /* get the string */
    iostat = g_io_channel_read_line(chan, &line, NULL,
				    &term_pos, &tmp_error);
    if (iostat == G_IO_STATUS_ERROR || tmp_error != NULL) {
      g_free(line);
      g_propagate_error(err, tmp_error);
      return FALSE;
    }
    else if (iostat == G_IO_STATUS_EOF) {
      g_free(line);
      g_set_error(err,
		  g_quark_from_static_string("connection closed"),
		  0, "connection closed");
      return FALSE;
    }

    *(line+term_pos) = '\0';

    if (strcmp("done", line) == 0) {
      g_free(line);
      break;
    }
    else {
      gboolean parse_result;

      parse_result = dropbox_client_util_command_parse_arg(line, return_table);
      g_free(line);

      if (FALSE == parse_result) {
	g_set_error(err,
		    g_quark_from_static_string("parse error"),
		    0, "parse error");
	return FALSE;
      }
    }
    
    numargs += 1;
  }

  return TRUE;
}

static void my_g_hash_table_get_keys_helper(gpointer key,
					    gpointer value,
					    GList **ud) {
  *ud = g_list_append(*ud, key);
}

static GList *my_g_hash_table_get_keys(GHashTable *ght) {
  GList *list = NULL;
  g_hash_table_foreach(ght, (GHFunc) 
		       my_g_hash_table_get_keys_helper, &list);
  return list;
}

/*
  sends a command to the dropbox server
  returns an hash of the return values

  in theory, this should disconnection errors
  but it doesn't matter right now, any error is a sufficient
  condition to disconnect
*/
static GHashTable *
send_command_to_db(GIOChannel *chan, const gchar *command_name,
		   GHashTable *args, GError **err) {
  GError *tmp_error = NULL;
  GIOStatus iostat;
  gsize bytes_trans;
  gchar *line;

  g_assert(chan != NULL);
  g_assert(command_name != NULL);
  

#define WRITE_OR_DIE_SANI(s,l) {					\
    gchar *sani_s;							\
    sani_s = dropbox_client_util_sanitize(s);				\
    iostat = g_io_channel_write_chars(chan, sani_s,l, &bytes_trans,	\
				      &tmp_error);			\
    g_free(sani_s);							\
    if (iostat == G_IO_STATUS_ERROR || tmp_error != NULL) {		\
      g_propagate_error(err, tmp_error);				\
      return NULL;							\
    }									\
  }
  
#define WRITE_OR_DIE(s,l) {						\
    iostat = g_io_channel_write_chars(chan, s,l, &bytes_trans,		\
				      &tmp_error);			\
    if (iostat == G_IO_STATUS_ERROR || tmp_error != NULL) {		\
      g_propagate_error(err, tmp_error);				\
      return NULL;							\
    }									\
  }
  
  /* send command to server */
  WRITE_OR_DIE_SANI(command_name, -1);
  WRITE_OR_DIE("\n", -1);

  if (args != NULL) {
    GList *keys, *li;

    /* oh god */
    keys = glib_check_version(2, 14, 0)
      ? my_g_hash_table_get_keys(args)
      : g_hash_table_get_keys(args);

    for (li = keys; li != NULL; li = g_list_next(li)) {
      int i;
      gchar **value;
      
      WRITE_OR_DIE_SANI((gchar *) li->data, -1);
      
      value = g_hash_table_lookup(args, li->data);
      for (i = 0; value[i] != NULL; i++) {
	WRITE_OR_DIE("\t", -1);
	WRITE_OR_DIE_SANI(value[i], -1);
      }
      WRITE_OR_DIE("\n", -1);
    }

    g_list_free(keys);
  }


  WRITE_OR_DIE("done\n", -1);

#undef WRITE_OR_DIE
#undef WRITE_OR_DIE_SANI

  g_io_channel_flush(chan, &tmp_error);
  if (tmp_error != NULL) {
    g_propagate_error(err, tmp_error);
    return NULL;
  }

  /* now we have to read the data */
  iostat = g_io_channel_read_line(chan, &line, NULL,
				  NULL, &tmp_error);
  if (iostat == G_IO_STATUS_ERROR || tmp_error != NULL) {
    if (line != NULL) g_free(line);
    g_propagate_error(err, tmp_error);
    return NULL;
  }
  else if (iostat == G_IO_STATUS_EOF) {
    if (line != NULL) g_free(line);
    g_set_error(err,
		g_quark_from_static_string("dropbox command connection closed"),
		0,
		"dropbox command connection closed");
    return NULL;
  }

  /* if the response was okay */
  if (strncmp(line, "ok\n", 3) == 0) {
    GHashTable *return_table = 
      g_hash_table_new_full((GHashFunc) g_str_hash,
			    (GEqualFunc) g_str_equal,
			    (GDestroyNotify) g_free,
			    (GDestroyNotify) g_strfreev);
    
    g_free(line);

    receive_args_until_done(chan, return_table, &tmp_error);
    if (tmp_error != NULL) {
      g_hash_table_destroy(return_table);
      g_propagate_error(err, tmp_error);
      return NULL;
    }
      
    return return_table;
  }
  /* otherwise */
  else {
    g_free(line);

    /* read errors off until we get done */
    do {
      /* clear string */
      iostat = g_io_channel_read_line(chan, &line, NULL,
				      NULL, &tmp_error);
      if (iostat == G_IO_STATUS_ERROR ||
	  tmp_error != NULL) {
	g_free(line);
	g_propagate_error(err, tmp_error);
	return NULL;
      }
      else if (iostat == G_IO_STATUS_EOF) {
	g_free(line);
	g_set_error(err,
		    g_quark_from_static_string("dropbox command connection closed"),
		    0,
		    "dropbox command connection closed");
	return NULL;
      }

      /* we got our line */
    } while (strncmp(line, "done\n", 5) != 0);

    g_free(line);
    return NULL;
  }
}

static void
do_file_info_command(GIOChannel *chan, DropboxFileInfoCommand *dfic,
		     GError **gerr) {
  /* we need to send two requests to dropbox:
     file status, and context options */
  GError *tmp_gerr = NULL;
  DropboxFileInfoCommandResponse *dficr;
  GHashTable *file_status_response = NULL,
    *context_options_response = NULL, *args, *folder_tag_response = NULL;
  gchar *filename;

  /* TODO: might be thread-unsafe */
  filename = g_filename_from_uri(nautilus_file_info_get_uri(dfic->file),
				 NULL, NULL);

  args = g_hash_table_new_full((GHashFunc) g_str_hash,
			       (GEqualFunc) g_str_equal,
			       (GDestroyNotify) g_free,
			       (GDestroyNotify) g_strfreev);
  {
    gchar **path_arg;
    path_arg = g_new(gchar *, 2);
    path_arg[0] = g_strdup(filename);
    path_arg[1] = NULL;
    g_hash_table_insert(args, g_strdup("path"), path_arg);
  }

  
  /* send status command to server */
  file_status_response = send_command_to_db(chan, "icon_overlay_file_status",
					    args, &tmp_gerr);
  g_hash_table_unref(args);
  args = NULL;
  if (tmp_gerr != NULL) {
    g_free(filename);
    g_assert(file_status_response == NULL);
    g_propagate_error(gerr, tmp_gerr);
    return;
  }

  args = g_hash_table_new_full((GHashFunc) g_str_hash,
			       (GEqualFunc) g_str_equal,
			       (GDestroyNotify) g_free,
			       (GDestroyNotify) g_strfreev);
  {
    gchar **paths_arg;
    paths_arg = g_new(gchar *, 2);
    paths_arg[0] = g_strdup(filename);
    paths_arg[1] = NULL;
    g_hash_table_insert(args, g_strdup("paths"), paths_arg);
  }

  /* send context options command to server */
  context_options_response =
    send_command_to_db(chan, "icon_overlay_context_options",
		       args, &tmp_gerr);
  g_hash_table_unref(args);
  args = NULL;
  if (tmp_gerr != NULL) {
    if (file_status_response != NULL)
      g_hash_table_destroy(file_status_response);
    g_assert(context_options_response == NULL);
    g_propagate_error(gerr, tmp_gerr);
    return;
  }

  if (nautilus_file_info_is_directory(dfic->file)) {
    args = g_hash_table_new_full((GHashFunc) g_str_hash,
				 (GEqualFunc) g_str_equal,
				 (GDestroyNotify) g_free,
				 (GDestroyNotify) g_strfreev);
    {
      gchar **paths_arg;
      paths_arg = g_new(gchar *, 2);
      paths_arg[0] = g_strdup(filename);
      paths_arg[1] = NULL;
      g_hash_table_insert(args, g_strdup("path"), paths_arg);
    }
    
    folder_tag_response =
      send_command_to_db(chan, "get_folder_tag", args, &tmp_gerr);
    g_hash_table_unref(args);
    args = NULL;
    if (tmp_gerr != NULL) {
      if (file_status_response != NULL)
	g_hash_table_destroy(file_status_response);
      if (context_options_response != NULL)
	g_hash_table_destroy(context_options_response);      
      g_assert(folder_tag_response == NULL);
      g_propagate_error(gerr, tmp_gerr);
      return;
    }
  }
  
  /* great server responded perfectly,
     now let's get this request done,
     ...in the glib main loop */
  dficr = g_new0(DropboxFileInfoCommandResponse, 1);
  dficr->dfic = dfic;
  dficr->folder_tag_response = folder_tag_response;
  dficr->file_status_response = file_status_response;
  dficr->context_options_response = context_options_response;
  g_idle_add((GSourceFunc) nautilus_dropbox_finish_file_info_command, dficr);

  g_free(filename);
  
  return;
}

static gboolean
finish_general_command(DropboxGeneralCommandResponse *dgcr) {
  if (dgcr->dgc->handler != NULL) {
    dgcr->dgc->handler(dgcr->response, dgcr->dgc->handler_ud);
  }
  
  if (dgcr->response != NULL) {
    g_hash_table_destroy(dgcr->response);
  }

  g_free(dgcr->dgc->command_name);
  if (dgcr->dgc->command_args != NULL) {
    g_hash_table_unref(dgcr->dgc->command_args);
  }
  g_free(dgcr->dgc);
  g_free(dgcr);
  
  return FALSE;
}

static void
do_general_command(GIOChannel *chan, DropboxGeneralCommand *dcac,
		   GError **gerr) {
  GError *tmp_gerr = NULL;
  GHashTable *response;

  /* send status command to server */
  response = send_command_to_db(chan, dcac->command_name,
				dcac->command_args, &tmp_gerr);
  if (tmp_gerr != NULL) {
    g_assert(response == NULL);
    g_propagate_error(gerr, tmp_gerr);
    return;
  }

  /* great, the server did the command perfectly,
     now call the handler with the response */
  {
    DropboxGeneralCommandResponse *dgcr = g_new0(DropboxGeneralCommandResponse, 1);
    dgcr->dgc = dcac;
    dgcr->response = response;
    g_idle_add((GSourceFunc) finish_general_command, dgcr);
  }
  
  return;
}

static gboolean
check_connection(GIOChannel *chan) {
  gchar fake_buf[4096];
  gsize bytes_read;
  GError *tmp_error = NULL;
  GIOFlags flags;
  GIOStatus iostat;

  flags = g_io_channel_get_flags(chan);
  
  /* set non-blocking */
  g_io_channel_set_flags(chan, flags | G_IO_FLAG_NONBLOCK,
			 &tmp_error);
  if (tmp_error != NULL) {
    g_error_free(tmp_error);
    return FALSE;
  }
  
  iostat = g_io_channel_read_chars(chan, fake_buf,
				   sizeof(fake_buf), 
				   &bytes_read, &tmp_error);
  g_io_channel_set_flags(chan, flags, NULL);
  if (tmp_error != NULL) {
    g_error_free(tmp_error);
    return FALSE;
  }

  /* this makes us disconnect from bad servers
     (those that send us information without us asking for it) */
  if (iostat == G_IO_STATUS_AGAIN) {
    return TRUE;
  }
  else {
    return FALSE;
  }
}

static gpointer
dropbox_command_client_thread(DropboxCommandClient *data);

static void
end_request(DropboxCommand *dc) {
  if ((gpointer (*)(DropboxCommandClient *data)) dc != &dropbox_command_client_thread) {
    switch (dc->request_type) {
    case GET_FILE_INFO: {
      DropboxFileInfoCommand *dfic = (DropboxFileInfoCommand *) dc;
      DropboxFileInfoCommandResponse *dficr = g_new0(DropboxFileInfoCommandResponse, 1);
      dficr->dfic = dfic;
      dficr->file_status_response = NULL;
      dficr->context_options_response = NULL;
      g_idle_add((GSourceFunc) nautilus_dropbox_finish_file_info_command, dficr);
    }
      break;
    case GENERAL_COMMAND: {
      DropboxGeneralCommand *dgc = (DropboxGeneralCommand *) dc;
      DropboxGeneralCommandResponse *dgcr = g_new0(DropboxGeneralCommandResponse, 1);
      dgcr->dgc = dgc;
      dgcr->response = NULL;
      g_idle_add((GSourceFunc) finish_general_command, dgcr);
    }
      break;
    default: 
      g_assert_not_reached();
      break;
    }
  }
}

static gpointer
dropbox_command_client_thread(DropboxCommandClient *dcc) {
  struct sockaddr_un addr;
  socklen_t addr_len;

  /* intialize address structure */
  addr.sun_family = AF_UNIX;
  g_snprintf(addr.sun_path,
	     sizeof(addr.sun_path),
#ifdef ND_DEBUG
	     "%s/.dropboxlocal/command_socket",
#else
	     "%s/.dropbox/command_socket",
#endif
	     g_get_home_dir());
  addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path);

  while (1) {
    GIOChannel *chan = NULL;
    GError *gerr = NULL;
    int sock;
    int i;

    sock = socket(PF_UNIX, SOCK_STREAM, 0);

    for (i = 1; ;i++) {
      /* first we have to connect to the dropbox command server */
      if (-1 == connect(sock, (struct sockaddr *) &addr, addr_len)) {
	ConnectionAttempt *ca = g_new(ConnectionAttempt, 1);
	ca->dcc = dcc;
	ca->connect_attempt = i;
	g_idle_add((GSourceFunc) on_connection_attempt, ca);
	g_usleep(G_USEC_PER_SEC);
      }
      else break;
    }

    /* connected */
    chan = g_io_channel_unix_new(sock);
    g_io_channel_set_close_on_unref(chan, TRUE);
    g_io_channel_set_line_term(chan, "\n", -1);

#define SET_CONNECTED_STATE(s)     {			\
      g_mutex_lock(dcc->command_connected_mutex);	\
      dcc->command_connected = s;			\
      g_mutex_unlock(dcc->command_connected_mutex);	\
    }
    
    SET_CONNECTED_STATE(TRUE);

    g_idle_add((GSourceFunc) on_connect, dcc);

    while (1) {
      DropboxCommand *dc;
      
      while (1) {
	GTimeVal gtv;

	g_get_current_time(&gtv);
	g_time_val_add(&gtv, G_USEC_PER_SEC / 10);
	/* get a request from nautilus */
	dc = g_async_queue_timed_pop(dcc->command_queue, &gtv);
	if (dc != NULL) {
	  break;
	}
	else {
	  if (check_connection(chan) == FALSE) {
	    goto BADCONNECTION;
	  }
	}
      }

      /* why this insures lexical module safety should be obvious,
	 (this function is static)
       */
      if ((gpointer (*)(DropboxCommandClient *data)) dc == &dropbox_command_client_thread) {
	debug("got a reset request");
	goto BADCONNECTION;
      }

      switch (dc->request_type) {
      case GET_FILE_INFO: {
	do_file_info_command(chan, (DropboxFileInfoCommand *) dc, &gerr);
      }
	break;
      case GENERAL_COMMAND: {
	do_general_command(chan, (DropboxGeneralCommand *) dc, &gerr);
      }
	break;
      default: 
	g_assert_not_reached();
	break;
      }
      
      if (gerr != NULL) {
	/* mark this request as never to be completed */
	end_request(dc);
	
	debug("command error: %s", gerr->message);
	
	g_error_free(gerr);
      BADCONNECTION:
	/* grab all the rest of the data off the async queue and mark it
	   never to be completed, who knows how long we'll be disconnected */
	while ((dc = g_async_queue_try_pop(dcc->command_queue)) != NULL) {
	  end_request(dc);
	}

	g_io_channel_unref(chan);

	SET_CONNECTED_STATE(FALSE);

	/* call the disconnect handler */
	g_idle_add((GSourceFunc) on_disconnect, dcc);

	break;
      }
    }
    
#undef SET_CONNECTED_STATE
  }
  
  return NULL;
}

/* thread safe */
gboolean
dropbox_command_client_is_connected(DropboxCommandClient *dcc) {
  gboolean command_connected;

  g_mutex_lock(dcc->command_connected_mutex);
  command_connected = dcc->command_connected;
  g_mutex_unlock(dcc->command_connected_mutex);

  return command_connected;
}

/* thread safe */
void dropbox_command_client_force_reconnect(DropboxCommandClient *dcc) {
  if (dropbox_command_client_is_connected(dcc) == TRUE) {
    debug("forcing command to reconnect");
    dropbox_command_client_request(dcc, (DropboxCommand *) &dropbox_command_client_thread);
  }
}

/* thread safe */
void
dropbox_command_client_request(DropboxCommandClient *dcc, DropboxCommand *dc) {
  g_async_queue_push(dcc->command_queue, dc);
}

/* should only be called once on initialization */
void
dropbox_command_client_setup(DropboxCommandClient *dcc) {
  dcc->command_queue = g_async_queue_new();
  dcc->command_connected_mutex = g_mutex_new();
  dcc->command_connected = FALSE;
  dcc->ca_hooklist = NULL;

  g_hook_list_init(&(dcc->ondisconnect_hooklist), sizeof(GHook));
  g_hook_list_init(&(dcc->onconnect_hooklist), sizeof(GHook));
}

void
dropbox_command_client_add_on_disconnect_hook(DropboxCommandClient *dcc,
					      DropboxCommandClientConnectHook dhcch,
					      gpointer ud) {
  GHook *newhook;
  
  newhook = g_hook_alloc(&(dcc->ondisconnect_hooklist));
  newhook->func = dhcch;
  newhook->data = ud;
  
  g_hook_append(&(dcc->ondisconnect_hooklist), newhook);
}

void
dropbox_command_client_add_on_connect_hook(DropboxCommandClient *dcc,
					   DropboxCommandClientConnectHook dhcch,
					   gpointer ud) {
  GHook *newhook;
  
  newhook = g_hook_alloc(&(dcc->onconnect_hooklist));
  newhook->func = dhcch;
  newhook->data = ud;
  
  g_hook_append(&(dcc->onconnect_hooklist), newhook);
}

void
dropbox_command_client_add_connection_attempt_hook(DropboxCommandClient *dcc,
						   DropboxCommandClientConnectionAttemptHook dhcch,
						   gpointer ud) {
  DropboxCommandClientConnectionAttempt *newhook;
  
  newhook = g_new(DropboxCommandClientConnectionAttempt, 1);
  newhook->h = dhcch;
  newhook->ud = ud;

  dcc->ca_hooklist = g_list_append(dcc->ca_hooklist, newhook);
}

/* should only be called once on initialization */
void
dropbox_command_client_start(DropboxCommandClient *dcc) {
  /* setup the connect to the command server */
  g_thread_create((gpointer (*)(gpointer data)) dropbox_command_client_thread,
		  dcc, FALSE, NULL);
}

/* thread safe */
void dropbox_command_client_send_simple_command(DropboxCommandClient *dcc, 
					 const char *command) {
  DropboxGeneralCommand *dgc;
  
  dgc = g_new(DropboxGeneralCommand, 1);
  
  dgc->dc.request_type = GENERAL_COMMAND;
  dgc->command_name = g_strdup(command);
  dgc->command_args = NULL;
  dgc->handler = NULL;
  dgc->handler_ud = NULL;
  
  dropbox_command_client_request(dcc, (DropboxCommand *) dgc);
}

/* thread safe */
/* this is the C API, there is another send_command_to_db
   that is more the actual over the wire command */
void dropbox_command_client_send_command(DropboxCommandClient *dcc, 
					 NautilusDropboxCommandResponseHandler h,
					 gpointer ud,
					 const char *command, ...) {
  va_list ap;
  DropboxGeneralCommand *dgc;
  gchar *na;
  
  dgc = g_new(DropboxGeneralCommand, 1);
  dgc->dc.request_type = GENERAL_COMMAND;
  dgc->command_name = g_strdup(command);
  dgc->command_args = g_hash_table_new_full((GHashFunc) g_str_hash,
					    (GEqualFunc) g_str_equal,
					    (GDestroyNotify) g_free,
					    (GDestroyNotify) g_strfreev);
  dgc->handler = h;
  dgc->handler_ud = ud;
  
  va_start(ap, command);
  while ((na = va_arg(ap, char *)) != NULL) {
    gchar **is_active_arg;

    is_active_arg = g_new(gchar *, 2);

    g_hash_table_insert(dgc->command_args,
			g_strdup(na), is_active_arg);

    is_active_arg[0] = g_strdup(va_arg(ap, char *));
    is_active_arg[1] = NULL;
  }
  va_end(ap);

  dropbox_command_client_request(dcc, (DropboxCommand *) dgc);
}
