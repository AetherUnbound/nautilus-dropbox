/*
 * Copyright 2008 Evenflow, Inc.
 *
 * nautilus-dropbox.c
 * Implements the Nautilus extension API for Dropbox. 
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

#ifdef HAVE_CONFIG_H
#include <config.h> /* for GETTEXT_PACKAGE */
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>

#include <libnautilus-extension/nautilus-extension-types.h>
#include <libnautilus-extension/nautilus-menu-provider.h>
#include <libnautilus-extension/nautilus-info-provider.h>

#include "g-util.h"
#include "dropbox-command-client.h"
#include "nautilus-dropbox-common.h"
#include "nautilus-dropbox.h"
#include "nautilus-dropbox-hooks.h"
#include "nautilus-dropbox-tray.h"

typedef struct {
  gchar *title;
  gchar *tooltip;
  gchar *verb;
} DropboxContextMenuItem;

static char *emblems[] = {"dropbox-uptodate", "dropbox-syncing"};

gboolean dropbox_use_nautilus_submenu_workaround;
gboolean dropbox_use_operation_in_progress_workaround;

static GType dropbox_type = 0;

/* probably my favorite function */
static gchar *
canonicalize_path(gchar *path) {
  int i, j = 0;
  gchar *toret, **cpy, **elts;
  
  g_assert(path != NULL);
  g_assert(path[0] == '/');

  elts = g_strsplit(path, "/", 0);
  cpy = g_new(gchar *, g_strv_length(elts)+1);
  cpy[j++] = "/";
  for (i = 0; elts[i] != NULL; i++) {
    if (strcmp(elts[i], "..") == 0) {
      j--;
    }
    else if (strcmp(elts[i], ".") != 0 && elts[i][0] != '\0') {
      cpy[j++] = elts[i];
    }
  }
  
  cpy[j] = NULL;
  toret = g_build_filenamev(cpy);
  g_free(cpy);
  g_strfreev(elts);
  
  return toret;
}

static void
menu_item_free(gpointer data) {
  DropboxContextMenuItem *dcmi = (DropboxContextMenuItem *) data;
  g_free(dcmi->title);
  g_free(dcmi->tooltip);
  g_free(dcmi->verb);
  g_free(dcmi);
}

static void
reset_file(NautilusFileInfo *file) {
  nautilus_file_info_invalidate_extension_info(file);

  g_object_set_data(G_OBJECT(file),
		    "nautilus_dropbox_menu_item", NULL);
}

static void
test_cb(NautilusFileInfo *file, NautilusDropbox *cvs) {
  /* check if this file's path has changed, if so update the hash and invalidate
     the file */
  gchar *filename, *pfilename;
  gchar *filename2;
  
  pfilename = g_filename_from_uri(nautilus_file_info_get_uri(file), NULL, NULL);
  filename = canonicalize_path(pfilename);
  g_free(pfilename);
  filename2 =  g_hash_table_lookup(cvs->obj2filename, file);

  /* if filename2 is NULL we've never seen this file in update_file_info */
  if (filename2 == NULL) {
    g_free(filename);
    return;
  }

  /* this is a hack, because nautilus doesn't do this for us, for some reason
     the file's path has changed */
  if (strcmp(filename, filename2) != 0) {
    debug("shifty old: %s, new %s", filename2, filename);

    /* gotta do this first, the call after this frees filename2 */
    g_hash_table_remove(cvs->filename2obj, filename2);

    g_hash_table_replace(cvs->obj2filename, file, g_strdup(filename));

    {
      NautilusFileInfo *f2;
      /* we shouldn't have another mapping from filename to an object */
      f2 = g_hash_table_lookup(cvs->filename2obj, filename);
      if (f2 != NULL) {
	/* lets fix it if it's true, just remove the mapping */
	g_hash_table_remove(cvs->filename2obj, filename);
	g_hash_table_remove(cvs->obj2filename, f2);
      }
    }

    g_hash_table_insert(cvs->filename2obj, g_strdup(filename), file);
    reset_file(file);
  }
  
  g_free(filename);
}

static void
when_file_dies(NautilusDropbox *cvs, NautilusFileInfo *address) {
  gchar *filename;

  filename = g_hash_table_lookup(cvs->obj2filename, address);
  
  /* we never got a change to view this file */
  if (filename == NULL) {
    return;
  }

  /* too chatty */
  /*  debug("removing %s <-> 0x%p", filename, address); */

  g_hash_table_remove(cvs->filename2obj, filename);
  g_hash_table_remove(cvs->obj2filename, address);
}

