message(STATUS "Building junction kernel")

set(SOURCES_C_CPP
  arena.cc
  as.cc
  elf.cc
  exec.cc
  futex.cc
  itimer.cc
  misc.cc
  memprobe.cc
  memtrace.cc
  mm.cc
  proc.cc
  random.cc
  sched.cc
  sigframe.cc
  signal.cc
  time.cc
  trapframe.cc
)

set(SOURCES_ASM
  ksys.S
)

add_library(kernel_c_cpp OBJECT ${SOURCES_C_CPP})
add_library(kernel_asm OBJECT ${SOURCES_ASM})

add_library(kernel STATIC
  $<TARGET_OBJECTS:kernel_c_cpp>
  $<TARGET_OBJECTS:kernel_asm>
)

target_link_libraries(kernel PUBLIC
  base
  bindings
  control
  snapshot
)
