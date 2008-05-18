#include "cvs_connection.h"
#include "branch.h"
#include "changeset.h"
#include "database.h"
#include "emission.h"
#include "file.h"
#include "log.h"
#include "log_parse.h"
#include "string_cache.h"
#include "utils.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long mark_counter;

static const char * format_date (const time_t * time)
{
    struct tm dtm;
    static char date[32];
    size_t dl = strftime (date, sizeof (date), "%F %T %Z",
                          localtime_r (time, &dtm));
    if (dl == 0)
        // Maybe someone gave us a crap timezone?
        dl = strftime (date, sizeof (date), "%F %T %Z",
                       gmtime_r (time, &dtm));

    assert (dl != 0);
    return date;
}


static bool check_entry (const char * s, const char * version)
{
    if (s[0] != '/')
        return false;

    const char * slash = strchr (s + 1, '/');
    if (slash == NULL)
        return false;

    size_t vlen = strlen (version);
    if (strncmp (slash + 1, version, vlen) != 0)
        return false;

    if (slash[vlen + 1] != '/')
        return false;

    return true;
}


static void grab_version (cvs_connection_t * s, const version_t * version)
{
    if (version->dead) {
        printf ("D %s\n", version->file->path); // Easy peasy.
        return;
    }

    fprintf (s->stream,
             "Argument -kk\n"
             "Argument -r%s\n"
             "Argument --\n"
             "Argument %s/%s\nco\n",
             version->version, s->module, version->file->path);

    do
        next_line (s);
    while (starts_with (s->line, "M ") || starts_with (s->line, "MT "));

    if (!starts_with (s->line, "Created ") &&
        !starts_with (s->line, "Update-existing ") &&
        !starts_with (s->line, "Updated "))
        fatal ("cvs checkout %s %s - did not get Update line: '%s'\n",
               version->version, version->file->path, s->line);

    next_line (s);
    // It looks like some CVS servers give us absolute paths; others give us
    // relative.  So just check the end of the path.
    if (!ends_with (s->line, version->file->path))
        fatal ("cvs checkout %s %s - got unexpected file '%s'\n",
               version->version, version->file->path, s->line);

    next_line (s);

    if (!check_entry (s->line, version->version))
        fatal ("cvs checkout %s %s - got unexpected entry '%s'\n",
               version->version, version->file->path, s->line);

    next_line (s);
    if (!starts_with (s->line, "u="))
        fatal ("cvs checkout %s %s - got unexpected file mode '%s'\n",
               version->version, version->file->path, s->line);

    bool exec = strchr (s->line, 'x') != NULL;

    next_line (s);
    char * tail;
    unsigned long len = strtoul (s->line, &tail, 10);
    if (len == ULONG_MAX || *tail != 0)
        fatal ("cvs checkout %s %s - got unexpected file length '%s'\n",
               version->version, version->file->path, s->line);

    printf ("M %s inline %s\ndata %lu\n",
            exec ? "755" : "644", version->file->path, len);

    while (len != 0) {
        char buffer[4096];
        size_t get = len < 4096 ? len : 4096;
        size_t got = fread (&buffer, 1, get, s->stream);
        if (got == 0)
            fatal ("cvs checkout %s %s - interrupted: %s\n",
                   version->version, version->file->path,
                   ferror (s->stream) ? strerror (errno) : (
                       feof (s->stream) ? "closed" : "unknown"));
        size_t put = fwrite (&buffer, got, 1, stdout);
        if (put != 1)
            fatal ("git import %s %s - interrupted: %s\n",
                   version->version, version->file->path,
                   ferror (stdout) ? strerror (errno) : (
                       feof (stdout) ? "closed" : "unknown"));
        len -= got;
    }
    printf ("\n");

    next_line (s);
    if (strcmp (s->line, "ok") != 0)
        fatal ("cvs checkout %s %s - did not get ok: '%s'\n",
               version->version, version->file->path, s->line);
}


