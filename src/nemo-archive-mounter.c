/* nemo-archive-mounter.c - FUSE-based archive mounting for transparent browsing
 *
 * Copyright (C) 2026 nemo-smpl contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <config.h>
#include "nemo-archive-mounter.h"
#include <glib.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

/* Archive type detection based on MIME type */
typedef enum {
    ARCHIVE_TYPE_ZIP,
    ARCHIVE_TYPE_TAR,
    ARCHIVE_TYPE_TAR_GZ,
    ARCHIVE_TYPE_TAR_BZ2,
    ARCHIVE_TYPE_TAR_XZ,
    ARCHIVE_TYPE_7Z,
    ARCHIVE_TYPE_RAR,
    ARCHIVE_TYPE_UNKNOWN
} ArchiveType;

/* Mounted archive tracking */
typedef struct {
    gchar *archive_path;
    gchar *mount_point;
    GPid mount_pid;
    guint64 last_access;
} MountedArchive;

static GHashTable *mounted_archives = NULL;
static GMutex mount_mutex;

static void
init_mounted_archives (void)
{
    if (mounted_archives == NULL) {
        mounted_archives = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free, g_free);
    }
}

static ArchiveType
detect_archive_type (const gchar *mime_type)
{
    if (mime_type == NULL) {
        return ARCHIVE_TYPE_UNKNOWN;
    }

    if (g_strcmp0 (mime_type, "application/zip") == 0 ||
        g_strcmp0 (mime_type, "application/x-zip-compressed") == 0) {
        return ARCHIVE_TYPE_ZIP;
    } else if (g_strcmp0 (mime_type, "application/x-tar") == 0) {
        return ARCHIVE_TYPE_TAR;
    } else if (g_strcmp0 (mime_type, "application/x-compressed-tar") == 0 ||
               g_strcmp0 (mime_type, "application/x-gzip") == 0) {
        return ARCHIVE_TYPE_TAR_GZ;
    } else if (g_strcmp0 (mime_type, "application/x-bzip-compressed-tar") == 0 ||
               g_strcmp0 (mime_type, "application/x-bzip2") == 0) {
        return ARCHIVE_TYPE_TAR_BZ2;
    } else if (g_strcmp0 (mime_type, "application/x-xz-compressed-tar") == 0 ||
               g_strcmp0 (mime_type, "application/x-xz") == 0) {
        return ARCHIVE_TYPE_TAR_XZ;
    } else if (g_strcmp0 (mime_type, "application/x-7z-compressed") == 0) {
        return ARCHIVE_TYPE_7Z;
    } else if (g_strcmp0 (mime_type, "application/x-rar") == 0 ||
               g_strcmp0 (mime_type, "application/vnd.rar") == 0) {
        return ARCHIVE_TYPE_RAR;
    }

    return ARCHIVE_TYPE_UNKNOWN;
}

static gboolean
check_tool_available (const gchar *tool)
{
    gchar *path = g_find_program_in_path (tool);
    gboolean available = (path != NULL);
    g_free (path);
    return available;
}

static gchar *
get_mount_base_dir (void)
{
    const gchar *runtime_dir = g_get_user_runtime_dir ();
    gchar *mount_base = g_build_filename (runtime_dir, "nemo-archives", NULL);
    
    /* Create directory if it doesn't exist */
    if (g_mkdir_with_parents (mount_base, 0700) != 0) {
        g_warning ("Failed to create mount directory: %s", mount_base);
        g_free (mount_base);
        return NULL;
    }
    
    return mount_base;
}

static gchar *
create_mount_point (const gchar *archive_name)
{
    gchar *mount_base = get_mount_base_dir ();
    if (mount_base == NULL) {
        return NULL;
    }
    
    /* Create unique mount point name */
    gchar *basename = g_path_get_basename (archive_name);
    gchar *sanitized = g_strdup (basename);
    
    /* Replace special characters */
    for (gchar *p = sanitized; *p; p++) {
        if (!g_ascii_isalnum (*p) && *p != '.' && *p != '-') {
            *p = '_';
        }
    }
    
    gchar *mount_point = g_build_filename (mount_base, sanitized, NULL);
    
    /* If directory exists, add timestamp suffix */
    if (g_file_test (mount_point, G_FILE_TEST_EXISTS)) {
        gchar *timestamped = g_strdup_printf ("%s_%ld", mount_point, time (NULL));
        g_free (mount_point);
        mount_point = timestamped;
    }
    
    /* Create mount point directory */
    if (mkdir (mount_point, 0700) != 0) {
        g_warning ("Failed to create mount point: %s", mount_point);
        g_free (mount_point);
        g_free (mount_base);
        g_free (basename);
        g_free (sanitized);
        return NULL;
    }
    
    g_free (mount_base);
    g_free (basename);
    g_free (sanitized);
    
    return mount_point;
}