static NautilusOperationResult
nautilus_dropbox_update_file_info(NautilusInfoProvider     *provider,
                                  NautilusFileInfo         *file,
                                  GClosure                 *update_complete,
                                  NautilusOperationHandle **handle) {
  NautilusDropbox *cvs;

  cvs = NAUTILUS_DROPBOX(provider);

  /* this code adds this file object to our two-way hash of file objects
     so we can shell touch these files later */
  {
    gchar *pfilename;

    pfilename = g_filename_from_uri(nautilus_file_info_get_uri(file), NULL, NULL);
    if (pfilename == NULL) {
      return NAUTILUS_OPERATION_COMPLETE;
    }
    else {
      int cmp = 0;
      gchar *stored_filename;
      gchar *filename;
      
      filename = canonicalize_path(pfilename);
      g_free(pfilename);
      stored_filename = g_hash_table_lookup(cvs->obj2filename, file);

      /* don't worry about the dup checks, gcc is smart enough to optimize this
	 GCSE ftw */
      if ((stored_filename != NULL && (cmp = strcmp(stored_filename, filename)) != 0) ||
	  stored_filename == NULL) {
	
	if (stored_filename != NULL && cmp != 0) {
	  /* this happens when the filename changes name on a file obj 
	     but test_cb isn't called */
	  g_object_weak_unref(G_OBJECT(file), (GWeakNotify) when_file_dies, cvs);
	  g_hash_table_remove(cvs->obj2filename, file);
	  g_hash_table_remove(cvs->filename2obj, stored_filename);
	  g_signal_handlers_disconnect_by_func(file, G_CALLBACK(test_cb), cvs);
	}
	else if (stored_filename == NULL) {
	  NautilusFileInfo *f2;

	  if ((f2 = g_hash_table_lookup(cvs->filename2obj, filename)) != NULL) {
	    /* if the filename exists in the filename2obj hash
	       but the file obj doesn't exist in the obj2filename hash:
	       
	       this happens when nautilus allocates another file object
	       for a filename without first deleting the original file object
	       
	       just remove the association to the older file object, it's obsolete
	    */
	    g_object_weak_unref(G_OBJECT(f2), (GWeakNotify) when_file_dies, cvs);
	    g_signal_handlers_disconnect_by_func(f2, G_CALLBACK(test_cb), cvs);
	    g_hash_table_remove(cvs->filename2obj, filename);
	    g_hash_table_remove(cvs->obj2filename, f2);
	  }
	}

	/* too chatty */
	/* debug("adding %s <-> 0x%p", filename, file);*/
	g_object_weak_ref(G_OBJECT(file), (GWeakNotify) when_file_dies, cvs);
	g_hash_table_insert(cvs->filename2obj, g_strdup(filename), file);
	g_hash_table_insert(cvs->obj2filename, file, g_strdup(filename));
	g_signal_connect(file, "changed", G_CALLBACK(test_cb), cvs);
      }

      g_free(filename);
    }
  }

  if (dropbox_client_is_connected(&(cvs->dc)) == FALSE ||
      nautilus_file_info_is_gone(file)) {
    return NAUTILUS_OPERATION_COMPLETE;
  }

  {
    DropboxFileInfoCommand *dfic = g_new0(DropboxFileInfoCommand, 1);

    dfic->cancelled = FALSE;
    dfic->provider = provider;
    dfic->dc.request_type = GET_FILE_INFO;
    dfic->update_complete = g_closure_ref(update_complete);
    dfic->file = g_object_ref(file);
    
    dropbox_command_client_request(&(cvs->dc.dcc), (DropboxCommand *) dfic);
    
    *handle = (NautilusOperationHandle *) dfic;
    
    return dropbox_use_operation_in_progress_workaround
      ? NAUTILUS_OPERATION_COMPLETE
      : NAUTILUS_OPERATION_IN_PROGRESS;
  }
}

static void
handle_shell_touch(GHashTable *args, NautilusDropbox *cvs) {
  gchar **path;

  //  debug_enter();

  if ((path = g_hash_table_lookup(args, "path")) != NULL &&
      path[0][0] == '/') {
    NautilusFileInfo *file;
    gchar *filename;

    filename = canonicalize_path(path[0]);

    file = g_hash_table_lookup(cvs->filename2obj, filename);
    if (file != NULL) {
      reset_file(file);
    }
    g_free(filename);
  }

  return;
}