static void print_commit (const database_t * db, changeset_t * cs,
                          cvs_connection_t * s)
{
    const version_t * v = cs->versions;
    if (!v->branch) {
        fprintf (stderr, "%s <anon> %s %s COMMIT - skip\n%s\n",
                 format_date (&cs->time), v->author, v->commitid, v->log);
        return;
    }

    // Check to see if this commit actually does anything...
    bool nil = true;
    for (const version_t * i = v; i; i = i->cs_sibling)
        if (i->used) {
            const version_t * bv
                = v->branch->tag->branch_versions[i->file - db->files];
            if (bv != NULL && bv->dead)
                bv = NULL;
            const version_t * cv = i->dead ? NULL : i;
            if (bv != cv) {
                nil = false;
                break;
            }
        }

    if (nil) {
        fprintf (stderr, "%s %s %s %s COMMIT - does nothing\n",
                 format_date (&cs->time), v->branch->tag->tag,
                 v->author, v->commitid);
        cs->mark = v->branch->tag->last->mark;
        v->branch->tag->last = cs;
        return;
    }

    v->branch->tag->last = cs;
    cs->mark = ++mark_counter;

    // Give CVS a unique directory for the changeset in case we have repeated
    // checkouts of the same file.
    fprintf (s->stream, "Directory %lu\n%s\n",
             cs->mark, s->remote_root);

    printf ("commit refs/heads/%s\n",
            *v->branch->tag->tag ? v->branch->tag->tag : "cvs_master");
    printf ("mark :%lu\n", cs->mark);
    printf ("committer %s <%s> %ld +0000\n", v->author, v->author, cs->time);
    printf ("data %u\n%s\n", strlen (v->log), v->log);

    for (const version_t * i = v; i; i = i->cs_sibling)
        if (i->used)
            grab_version (s, i);
}


static void print_tag (const database_t * db, tag_t * tag,
                       cvs_connection_t * s)
{
    fprintf (stderr, "%s %s %s\n",
             format_date (&tag->changeset.time),
             tag->branch_versions ? "BRANCH" : "TAG",
             tag->tag);

    if (tag->exact_match)
        fprintf (stderr, "Exact match\n");

    tag_t * branch;
    if (tag->parent == NULL)
        branch = NULL;
    else if (tag->parent->type == ct_commit)
        branch = tag->parent->versions->branch->tag;
    else
        branch = as_tag (tag->parent);

    assert (tag->parent == NULL || (branch && branch->last == tag->parent));

    printf ("reset refs/%s/%s\n",
            tag->branch_versions ? "heads" : "tags",
            *tag->tag ? tag->tag : "cvs_master");

    if (tag->parent) {
        printf ("from :%lu\n\n", tag->parent->mark);
        tag->changeset.mark = tag->parent->mark;
    }

    tag->last = &tag->changeset;

    // Go through the current versions on the branch and note any version
    // fix-ups required.
    size_t keep = 0;
    size_t added = 0;
    size_t deleted = 0;
    size_t modified = 0;

    file_tag_t ** tf = tag->tag_files;
    for (file_t * i = db->files; i != db->files_end; ++i) {
        version_t * bv = branch
            ? version_live (branch->branch_versions[i - db->files]) : NULL;
        version_t * tv = NULL;
        if (tf != tag->tag_files_end && (*tf)->file == i)
            tv = version_live ((*tf++)->version);

        if (bv == tv) {
            if (bv != NULL)
                ++keep;
            continue;
        }

        if (bv == NULL)
            ++added;
        else if (tv == NULL)
            ++deleted;
        else
            ++modified;
    }

    if (added == 0 && deleted == 0 && modified == 0) {
        if (!tag->exact_match)
            fprintf (stderr, "WIERD: no fixups but not exact match\n");
        return;                         // Nothing to do.
    }

    tag->changeset.mark = ++mark_counter;
    if (tag->exact_match)
        fprintf (stderr, "WIERD: fixups for exact match\n");

    const char ** list = NULL;
    const char ** list_end = NULL;

    ARRAY_APPEND (list,
                  xasprintf ("Fix-up commit generated by crap-clone.  "
                            "(~%zu +%zu -%zu =%zu)\n",
                            modified, added, deleted, keep));

    tf = tag->tag_files;
    for (file_t * i = db->files; i != db->files_end; ++i) {
        version_t * bv = branch
            ? version_live (branch->branch_versions[i - db->files]) : NULL;
        version_t * tv = NULL;
        if (tf != tag->tag_files_end && (*tf)->file == i)
            tv = version_live ((*tf++)->version);

        if (bv == tv) {
            if (bv != NULL && keep <= deleted)
                ARRAY_APPEND (list, xasprintf ("%s KEEP %s\n",
                                               bv->file->path, bv->version));
            continue;
        }

        if (tv != NULL || deleted <= keep)
            ARRAY_APPEND (list, xasprintf ("%s %s->%s\n", bv->file->path,
                                           bv ? bv->version : "ADD",
                                           tv ? tv->version : "DELETE"));
    }

    size_t log_len = 0;
    for (const char ** i = list; i != list_end; ++i)
        log_len += strlen (*i);

    printf ("commit refs/%s/%s\n",
            tag->branch_versions ? "heads" : "tags",
            *tag->tag ? tag->tag : "cvs_master");
    printf ("mark :%lu\n", tag->changeset.mark);
    printf ("committer crap <crap> %ld +0000\n", tag->changeset.time);
    printf ("data %u\n", log_len);
    for (const char ** i = list; i != list_end; ++i) {
        fputs (*i, stdout);
        xfree (*i);
    }
    xfree (list);

    // Give CVS a unique directory for the changeset in case we have
    // repeated checkouts of the same file.
    fprintf (s->stream, "Directory %lu\n%s\n",
             tag->changeset.mark, s->remote_root);

    tf = tag->tag_files;
    for (file_t * i = db->files; i != db->files_end; ++i) {
        version_t * bv = branch
            ? version_live (branch->branch_versions[i - db->files]) : NULL;
        version_t * tv = NULL;
        if (tf != tag->tag_files_end && (*tf)->file == i)
            tv = version_live ((*tf++)->version);

        if (tv != bv) {
            if (tv == NULL)
                printf ("D %s\n", i->path);
            else
                grab_version (s, tv);
        }
    }
}


