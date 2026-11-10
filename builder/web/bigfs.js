/* An Emscripten file system for the web builder's large files (the disc pack and its pools, the 1.6 GB output):
 * directories are MEMFS's, each file's bytes are an OPFS file (the browser's private file system, on disk) through a
 * FileSystemSyncAccessHandle. Opening one is asynchronous and Python's file calls are not, so the handles are opened
 * before the build and lent to files as they are created; a file's handle goes back (emptied) when it is deleted
 * and no longer open, or replaced by a rename. */
export function makeBigFS(FS, pool) {
  const MEMFS = FS.filesystems.MEMFS;
  const free = pool.slice();
  const take = () => {
    const h = free.pop();
    if (!h) throw new FS.ErrnoError(28); // ENOSPC: more large files at once than the pool has
    h.truncate(0);
    return h;
  };
  const release = (node) => {
    if (node.big && node.unlinked && !node.opens) {
      node.big.truncate(0);
      free.push(node.big);
      node.big = null;
    }
  };
  const fileNode = {
    getattr(node) {
      const a = MEMFS.node_ops.getattr(node);
      a.size = node.big.getSize();
      a.blocks = Math.ceil(a.size / 4096);
      a.blksize = 1 << 20; // (Python sizes its buffers by it: 1 MB writes, not 4 KB)
      return a;
    },
    setattr(node, attr) {
      if (attr.size !== undefined) node.big.truncate(attr.size);
      const rest = Object.assign({}, attr);
      delete rest.size;
      MEMFS.node_ops.setattr(node, rest);
    },
  };
  const fileStream = {
    open(stream) { stream.node.opens++; },
    close(stream) { stream.node.opens--; release(stream.node); },
    read(stream, buffer, offset, length, position) {
      return stream.node.big.read(buffer.subarray(offset, offset + length), { at: position });
    },
    write(stream, buffer, offset, length, position) {
      return stream.node.big.write(buffer.subarray(offset, offset + length), { at: position });
    },
    llseek(stream, offset, whence) {
      let p = offset;
      if (whence === 1) p += stream.position;
      else if (whence === 2) p += stream.node.big.getSize();
      if (p < 0) throw new FS.ErrnoError(28);
      return p;
    },
  };
  const BIG = {
    mount() { return BIG.createNode(null, '/', 0o40777, 0); },
    createNode(parent, name, mode, dev) {
      if (!FS.isDir(mode) && !FS.isFile(mode)) throw new FS.ErrnoError(63);
      const node = MEMFS.createNode(parent, name, mode, dev);
      if (FS.isDir(node.mode)) {
        node.node_ops = dirNode;
      } else {
        node.node_ops = fileNode;
        node.stream_ops = fileStream;
        node.big = take();
        node.opens = 0;
      }
      return node;
    },
    /* the OPFS handle behind a file (to hand the finished output over: closed, so the page can read it) */
    handleOf(path) { return FS.lookupPath(path).node.big; },
  };
  const dropped = (node) => {
    if (node && node.big) { node.unlinked = true; release(node); }
  };
  const dirNode = Object.assign({}, MEMFS.node_ops, {
    mknod: (parent, name, mode, dev) => BIG.createNode(parent, name, mode, dev),
    rename(oldNode, newDir, newName) {
      const target = newDir.contents[newName];
      MEMFS.node_ops.rename(oldNode, newDir, newName);
      if (target !== oldNode) dropped(target); // (a file renamed over: its bytes are gone)
    },
    unlink(parent, name) {
      const node = parent.contents[name];
      MEMFS.node_ops.unlink(parent, name);
      dropped(node);
    },
  });
  return BIG;
}
