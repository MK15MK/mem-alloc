# GDB helpers for inspecting the ma heap of a live (or stopped) process.
#   gdb -x tools/ma.gdb ./build/demo

define ma-heap
  call (void) ma_dump(1)
end
document ma-heap
Print every arena, segment and chunk of the ma heap.
end

define ma-check
  print (int) ma_check()
end
document ma-check
Verify heap invariants; prints 0 if consistent.
end

# ma-chunk PTR: decode the chunk header in front of a user pointer.
define ma-chunk
  set $c = (unsigned long *)((char *)($arg0) - 16)
  set $h = $c[1]
  printf "chunk %p size=%lu inuse=%lu prev_inuse=%lu mmapped=%lu arena=%lu\n", $c, $h & 0x00fffffffffffff0, $h & 1, ($h >> 1) & 1, ($h >> 2) & 1, $h >> 56
end
document ma-chunk
Decode the header of the chunk owning user pointer ARG0.
end
