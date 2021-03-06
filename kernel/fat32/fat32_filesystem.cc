#include "types.h"
#include "fat32.hh"
#include "kstream.hh"

static console_stream verbose(false);

// TODO: make the filesystem writable (and therefore include locking), instead of having it be read-only
sref<filesystem>
vfs_new_fat32(disk *device)
{
  fat32_header hdr = {};
  device->read((char*) &hdr, sizeof(fat32_header), 0);
  if (!hdr.check_signature())
    return sref<filesystem>();
  u64 cluster_size = hdr.sectors_per_cluster * SECTORSIZ;
  if (cluster_size % PGSIZE != 0) {
    // this is a restriction imposed by this implementation
    verbose.println("cannot mount FAT32 filesystem: cluster sizes of at least PGSIZE=", PGSIZE, " are required, but found ", cluster_size);
    return sref<filesystem>();
  }
  verbose.println("found a valid FAT32 signature with cluster size of ", cluster_size);
  u64 max_clusters = 1024 * 1024 / cluster_size; // use 1 MB for cluster cache
  auto cluster_cache = make_sref<fat32_cluster_cache>(device, max_clusters, cluster_size, hdr.first_data_sector() * SECTORSIZ);
  return make_sref<fat32_filesystem>(cluster_cache, hdr);
}

void
vfs_enable_fat32_writeback(const sref<filesystem> &fs)
{
  auto ffs = dynamic_cast<fat32_filesystem *>(fs.get());
  assert(ffs);
  ffs->enable_writeback();
}

fat32_filesystem::fat32_filesystem(const sref<fat32_cluster_cache>& cluster_cache, fat32_header hdr)
  : hdr(hdr), weaklink(make_sref<fat32_filesystem_weaklink>(this)), cluster_cache(cluster_cache)
{
  fat = make_sref<fat32_alloc_table>(cluster_cache, hdr.first_fat_sector(), hdr.sectors_per_fat());
  u32 cluster = hdr.root_directory_cluster_id;
  root_node = make_sref<vnode_fat32>(weaklink, cluster, true, sref<vnode_fat32>(), 0);
}

sref<vnode>
fat32_filesystem::root()
{
  return root_node;
}

sref<vnode>
fat32_filesystem::resolve_child(const sref<vnode>& base, const char *filename)
{
  return base->cast<vnode_fat32>()->ref_child(filename);
}

sref<vnode>
fat32_filesystem::resolve_parent(const sref<vnode>& base)
{
  return base->cast<vnode_fat32>()->ref_parent();
}

void
fat32_filesystem::enable_writeback()
{
  cluster_cache->enable_writeback();
}

void
fat32_filesystem::onzero()
{
  delete this;
}

fat32_filesystem_weaklink::fat32_filesystem_weaklink(fat32_filesystem *fs)
  : filesystem(fs)
{
}

sref<fat32_filesystem>
fat32_filesystem_weaklink::get()
{
  return filesystem.get();
}