static void
handle_copy_to_clipboard(GHashTable *args, NautilusDropbox *cvs) {
  gchar **text;

  if ((text = g_hash_table_lookup(args, "text")) != NULL) {
    GtkClipboard *clip;
    clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, text[0], -1);
  }
  
  return;
}

static void
handle_launch_url(GHashTable *args, NautilusDropbox *cvs) {
  gchar **url;
  
  if ((url = g_hash_table_lookup(args, "url")) != NULL) {
    gchar *command_line, *escaped_string;

    gchar *msg;
    escaped_string = g_strescape(url[0], NULL);
    command_line = g_strdup_printf("gnome-open \"%s\"", url[0]);
    msg = g_strdup_printf("Couldn't start your browser using gnome-open. "
			  "Please check "
			  "and see if you have the 'gnome-open' program "
			  "installed.");
    
    nautilus_dropbox_common_launch_command_with_error(&(cvs->ndt), command_line,
						      "Couldn't start your browser",
						      msg);
    g_free(msg);
    g_free(command_line);
    g_free(escaped_string);
  }
}

gboolean
nautilus_dropbox_finish_file_info_command(DropboxFileInfoCommandResponse *dficr) {
  if (dficr->dfic->cancelled == FALSE) {
    gchar **status= NULL, **options=NULL;
    gboolean isdir;

    isdir = nautilus_file_info_is_directory(dficr->dfic->file) ;
    
    /* if the file status command went okay */
    if ((dficr->file_status_response != NULL &&
	(status =
	 g_hash_table_lookup(dficr->file_status_response, "status")) != NULL) &&
	(dficr->context_options_response != NULL &&
	 (options =
	  g_hash_table_lookup(dficr->context_options_response,
			      "options")) != NULL) &&
	((isdir == TRUE &&
	  dficr->folder_tag_response != NULL) || isdir == FALSE)) {
      gchar **tag = NULL;

      /* set the tag emblem */
      if (isdir &&
	  (tag = g_hash_table_lookup(dficr->folder_tag_response, "tag")) != NULL) {
	if (strcmp("public", tag[0]) == 0) {
	  nautilus_file_info_add_emblem(dficr->dfic->file, "web");
	}
	else if (strcmp("shared", tag[0]) == 0) {
	  nautilus_file_info_add_emblem(dficr->dfic->file, "people");
	}
	else if (strcmp("photos", tag[0]) == 0) {
	  nautilus_file_info_add_emblem(dficr->dfic->file, "photos");
	}
      }

      /* set the status emblem */
      {
	int emblem_code = 0;
	
	if (strcmp("up to date", status[0]) == 0) {
	  emblem_code = 1;
	}
	else if (strcmp("syncing", status[0]) == 0) {
	  emblem_code = 2;
	}
	
	if (emblem_code > 0) {
	  /*
	    debug("%s to %s", emblems[emblem_code-1],
	    g_filename_from_uri(nautilus_file_info_get_uri(dficr->dfic->file),
	    NULL, NULL));
	  */
	  nautilus_file_info_add_emblem(dficr->dfic->file, emblems[emblem_code-1]);
	}
      }

      /* complete the info request */
      if (!dropbox_use_operation_in_progress_workaround) {
	nautilus_info_provider_update_complete_invoke(dficr->dfic->update_complete,
						      dficr->dfic->provider,
						      (NautilusOperationHandle*) dficr->dfic,
						      NAUTILUS_OPERATION_COMPLETE);
      }

      /* save the context menu options */
      {
	/* great now we have to parse these freaking optionssss also make the menu items */
	/* this is where python really makes things easy */
	GHashTable *context_option_hash;
	int i;
	
	context_option_hash = g_hash_table_new_full((GHashFunc) g_str_hash,
						    (GEqualFunc) g_str_equal,
						    g_free, menu_item_free);

	for (i = 0; options[i] != NULL; i++) {
	  gchar **option_info;

	  /*debug("option string %d: %s", i, options[i]);*/
	  
	  option_info = g_strsplit(options[i], "~", 3);
	  /* if this is a valid string */
	  if (option_info[0] != NULL && option_info[1] != NULL &&
	      option_info[2] != NULL && option_info[3] == NULL) {
	    DropboxContextMenuItem *dcmi = g_new0(DropboxContextMenuItem, 1);
	    
	    dcmi->title = g_strdup(option_info[0]);	  
	    dcmi->tooltip = g_strdup(option_info[1]);
	    dcmi->verb = g_strdup(option_info[2]);
    
	    g_hash_table_insert(context_option_hash, g_strdup(dcmi->verb), dcmi);
	  }
	  
	  g_strfreev(option_info);
	}
	
	/*debug("setting nautilus_dropbox_menu_item"); */
	g_object_set_data_full(G_OBJECT(dficr->dfic->file),
			       "nautilus_dropbox_menu_item",
			       context_option_hash,
			       (GDestroyNotify) g_hash_table_unref);
	
	/* lol that wasn't so bad, glib is a big help */
      }    
    }
    else {
      /* operation failed, for some reason..., just complete the invoke */
      if (!dropbox_use_operation_in_progress_workaround) {
	nautilus_info_provider_update_complete_invoke(dficr->dfic->update_complete,
						      dficr->dfic->provider,
						      (NautilusOperationHandle*) dficr->dfic,
						      NAUTILUS_OPERATION_FAILED);
      }
    }
  }

  /* destroy the objects we created */
  if (dficr->file_status_response != NULL)
    g_hash_table_unref(dficr->file_status_response);
  if (dficr->context_options_response != NULL)
    g_hash_table_unref(dficr->context_options_response);
  
  /* unref the objects we didn't create */
  g_closure_unref(dficr->dfic->update_complete);
  g_object_unref(dficr->dfic->file);

  /* now free the structs */
  g_free(dficr->dfic);
  g_free(dficr);

  return FALSE;
}

