/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsjobunmount.h"

G_DEFINE_TYPE (GVfsJobUnmount, g_vfs_job_unmount, G_VFS_TYPE_JOB_DBUS)

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);
static void     create_reply (GVfsJob               *job,
                              GVfsDBusMount         *object,
                              GDBusMethodInvocation *invocation);

static void
g_vfs_job_unmount_finalize (GObject *object)
{
  GVfsJobUnmount *job;

   job = G_VFS_JOB_UNMOUNT (object);

  if (job->mount_source)
    g_object_unref (job->mount_source);

  if (G_OBJECT_CLASS (g_vfs_job_unmount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_unmount_parent_class)->finalize) (object);
}

static void
g_vfs_job_unmount_class_init (GVfsJobUnmountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_unmount_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;

  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_unmount_init (GVfsJobUnmount *job)
{
}

gboolean
g_vfs_job_unmount_new_handle (GVfsDBusMount *object,
                              GDBusMethodInvocation *invocation,
                              const gchar *arg_dbus_id,
                              const gchar *arg_obj_path,
                              guint arg_flags,
                              GVfsBackend *backend)
{
  GVfsJobUnmount *job;

  g_print ("called Unmount()\n");

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;

  g_debug ("g_vfs_job_unmount_new request: %p\n", invocation);

  job = g_object_new (G_VFS_TYPE_JOB_UNMOUNT,
                      "object", object,
                      "invocation", invocation,
                      NULL);

  job->backend = backend;
  job->flags = arg_flags;
  job->mount_source = g_mount_source_new (arg_dbus_id, arg_obj_path);
  
  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

static void
run (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->unmount == NULL)
    return;

  class->unmount (op_job->backend,
		  op_job,
                  op_job->flags,
                  op_job->mount_source);
}

static gboolean
job_finish_immediately_if_possible (GVfsJobUnmount *op_job)
{
  GVfsBackend      *backend = op_job->backend;
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean is_busy;
  gboolean force_unmount;

  if (class->try_unmount != NULL || class->unmount != NULL)
    return FALSE;

  is_busy = g_vfs_backend_has_blocking_processes (backend);
  force_unmount = op_job->flags & G_MOUNT_UNMOUNT_FORCE;

  if (is_busy && ! force_unmount)
    g_vfs_job_failed_literal (G_VFS_JOB (op_job),
			      G_IO_ERROR, G_IO_ERROR_BUSY,
			      _("Filesystem is busy"));
  else
    g_vfs_job_succeeded (G_VFS_JOB (op_job));

  return TRUE;
}

static void
unmount_cb (GVfsBackend  *backend,
            GAsyncResult *res,
            gpointer      user_data)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (user_data);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean should_unmount;
  gboolean finished;

  should_unmount = g_vfs_backend_unmount_with_operation_finish (backend,
                                                                res);

  if (should_unmount)
    op_job->flags |= G_MOUNT_UNMOUNT_FORCE;

  finished = job_finish_immediately_if_possible (op_job);

  if (! finished)
    {
      gboolean run_in_thread = TRUE;

      if (class->try_unmount != NULL)
	run_in_thread = ! class->try_unmount (op_job->backend,
					      op_job,
					      op_job->flags,
					      op_job->mount_source);

       if (run_in_thread)
	g_vfs_daemon_run_job_in_thread (g_vfs_backend_get_daemon (backend),
					G_VFS_JOB (op_job));
    }
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);
  GVfsBackend    *backend = op_job->backend;
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean is_busy;
  gboolean force_unmount;

  is_busy = g_vfs_backend_has_blocking_processes (backend);
  force_unmount = op_job->flags & G_MOUNT_UNMOUNT_FORCE;
  
  if (is_busy && ! force_unmount
      && ! g_mount_source_is_dummy (op_job->mount_source))
    {
      g_vfs_backend_unmount_with_operation (backend,
					    op_job->mount_source,
					    (GAsyncReadyCallback) unmount_cb,
					    op_job);
      return TRUE;
    }

  if (job_finish_immediately_if_possible (op_job))
    return TRUE;
  else if (class->try_unmount != NULL)
    return class->try_unmount (op_job->backend,
			       op_job,
			       op_job->flags,
			       op_job->mount_source);
  else
    return FALSE;
}

static void
unregister_mount_callback (GVfsDBusMountTracker *proxy,
                           GAsyncResult *res,
                           gpointer user_data)
{
  GVfsBackend *backend;
  GVfsDaemon *daemon;
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (user_data);
  GError *error = NULL;

  gvfs_dbus_mount_tracker_call_unregister_mount_finish (proxy,
                                                        res,
                                                        &error);
  g_debug ("unregister_mount_callback, error: %p\n", error);
  
  if (error != NULL)
    {
      /* If we failed before, don't overwrite the error as this one is not that important */ 
      if (! job->failed)
        g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
    }
  
  backend = op_job->backend;
  (*G_VFS_JOB_CLASS (g_vfs_job_unmount_parent_class)->send_reply) (G_VFS_JOB (op_job));

  /* Unlink job source from daemon */
  daemon = g_vfs_backend_get_daemon (backend);
  g_vfs_job_source_closed (G_VFS_JOB_SOURCE (backend));

  g_vfs_daemon_close_active_channels (daemon);
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);

  g_debug ("send_reply, failed: %d\n", job->failed);

  if (job->failed)
    (*G_VFS_JOB_CLASS (g_vfs_job_unmount_parent_class)->send_reply) (G_VFS_JOB (op_job));
  else
    {
      GVfsBackend *backend = op_job->backend;

      /* Setting the backend to block requests will also
         set active GVfsChannels to block requets  */
      g_vfs_backend_set_block_requests (backend);
      g_vfs_backend_unregister_mount (backend,
				      (GAsyncReadyCallback) unregister_mount_callback,
				      job);
    }
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  gvfs_dbus_mount_complete_unmount (object, invocation);
}
