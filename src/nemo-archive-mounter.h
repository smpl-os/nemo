/* nemo-archive-mounter.h - FUSE-based archive mounting for transparent browsing
 *
 * Copyright (C) 2026 nemo-smpl contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef NEMO_ARCHIVE_MOUNTER_H
#define NEMO_ARCHIVE_MOUNTER_H

#include <glib.h>

G_BEGIN_DECLS

gboolean nemo_archive_mounter_is_archive (const gchar *mime_type);

/* Returns TRUE only if this MIME type is an archive AND the required
 * FUSE mount helper (fuse-zip / archivemount) is available on PATH.
 * Callers should skip the mount-and-browse interception when this
 * returns FALSE and fall back to the default GIO handler. */
gboolean nemo_archive_mounter_can_mount (const gchar *mime_type);

gchar *nemo_archive_mounter_mount (const gchar *archive_path,
                                   const gchar *mime_type,
                                   GError **error);

void nemo_archive_mounter_unmount (const gchar *archive_path);

G_END_DECLS

#endif /* NEMO_ARCHIVE_MOUNTER_H */