static void
nautilus_dropbox_cancel_update(NautilusInfoProvider     *provider,
                               NautilusOperationHandle  *handle) {
  DropboxFileInfoCommand *dfic = (DropboxFileInfoCommand *) handle;
  dfic->cancelled = TRUE;
  return;
}

/*
  this is the context options plan:
  1. for each file, whenever we get a file we get the context options too
  2. when the context options are needed for a group of files or file
  we take the intersection of all the context options
  3. when a action is needed to be executed on a group of files:
  create the menu/menuitmes and install a responder that takes 
  the group and then sends the command to dropbox_command
  4. the dropbox command "action", takes a list of paths and an action to do
  (needs to be implemented on the server)
*/
static void
menu_item_cb(NautilusMenuItem *item,
	     NautilusDropbox *cvs) {
  gchar *verb;
  GList *files;
  DropboxGeneralCommand *dcac;

  dcac = g_new(DropboxGeneralCommand, 1);

  /* maybe these would be better passed in a container
     struct used as the userdata pointer, oh well this
     is how dave camp does it */
  files = g_object_get_data(G_OBJECT(item), "nautilus_dropbox_files");
  verb = g_object_get_data(G_OBJECT(item), "nautilus_dropbox_verb");

  dcac->dc.request_type = GENERAL_COMMAND;

  /* build the argument list */
  dcac->command_args = g_hash_table_new_full((GHashFunc) g_str_hash,
					     (GEqualFunc) g_str_equal,
					     (GDestroyNotify) g_free,
					     (GDestroyNotify) g_strfreev);
  {
    gchar **arglist;
    guint i;
    GList *li;

    arglist = g_new(gchar *,g_list_length(files) + 1);

    for (li = files, i = 0; li != NULL; li = g_list_next(li), i++) {
      char *path =
	g_filename_from_uri(nautilus_file_info_get_uri(NAUTILUS_FILE_INFO(li->data)),
			    NULL, NULL);
      arglist[i] = path;
    }
    
    arglist[i] = NULL;
    
    g_hash_table_insert(dcac->command_args,
			g_strdup("paths"),
			arglist);
  }

  {
    gchar **arglist;
    arglist = g_new(gchar *, 2);
    arglist[0] = g_strdup(verb);
    arglist[1] = NULL;
    g_hash_table_insert(dcac->command_args, g_strdup("verb"), arglist);
  }

  dcac->command_name = g_strdup("icon_overlay_context_action");
  dcac->handler = NULL;
  dcac->handler_ud = NULL;

  dropbox_command_client_request(&(cvs->dc.dcc), (DropboxCommand *) dcac);
}

