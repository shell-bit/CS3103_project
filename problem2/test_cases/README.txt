Q2 test command scripts

From the problem2 directory, build minifs and run tests with:
  make -B minifs
  ./minifs < test_cases/case1_basic.txt

Scripts use only commands in the specification. Each independent case starts
with format, which initializes the simulated disk in the working directory.
The restart half of case4 is deliberately different: run both halves from
the same directory and do not format or delete virtual_disk.bin between them.

case1_basic.txt
  Exercise create, write, read, append, replacement, deletion, ls and df.
  The first three reads of hello.txt show, in order:
    Operating Systems
    Operating Systems project
    OS
  reused.txt contains Reused.

case2_paths.txt
  Exercise nested directories, absolute/relative paths, '.', '..', and cleanup.
  pwd first shows /courses/os/labs, then /courses/os. The report reads all
  return Path example. The script returns to / before deleting directories.

case3_blocks.txt
  Exercise an empty file, 256-byte content, append across a block boundary,
  600-byte content, shrinkage, deletion and reuse. Content sizes are 0, 256,
  257, 600, then 5 bytes before deletion. The final reused.txt is 600 bytes.
  Check stat and df against the data and metadata layout you document.

case4_persist_create.txt and case4_persist_restart.txt
  Run these as two separate program invocations, in order:
    ./minifs < test_cases/case4_persist_create.txt
    ./minifs < test_cases/case4_persist_restart.txt
  The hierarchy and Operating Systems content must survive restart. The
  restart script then appends text and reads Operating Systems project.

case5_invalid.txt
  After setup, attempt a duplicate name, missing paths, traversal through a
  regular file, removal of a nonempty directory or root, a 32-character name,
  and a write missing the documented quotes. These operations should report
  errors without changing the file system. kept.txt must remain unchanged.
  No exact wording for error messages is prescribed by these samples.

case6_entry_limit.txt
  Root uses one of the 128 metadata slots. Starting from format, the first
  127 creates fit, and creating f127.txt should fail without partial changes.
  f126.txt must remain accessible, and no f127.txt entry should be created.

cases 7-14 supplement the instructor's cases 1-6. The expected df values use
this documented layout: 4096 blocks of 256 bytes, 97 reserved metadata blocks
and 3999 data blocks (1,023,744 bytes). Cases 9-11 contain large literal inputs
for this capacity. Adapt them if your layout changes. No generator is needed.


case7_format_reset.txt
  Exercise reformatting while the current directory is /old. The first pwd
  shows /old; after format it shows /. The old file must be absent, ls empty,
  root children=0, and df used=97 free=3999. Reading the old file should fail.
  A new fresh.txt then contains NEW, is 3 bytes / 1 block, and df is 98/3998.
  In the Linux shell, stat -c '%s' virtual_disk.bin must report 1048576.

case8_write_append_boundaries.txt
  Exercise final-block slack, exact block boundaries and empty content.
  edge.txt sizes are 255, 256, 257, 512, 513, 0, 0, then 1 byte; block counts
  are 1, 1, 2, 2, 3, 0, 0, then 1. The first read is 255 A characters + BC;
  the second is 512 D characters + E. An empty write releases all blocks;
  an empty append allocates none. The empty read has no content; final read
  is Z. df after clearing is 97/3999, and the final df is 98/3998.

case9_fragmentation.txt
  Fill the data region using two one-block hole files, a one-block guard and
  filler.bin (1,022,976 F characters / 3996 blocks). Delete the hole files
  and write 256 A + 256 B to mixed.bin (512 bytes / 2 blocks). With the current
  first-free FAT allocator, its blocks are separated by the guard's block.
  Read both blocks correctly. Delete mixed.bin and reuse its two blocks for
  reused.bin (512 X characters). KEEP and every filler byte must remain intact.
  df is 4096/0 when full, 4094/2 after deleting mixed.bin, 4096/0 after reuse,
  and 97/3999 after cleanup. No errors are expected.

case10_disk_full.txt
  Fill full.bin with 1,023,743 A characters, using all 3999 data blocks.
  Creating empty.txt must still succeed without a data block. Appending Z
  uses the final byte of full.bin's last block: size=1023744, blocks=3999.
  Appending ! and writing X to empty.txt must both fail. Read must still show
  the original A characters followed by Z; empty.txt remains 0 bytes / 0 blocks.
  df stays 4096/0. Delete full.bin: df becomes 97/3999. Writing OK to empty.txt
  must now succeed, with size=2, blocks=1 and final df 98/3998.

case11_multiblock_failure.txt
  victim.txt contains ORIGINAL (8 bytes / 1 block), keeper.txt contains KEEP
  (4 bytes / 1 block), and filler.bin contains 1,022,976 F bytes / 3996 blocks.
  df is used=4095 free=1. Replacing victim.txt with 513 W bytes needs TWO extra
  blocks and must fail. Appending 505 A bytes also needs TWO extra blocks and
  must fail. After each failure, ORIGINAL remains 8 bytes / 1 block and df is
  still 4095/1. KEEP and all filler bytes must be unchanged. Delete filler.bin
  and retry: 513 W bytes must succeed using 3 blocks, with df 101/3995.
  Deleting the remaining files then restores df to 97/3999.

case12_entry_reuse.txt
  Root, d0, d1, guard.txt and 124 files under d0 fill all 128 metadata slots.
  Creating overflow.txt or extra directory must fail without partial changes.
  Delete d0/f000.txt and reuse its slot for d0/reused.txt containing REUSED
  (6 bytes / 1 block). d0 still has 124 children. Remove empty d1 and reuse its
  slot for newdir, which has 0 children. Another overflow create must fail;
  querying deleted f000.txt must fail. KEEP remains intact. The final root
  children count is 3 and df is 99/3997. Four rejections are expected in total.

case13_names_directories_paths.txt
  At root, cd .. leaves pwd at /. Allow same.txt in left and right, but reject
  a duplicate within left. Accept a 31-character name containing NAME31;
  reject 32-character file and directory names. From left, read ../right/same.txt
  as RIGHT; absolute append changes it to RIGHT_R, and relative write changes
  left/same.txt to LEFT2. stat . and .. each show 2 direct children, excluding
  dot entries. Exercise relative creation/deletion of a file and empty directory.
  Create nine 31-character directory components: deepest pwd is 288 bytes long;
  leaf.txt's absolute path is 297 bytes. Both relative reads return DEEP_OK.
  The deepest directory has 1 child; root has 3. Final df is 101/3995.
  Three rejections are expected: the duplicate and the two overlong names.

case14_invalid_commands.txt
  Prepare guard.txt=SAFE, box/file.txt=DATA and df 99/3997. Reject an unknown
  command, missing/extra arguments, malformed quotes, directory-as-file and
  file-as-directory operations. format extra must not clear the disk, and
  exit extra must not stop later checks. Fourteen rejections are expected.
  Afterwards pwd is /, reads still return SAFE and DATA, guard is 4 bytes /
  1 block, box has 1 child, root has 2, and df is unchanged. A valid append
  then reads SAFE! (5 bytes / 1 block), showing normal operation continues.