int main (int argc, const char * const * argv)
{
    if (argc != 3)
        fatal ("Usage: %s <root> <repo>\n", argv[0]);

    cvs_connection_t stream;
    connect_to_cvs (&stream, argv[1]);
    stream.prefix = xasprintf ("%s/%s/", stream.remote_root, argv[2]);
    stream.module = xstrdup (argv[2]);

    fprintf (stream.stream,
             "Global_option -q\n"
             "Argument --\n"
             "Argument %s\n"
             "rlog\n", stream.module);

    database_t db;

    read_files_versions (&db, &stream);

    create_changesets (&db);

    branch_analyse (&db);

    // Prepare for the real changeset emission.  This time the tags go through
    // the the usual emission process, and branches block revisions on the
    // branch.

    for (tag_t * i = db.tags; i != db.tags_end; ++i) {
        i->is_released = false;
        for (changeset_t ** j = i->changeset.children;
             j != i->changeset.children_end; ++j)
            ++(*j)->unready_count;
    }

    // Re-do the version->changeset unready counts.
    prepare_for_emission (&db, NULL);

    // Mark the initial tags as ready to emit, and fill in branches with their
    // initial versions.
    for (tag_t * i = db.tags; i != db.tags_end; ++i) {
        if (i->changeset.unready_count == 0)
            heap_insert (&db.ready_changesets, &i->changeset);
        if (i->branch_versions) {
            memset (i->branch_versions, 0,
                    sizeof (version_t *) * (db.files_end - db.files));
            for (file_tag_t ** j = i->tag_files; j != i->tag_files_end; ++j)
                i->branch_versions[(*j)->file - db.files] = (*j)->version;
        }
    }

    // Emit the changesets for real.
    size_t emitted_commits = 0;
    changeset_t * changeset;
    while ((changeset = next_changeset (&db))) {
        if (changeset->type == ct_commit) {
            ++emitted_commits;
            print_commit (&db, changeset, &stream);
            changeset_update_branch_versions (&db, changeset);
        }
        else {
            tag_t * tag = as_tag (changeset);
            tag->is_released = true;
            print_tag (&db, tag, &stream);
        }

        changeset_emitted (&db, NULL, changeset);
    }

    fflush (NULL);
    fprintf (stderr,
             "Emitted %u commits (%s total %u).\n",
             emitted_commits,
             emitted_commits == db.changesets_end - db.changesets ? "=" : "!=",
             db.changesets_end - db.changesets);

    size_t matched_branches = 0;
    size_t late_branches = 0;
    size_t matched_tags = 0;
    size_t late_tags = 0;
    for (tag_t * i = db.tags; i != db.tags_end; ++i) {
        assert (i->is_released);
        if (i->branch_versions)
            if (i->exact_match)
                ++matched_branches;
            else
                ++late_branches;
        else
            if (i->exact_match)
                ++matched_tags;
            else
                ++late_tags;
    }

    fprintf (stderr,
             "Matched %5u + %5u = %5u branches + tags.\n"
             "Late    %5u + %5u = %5u branches + tags.\n",
             matched_branches, matched_tags, matched_branches + matched_tags,
             late_branches, late_tags, late_branches + late_tags);

    string_cache_stats (stderr);

    printf ("progress done\n");

    cvs_connection_destroy (&stream);

    database_destroy (&db);
    string_cache_destroy();

    return 0;
}