static GList *
nautilus_dropbox_get_file_items(NautilusMenuProvider *provider,
                                GtkWidget            *window,
				GList                *files) {
  GList *toret = NULL;
  GList *li;
  GHashTable *set;

  /* we only do options for single files... for now */
  if (g_list_length(files) != 1) {
    return NULL;
  }

  set = g_hash_table_new((GHashFunc) g_str_hash,
			 (GEqualFunc) g_str_equal);

  /* first seed the set with the first items options */  
  {
    GHashTableIter iter;
    GHashTable *initialset;
    gchar *key;
    DropboxContextMenuItem *dcmi;
    initialset = (GHashTable *) g_object_get_data(G_OBJECT(files->data),
						  "nautilus_dropbox_menu_item");

    /* if a single file isn't a dropbox file
       we don't want it */
    if (initialset == NULL) {
      g_hash_table_unref(set);
      return NULL;
    }

    g_hash_table_iter_init(&iter, initialset);
    while (g_hash_table_iter_next(&iter, (gpointer) &key,
				  (gpointer) &dcmi)) {
      g_hash_table_insert(set, key, dcmi);
    }
  }

  /* need to do the set intersection of all the menu options */
  /* THIS IS EFFECTIVELY IGNORED, we only do options for single files... for now */
  for (li = g_list_next(files); li != NULL; li = g_list_next(li)) {
    GHashTableIter iter;
    GHashTable *fileset;
    gchar *key;
    DropboxContextMenuItem *dcmi;
    GList *keys_to_remove = NULL, *li2;
    
    fileset = (GHashTable *) g_object_get_data(G_OBJECT(li->data),
					       "nautilus_dropbox_menu_item");
    /* check if all the values in set are in the
       this fileset, if they are then keep that file in the set
       if not, then remove that file from the set */
    g_hash_table_iter_init(&iter, set);      
    while (g_hash_table_iter_next(&iter, (gpointer) &key,
				  (gpointer) &dcmi)) {
      if (g_hash_table_lookup(fileset, key) == NULL) {
	keys_to_remove = g_list_append(keys_to_remove, key);
      }
    }

    /* now actually remove, since we can't in the iterator */
    for (li2 = keys_to_remove; li2 != NULL; li2 = g_list_next(li2)) {
      g_hash_table_remove(set, li2->data);
    }

    g_list_free(keys_to_remove);
  }

  /* if the hash table is empty, don't show options */
  if (g_hash_table_size(set) == 0) {
    g_hash_table_unref(set);
    return NULL;
  }

  /* build the menu */
  {
    NautilusMenuItem *root_item;
    NautilusMenu *root_menu;
    GHashTableIter iter;
    gchar *key;
    DropboxContextMenuItem *dcmi;

    root_menu = nautilus_menu_new();
    root_item = nautilus_menu_item_new("NautilusDropbox::root_item",
				       "Dropbox", "Dropbox Options", "dropbox");
    nautilus_menu_item_set_submenu(root_item, root_menu);

    toret = g_list_append(toret, root_item);

    g_hash_table_iter_init(&iter, set);      
    while (g_hash_table_iter_next(&iter, (gpointer) &key,
				  (gpointer) &dcmi)) {
      NautilusMenuItem *item;
      GString *new_action_string;
      
      new_action_string = g_string_new("NautilusDropbox::");
      g_string_append(new_action_string, dcmi->verb);

      item = nautilus_menu_item_new(new_action_string->str,
				    dcmi->title,
				    dcmi->tooltip, NULL);

      nautilus_menu_append_item(root_menu, item);
      /* add the file metadata to this item */
      g_object_set_data_full (G_OBJECT(item), "nautilus_dropbox_files", 
			      nautilus_file_info_list_copy (files),
			      (GDestroyNotify) nautilus_file_info_list_free);
      /* add the verb metadata */
      g_object_set_data_full (G_OBJECT(item), "nautilus_dropbox_verb", 
			      g_strdup(dcmi->verb),
			      (GDestroyNotify) g_free);
      g_signal_connect (item, "activate", G_CALLBACK (menu_item_cb), provider);

      /* taken from nautilus-file-repairer (http://repairer.kldp.net/):
       * this code is a workaround for a bug of nautilus
       * See: http://bugzilla.gnome.org/show_bug.cgi?id=508878 */
      if (dropbox_use_nautilus_submenu_workaround) {
	toret = g_list_append(toret, item);
      }

      g_object_unref(item);
      g_string_free(new_action_string, TRUE);
    }

    g_object_unref(root_menu);
    g_hash_table_unref(set);
    
    return toret;
  }
}

