# vultureFS

vultureFS is the root filesystem of vultureOS. It is a copy-on-write
filesystem: nothing on disk that the last checkpoint uses is ever
overwritten. A crash at any moment therefore leaves the last checkpoint
whole, and mounting needs no journal replay and no repair.

One implementation serves everything. `Libraries/LibVultureFS` is built
into the kernel (`Kernel/FileSystem/VultureFS.cpp` adds the page cache and
locking), into the host tools `mkvfs` and `fsck.vfs`, and into the host
tests.

## Layout

A volume is an array of 4 KiB blocks:

| Blocks | What |
|---|---|
| 0, 1 | Superblocks. Checkpoint *N* is written to block *N* mod 2. |
| anywhere else | B+tree nodes, allocation bitmap blocks and file data, wherever the allocator put them. |

On a disk, the volume is a GPT partition of type
`7675c7e1-56d1-4c4f-8f5e-76756c747572`. All integers are little-endian. The
structures are defined in `Libraries/LibVultureFS/Format.h`.

### Superblock

| Field | Meaning |
|---|---|
| `magic` | `vulture1` |
| `checksum` | CRC-32C of the whole block, computed with this field zero |
| `generation` | Checkpoint number, from 1 |
| `block_count`, `free_blocks` | Volume size and free space, in blocks |
| `tree_root`, `tree_root_generation` | The B+tree's root node |
| `next_object` | The next inode number to hand out |
| `uuid`, `name` | Identity, e.g. "Vulture HD" |
| `bitmap[]` | Where each allocation bitmap block is, with its CRC-32C |

Up to 224 bitmap blocks of 32768 bits each limit a volume to 28 GiB in
version 1.

### The tree

All metadata lives in one B+tree of 4 KiB nodes. Each node has:

- A header: magic `VFSN`, a CRC-32C, the block it was written to (which
  catches misdirected writes), its generation, its level (0 for leaves)
  and an entry count.
- In leaves, entries hold a key plus an offset and size of a value; the
  values are packed from the end of the block.
- In internal nodes, entries hold a key plus the child's block and
  generation. A parent records each child's generation, so a lost write
  (a stale block where a new one should be) is detected.

Keys are `(object, type, offset)` and sort in that order:

| Item | Key | Value |
|---|---|---|
| Inode | `(inode, 1, 0)` | mode, link count, owner, size, allocated blocks, four timestamps (ns), parent directory |
| Directory entry | `(directory, 2, hash(name))` | one or more `(inode, type, name)` records; names whose FNV-1a hashes collide share an item |
| Extent | `(inode, 3, first file block)` | `(first disk block, block count)` |
| Inline data | `(inode, 4, 0)` | a symbolic link's target |
| Orphan | `(1, 5, inode)` | nothing: an inode with no names that was still open |

Inode 2 is the root directory; numbers below 16 are reserved. Directory
sizes count entries. A directory's link count is 2 plus its number of
subdirectories, as in Unix.

## Changing and committing

Changes happen in memory:

- Changed tree nodes are marked dirty, along with every node above them.
  They get a disk location only when a checkpoint writes them, bottom up,
  each to a newly allocated block.
- File data is the exception: it is written at once, but always to newly
  allocated blocks. The extents that point at it are tree changes, so the
  new data becomes part of the file only when its checkpoint commits.

A checkpoint goes like this:

1. Write the dirty tree nodes, then the changed bitmap blocks, all to free
   blocks.
2. Flush the disk's write cache.
3. Write the superblock to the slot the previous checkpoint did not use.
4. Flush again. The checkpoint is now committed.

The allocator keeps two bitmaps: what is in use now, and what the last
committed checkpoint uses. A block is allocated only if neither has it.
So a block freed since the last checkpoint, which that checkpoint may
still point at, is never overwritten before the next checkpoint commits.

If the power fails before step 4 completes, either the new superblock is
whole (its checksum proves it) and everything it points at was flushed in
step 2, or it is not, and mounting uses the other slot, whose blocks were
never touched.

Mounting reads both superblocks, takes the valid one with the highest
generation, and falls back to the other if its tree cannot be read. It
then deletes the inodes on the orphan list (files unlinked while open
when the machine went down) and commits.

## In the kernel

- **Page cache.** Each inode is cached once, with its file data in 4 KiB
  pages. `read`, `write` and `mmap` all use the same pages; a shared
  mapping writes the file's own pages.
- **Write-back.** On a miss, up to 16 pages are read at once. Dirty pages
  are written back in runs of up to 32 when a checkpoint is taken.
- **When checkpoints happen:**
  - every 5 seconds, if anything changed (the `sync` thread);
  - on `fsync` and `sync`;
  - when a writer has more than 32 MiB dirty;
  - at power off.
- **Deleting.** An unlinked file that is still open becomes an orphan. It
  is deleted when the last reference to it goes away, or at the next
  mount if the machine went down first.

## Checking

`fsck.vfs IMAGE` verifies, without writing, the checkpoint a mount would
use:

- every checksum;
- the tree's shape, key order and key ranges;
- that every block has exactly one user, and the bitmap says so exactly
  (no leaks, no double use);
- link counts, directory sizes and parents;
- extents (inside the volume, not overlapping, not past the end of the
  file);
- that every inode is reachable from the root or is an orphan.

`check_volume()` in LibVultureFS does the same for the tests.

## Tools and tests

- **`mkvfs create IMAGE SIZE_MIB ARCHIVE [NAME]`** makes a sparse GPT disk
  image holding one vultureFS partition, filled from the boot archive.
- **`mkvfs update IMAGE ARCHIVE`** brings a newer build's files into an
  existing image and leaves the rest alone. `Meta/run.sh` does this to
  `build/qemu/disk.img` after each build.
- **`ninja test-host`** includes the LibVultureFS tests, among them 300
  rounds of random changes on a simulated disk whose unflushed writes are
  lost, reordered or torn when its power fails. Each round must mount,
  pass the check and hold exactly its last committed checkpoint.
- **`ninja test-persistence`** (part of `check`) boots three times; each
  boot must find what the one before committed.
- **`ninja test-crash`** cuts QEMU's power 100 times at random moments
  while `/usr/tests/crash-writer` writes and fsyncs. After each cut,
  `fsck.vfs` must pass, and the next boot must find every committed file
  intact.