static gboolean
mount_archive_with_tool (const gchar *archive_path,
                         const gchar *mount_point,
                         ArchiveType type,
                         GPid *out_pid,
                         GError **error)
{
    gchar *argv[4] = { NULL, NULL, NULL, NULL };
    
    switch (type) {
        case ARCHIVE_TYPE_ZIP:
            if (!check_tool_available ("fuse-zip")) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "fuse-zip is not installed");
                return FALSE;
            }
            argv[0] = g_strdup ("fuse-zip");
            argv[1] = g_strdup (archive_path);
            argv[2] = g_strdup (mount_point);
            break;
            
        case ARCHIVE_TYPE_TAR:
        case ARCHIVE_TYPE_TAR_GZ:
        case ARCHIVE_TYPE_TAR_BZ2:
        case ARCHIVE_TYPE_TAR_XZ:
        case ARCHIVE_TYPE_7Z:
        case ARCHIVE_TYPE_RAR:
            if (!check_tool_available ("archivemount")) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "archivemount is not installed");
                return FALSE;
            }
            argv[0] = g_strdup ("archivemount");
            argv[1] = g_strdup (archive_path);
            argv[2] = g_strdup (mount_point);
            break;
            
        default:
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Unsupported archive type");
            return FALSE;
    }
    
    /* Spawn the mount process */
    gboolean success = g_spawn_async (NULL, argv, NULL,
                                      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                      NULL, NULL, out_pid, error);
    
    g_free (argv[0]);
    g_free (argv[1]);
    g_free (argv[2]);
    
    if (success) {
        /* Give it a moment to mount */
        g_usleep (500000); /* 0.5 seconds */
        
        /* Verify mount succeeded by checking if directory is accessible */
        if (!g_file_test (mount_point, G_FILE_TEST_IS_DIR)) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Mount point is not accessible after mounting");
            return FALSE;
        }
    }
    
    return success;
}

gboolean
nemo_archive_mounter_is_archive (const gchar *mime_type)
{
    return detect_archive_type (mime_type) != ARCHIVE_TYPE_UNKNOWN;
}

gboolean
nemo_archive_mounter_can_mount (const gchar *mime_type)
{
    ArchiveType type = detect_archive_type (mime_type);

    switch (type) {
        case ARCHIVE_TYPE_ZIP:
            return check_tool_available ("fuse-zip");
        case ARCHIVE_TYPE_TAR:
        case ARCHIVE_TYPE_TAR_GZ:
        case ARCHIVE_TYPE_TAR_BZ2:
        case ARCHIVE_TYPE_TAR_XZ:
        case ARCHIVE_TYPE_7Z:
        case ARCHIVE_TYPE_RAR:
            return check_tool_available ("archivemount");
        case ARCHIVE_TYPE_UNKNOWN:
        default:
            return FALSE;
    }
}

gchar *
nemo_archive_mounter_mount (const gchar *archive_path,
                            const gchar *mime_type,
                            GError **error)
{
    g_return_val_if_fail (archive_path != NULL, NULL);
    g_return_val_if_fail (mime_type != NULL, NULL);
    
    g_mutex_lock (&mount_mutex);
    init_mounted_archives ();
    
    /* Check if already mounted */
    MountedArchive *existing = g_hash_table_lookup (mounted_archives, archive_path);
    if (existing != NULL) {
        existing->last_access = g_get_real_time ();
        gchar *result = g_strdup (existing->mount_point);
        g_mutex_unlock (&mount_mutex);
        return result;
    }
    
    /* Detect archive type */
    ArchiveType type = detect_archive_type (mime_type);
    if (type == ARCHIVE_TYPE_UNKNOWN) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Unsupported archive type: %s", mime_type);
        g_mutex_unlock (&mount_mutex);
        return NULL;
    }
    
    /* Create mount point */
    gchar *mount_point = create_mount_point (archive_path);
    if (mount_point == NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create mount point");
        g_mutex_unlock (&mount_mutex);
        return NULL;
    }
    
    /* Mount the archive */
    GPid mount_pid = 0;
    if (!mount_archive_with_tool (archive_path, mount_point, type, &mount_pid, error)) {
        rmdir (mount_point);
        g_free (mount_point);
        g_mutex_unlock (&mount_mutex);
        return NULL;
    }
    
    /* Track mounted archive */
    MountedArchive *mounted = g_new0 (MountedArchive, 1);
    mounted->archive_path = g_strdup (archive_path);
    mounted->mount_point = g_strdup (mount_point);
    mounted->mount_pid = mount_pid;
    mounted->last_access = g_get_real_time ();
    
    g_hash_table_insert (mounted_archives, g_strdup (archive_path), mounted);
    
    g_mutex_unlock (&mount_mutex);
    
    return mount_point;
}

void
nemo_archive_mounter_unmount (const gchar *archive_path)
{
    g_return_if_fail (archive_path != NULL);
    
    g_mutex_lock (&mount_mutex);
    
    if (mounted_archives == NULL) {
        g_mutex_unlock (&mount_mutex);
        return;
    }
    
    MountedArchive *mounted = g_hash_table_lookup (mounted_archives, archive_path);
    if (mounted == NULL) {
        g_mutex_unlock (&mount_mutex);
        return;
    }
    
    /* Unmount using fusermount */
    gchar *argv[] = { "fusermount", "-u", mounted->mount_point, NULL };
    g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    
    /* Remove mount point directory */
    rmdir (mounted->mount_point);
    
    /* Remove from tracking */
    g_hash_table_remove (mounted_archives, archive_path);
    
    g_mutex_unlock (&mount_mutex);
}