static void
on_connect(NautilusDropbox *cvs) {
  /* gotta initialize icon overlays */
  dropbox_command_client_send_simple_command(&(cvs->dc.dcc), "icon_overlay_init");
  /* and tell dropbox what X server we're on */
  dropbox_command_client_send_command(&(cvs->dc.dcc), NULL, NULL,
				      "on_x_server", "display", g_getenv("DISPLAY"), NULL);
}

static void
on_disconnect(NautilusDropbox *cvs) {
  /* this works because you can call a function pointer with
     more arguments than it takes */
  g_hash_table_foreach(cvs->obj2filename, (GHFunc) reset_file, NULL);
}

static void
nautilus_dropbox_menu_provider_iface_init (NautilusMenuProviderIface *iface) {
  iface->get_file_items = nautilus_dropbox_get_file_items;
  return;
}

static void
nautilus_dropbox_info_provider_iface_init (NautilusInfoProviderIface *iface) {
  iface->update_file_info = nautilus_dropbox_update_file_info;
  iface->cancel_update = nautilus_dropbox_cancel_update;
  return;
}

static void
nautilus_dropbox_instance_init (NautilusDropbox *cvs) {
  /* this data is shared by all submodules */
  cvs->filename2obj = g_hash_table_new_full((GHashFunc) g_str_hash,
					    (GEqualFunc) g_str_equal,
					    (GDestroyNotify) g_free,
					    (GDestroyNotify) NULL);
  cvs->obj2filename = g_hash_table_new_full((GHashFunc) g_direct_hash,
					    (GEqualFunc) g_direct_equal,
					    (GDestroyNotify) NULL,
					    (GDestroyNotify) g_free);

  /* setup the connection obj*/
  dropbox_client_setup(&(cvs->dc));

  /* then the tray */
  nautilus_dropbox_tray_setup(&(cvs->ndt), &(cvs->dc));

  /* our hooks */
  nautilus_dropbox_hooks_add(&(cvs->dc.hookserv), "shell_touch",
			     (DropboxUpdateHook) handle_shell_touch, cvs);
  nautilus_dropbox_hooks_add(&(cvs->dc.hookserv), "copy_to_clipboard",
			     (DropboxUpdateHook) handle_copy_to_clipboard, cvs);
  nautilus_dropbox_hooks_add(&(cvs->dc.hookserv), "launch_url",
			     (DropboxUpdateHook) handle_launch_url, cvs);

  /* add connect handlers */
  dropbox_client_add_on_connect_hook(&(cvs->dc),
				     (DropboxClientConnectHook) on_connect, 
				     cvs);
  dropbox_client_add_on_disconnect_hook(&(cvs->dc),
					(DropboxClientConnectHook) on_disconnect, 
					cvs);
  
  /* now start the connection */
  dropbox_client_start(&(cvs->dc));

  return;
}

static void
nautilus_dropbox_class_init (NautilusDropboxClass *class) {
}

static void
nautilus_dropbox_class_finalize (NautilusDropboxClass *class) {
  debug("just checking");
  /* kill threads here? */
}

GType
nautilus_dropbox_get_type (void) {
  return dropbox_type;
}

void
nautilus_dropbox_register_type (GTypeModule *module) {
  static const GTypeInfo info = {
    sizeof (NautilusDropboxClass),
    (GBaseInitFunc) NULL,
    (GBaseFinalizeFunc) NULL,
    (GClassInitFunc) nautilus_dropbox_class_init,
    (GClassFinalizeFunc) nautilus_dropbox_class_finalize,
    NULL,
    sizeof (NautilusDropbox),
    0,
    (GInstanceInitFunc) nautilus_dropbox_instance_init,
  };

  static const GInterfaceInfo menu_provider_iface_info = {
    (GInterfaceInitFunc) nautilus_dropbox_menu_provider_iface_init,
    NULL,
    NULL
  };

  static const GInterfaceInfo info_provider_iface_info = {
    (GInterfaceInitFunc) nautilus_dropbox_info_provider_iface_init,
    NULL,
    NULL
  };

  dropbox_type =
    g_type_module_register_type(module,
				G_TYPE_OBJECT,
				"NautilusDropbox",
				&info, 0);
  
  g_type_module_add_interface (module,
			       dropbox_type,
			       NAUTILUS_TYPE_MENU_PROVIDER,
			       &menu_provider_iface_info);

  g_type_module_add_interface (module,
			       dropbox_type,
			       NAUTILUS_TYPE_INFO_PROVIDER,
			       &info_provider_iface_info);
}
