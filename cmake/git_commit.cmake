# Writes OUT, a header defining LOBSIM_GIT_COMMIT: the commit checked out in SRC, with "-dirty"
# appended when tracked files have uncommitted changes, or "unknown" without git. Run at every
# build; the file is rewritten only when its text changes, so nothing recompiles otherwise.
set(_commit unknown)
if(GIT)
  execute_process(COMMAND ${GIT} rev-parse HEAD WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE _head RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_rc EQUAL 0 AND _head MATCHES "^[0-9a-f]+$")
    set(_commit ${_head})
    execute_process(COMMAND ${GIT} --no-optional-locks status --porcelain --untracked-files=no WORKING_DIRECTORY ${SRC}
      OUTPUT_VARIABLE _changes RESULT_VARIABLE _rc2 ERROR_QUIET)
    if(_rc2 EQUAL 0 AND NOT _changes STREQUAL "")
      string(APPEND _commit "-dirty")
    endif()
  endif()
endif()
set(_text "// Generated at build time by cmake/git_commit.cmake.\n#define LOBSIM_GIT_COMMIT \"${_commit}\"\n")
set(_old "")
if(EXISTS ${OUT})
  file(READ ${OUT} _old)
endif()
if(NOT _old STREQUAL _text)
  file(WRITE ${OUT} "${_text}")
endif()
