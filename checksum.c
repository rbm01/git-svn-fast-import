/* Copyright (C) 2015 by Maxim Bublis <b@codemonkey.ru>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "checksum.h"
#include "node.h"
#include "sorts.h"
#include <svn_dirent_uri.h>
#include <svn_hash.h>
#include <svn_props.h>

#define SYMLINK_CONTENT_PREFIX "link"

struct checksum_cache_t
{
    apr_hash_t *cache;
    apr_pool_t *pool;
};

checksum_cache_t *
checksum_cache_create(apr_pool_t *pool)
{
    checksum_cache_t *c = apr_pcalloc(pool, sizeof(checksum_cache_t));
    c->pool = pool;
    c->cache = apr_hash_make(pool);

    return c;
}

static svn_checksum_t *
checksum_cache_get(checksum_cache_t *c,
                   const svn_checksum_t *svn_checksum,
                   apr_pool_t *pool)
{
    return svn_hash_gets(c->cache, svn_checksum_serialize(svn_checksum, pool, pool));
}

static void
checksum_cache_set(checksum_cache_t *c,
                   const svn_checksum_t *svn_checksum,
                   const svn_checksum_t *git_checksum)
{
    svn_checksum_t *key = svn_checksum_dup(svn_checksum, c->pool);
    svn_checksum_t *val = svn_checksum_dup(git_checksum, c->pool);
    svn_hash_sets(c->cache, svn_checksum_serialize(key, c->pool, c->pool), val);
}

svn_error_t *
checksum_cache_dump(checksum_cache_t *c,
                    svn_stream_t *dst,
                    apr_pool_t *pool)
{
    apr_hash_index_t *idx;

    for (idx = apr_hash_first(pool, c->cache); idx; idx = apr_hash_next(idx)) {
        const svn_checksum_t *svn_checksum;
        const svn_checksum_t *git_checksum = apr_hash_this_val(idx);

        SVN_ERR(svn_checksum_deserialize(&svn_checksum, apr_hash_this_key(idx),
                                         pool, pool));

        SVN_ERR(svn_stream_printf(dst, pool, "%s %s\n",
                                  svn_checksum_to_cstring_display(svn_checksum, pool),
                                  svn_checksum_to_cstring_display(git_checksum, pool)));
    }

    return SVN_NO_ERROR;
}

typedef struct
{
    svn_stream_t *stream;
    svn_checksum_ctx_t *read_ctx;
    svn_checksum_ctx_t *write_ctx;
} stream_ctx_t;

static svn_error_t *
read_handler(void *stream_ctx, char *buf, apr_size_t *len)
{
    stream_ctx_t *ctx = stream_ctx;

    SVN_ERR(svn_stream_read(ctx->stream, buf, len));

    if (ctx->read_ctx != NULL) {
        SVN_ERR(svn_checksum_update(ctx->read_ctx, buf, *len));
    }

    return SVN_NO_ERROR;
}

static svn_error_t *
write_handler(void *stream_ctx, const char *data, apr_size_t *len)
{
    stream_ctx_t *ctx = stream_ctx;

    if (ctx->write_ctx != NULL) {
        SVN_ERR(svn_checksum_update(ctx->write_ctx, data, *len));
    }

    SVN_ERR(svn_stream_write(ctx->stream, data, len));

    return SVN_NO_ERROR;
}

static svn_error_t *
close_handler(void *stream_ctx)
{
    stream_ctx_t *ctx = stream_ctx;

    SVN_ERR(svn_stream_close(ctx->stream));

    return SVN_NO_ERROR;
}

// Creates a stream that updates checksum context for all data
// read and written.
static svn_stream_t *
checksum_stream_create(svn_stream_t *s,
                       svn_checksum_ctx_t *read_ctx,
                       svn_checksum_ctx_t *write_ctx,
                       apr_pool_t *pool)
{
    stream_ctx_t *ctx = apr_pcalloc(pool, sizeof(stream_ctx_t));
    ctx->stream = s;
    ctx->read_ctx = read_ctx;
    ctx->write_ctx = write_ctx;

    svn_stream_t *stream = svn_stream_create(ctx, pool);
    svn_stream_set_read(stream, read_handler);
    svn_stream_set_write(stream, write_handler);
    svn_stream_set_close(stream, close_handler);

    return stream;
}

svn_error_t *
set_content_checksum(svn_checksum_t **checksum,
                     svn_stream_t *output,
                     checksum_cache_t *cache,
                     svn_fs_root_t *root,
                     const char *path,
                     apr_pool_t *pool)
{
    const char *hdr;
    apr_hash_t *props;
    svn_checksum_t *svn_checksum, *git_checksum;
    svn_checksum_ctx_t *ctx;
    svn_filesize_t size;
    svn_stream_t *content;

    SVN_ERR(svn_fs_file_checksum(&svn_checksum, svn_checksum_sha1,
                                 root, path, FALSE, pool));

    git_checksum = checksum_cache_get(cache, svn_checksum, pool);
    if (git_checksum != NULL) {
        *checksum = git_checksum;
        return SVN_NO_ERROR;
    }

    SVN_ERR(svn_fs_node_proplist(&props, root, path, pool));
    SVN_ERR(svn_fs_file_length(&size, root, path, pool));
    SVN_ERR(svn_fs_file_contents(&content, root, path, pool));

    // We need to strip a symlink marker from the beginning of a content
    // and subtract a symlink marker length from the blob size.
    if (svn_hash_gets(props, SVN_PROP_SPECIAL)) {
        apr_size_t skip = sizeof(SYMLINK_CONTENT_PREFIX);
        SVN_ERR(svn_stream_skip(content, skip));
        size -= skip;
    }

    hdr = apr_psprintf(pool, "blob %ld", size);
    ctx = svn_checksum_ctx_create(svn_checksum_sha1, pool);
    SVN_ERR(svn_checksum_update(ctx, hdr, strlen(hdr) + 1));

    SVN_ERR(svn_stream_printf(output, pool, "blob\n"));
    SVN_ERR(svn_stream_printf(output, pool, "data %ld\n", size));

    output = checksum_stream_create(output, NULL, ctx, pool);

    SVN_ERR(svn_stream_copy3(content, output, NULL, NULL, pool));

    SVN_ERR(svn_checksum_final(&git_checksum, ctx, pool));

    checksum_cache_set(cache, svn_checksum, git_checksum);
    *checksum = git_checksum;

    return SVN_NO_ERROR;
}

svn_error_t *
set_tree_checksum(svn_checksum_t **checksum,
                  apr_array_header_t **entries,
                  svn_stream_t *output,
                  checksum_cache_t *cache,
                  svn_fs_root_t *root,
                  const char *path,
                  const char *root_path,
                  tree_t *ignores,
                  apr_pool_t *pool)
{
    apr_array_header_t *sorted_entries, *nodes;
    apr_hash_t *dir_entries;
    const char *hdr, *ignored;
    svn_checksum_ctx_t *ctx;
    svn_stringbuf_t *buf;

    ctx = svn_checksum_ctx_create(svn_checksum_sha1, pool);
    buf = svn_stringbuf_create_empty(pool);
    nodes = apr_array_make(pool, 0, sizeof(node_t));

    SVN_ERR(svn_fs_dir_entries(&dir_entries, root, path, pool));

    sorted_entries = svn_sort__hash(dir_entries,
                                    svn_sort_compare_items_gitlike,
                                    pool);

    for (int i = 0; i < sorted_entries->nelts; i++) {
        apr_array_header_t *subentries = NULL;
        const char *node_path, *record, *subpath;
        node_t *node;
        svn_sort__item_t item = APR_ARRAY_IDX(sorted_entries, i,
                                              svn_sort__item_t);
        svn_fs_dirent_t *entry = item.value;
        svn_checksum_t *node_checksum;

        node_path = svn_relpath_join(path, entry->name, pool);
        subpath = svn_dirent_skip_ancestor(root_path, node_path);

        ignored = tree_find_longest_prefix(ignores, subpath);
        if (ignored != NULL) {
            continue;
        }

        if (entry->kind == svn_node_dir) {
            SVN_ERR(set_tree_checksum(&node_checksum, &subentries, output,
                                      cache, root, node_path, root_path,
                                      ignores, pool));
            // Skip empty directories.
            if (subentries->nelts == 0) {
                continue;
            }
        } else {
            SVN_ERR(set_content_checksum(&node_checksum, output, cache, root,
                                         node_path, pool));
        }

        node = apr_array_push(nodes);
        node->kind = entry->kind;
        node->path = node_path;
        node->checksum = node_checksum;
        node->entries = subentries;
        SVN_ERR(set_node_mode(&node->mode, root, node->path, pool));

        record = apr_psprintf(pool, "%o %s", node->mode, entry->name);
        svn_stringbuf_appendbytes(buf, record, strlen(record) + 1);
        svn_stringbuf_appendbytes(buf, (const char *)node->checksum->digest,
                                  svn_checksum_size(node->checksum));
    }

    *entries = nodes;

    hdr = apr_psprintf(pool, "tree %ld", buf->len);

    SVN_ERR(svn_checksum_update(ctx, hdr, strlen(hdr) + 1));
    SVN_ERR(svn_checksum_update(ctx, buf->data, buf->len));
    SVN_ERR(svn_checksum_final(checksum, ctx, pool));

    return SVN_NO_ERROR;
}
