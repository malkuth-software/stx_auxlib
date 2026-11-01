# Runs tests/console_process_tests.cpp's modes and checks their effects.
#   cmake -DPROGRAM=<console_process_tests> -DWORK=<scratch directory>
#         -P console_process_check.cmake

if(NOT PROGRAM OR NOT WORK)
    message(FATAL_ERROR "PROGRAM and WORK are required")
endif()
file(MAKE_DIRECTORY "${WORK}")

function(expect_equal actual expected what)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${what}:\n--- got ---\n${actual}\n--- expected ---\n${expected}")
    endif()
endfunction()

# write: order with printf kept, UTF-8 intact, invalid text refused, both
# streams separate. (CMake strings have no \x escapes, so the expected UTF-8
# is written literally; this file is UTF-8.)
execute_process(COMMAND "${PROGRAM}" write
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
expect_equal("${result}" "0" "write exit status")
string(CONCAT expected_output
    "from printf\n"
    "after printf\n"
    "héllo wörld 😀\n"
    "invalid text refused\n"
    "terminal: no\n")
expect_equal("${output}" "${expected_output}" "write output")
expect_equal("${error}" "to stderr\n" "write error stream")

# read: CRLF and LF stripped, an over-limit line refused and skipped,
# invalid UTF-8 refused, a line of exactly the limit kept, a last line
# without an ending returned, then the end.
execute_process(COMMAND "${PROGRAM}" prepare "${WORK}/input.txt" RESULT_VARIABLE result)
expect_equal("${result}" "0" "prepare exit status")
execute_process(COMMAND "${PROGRAM}" read INPUT_FILE "${WORK}/input.txt"
    RESULT_VARIABLE result OUTPUT_VARIABLE output)
expect_equal("${result}" "0" "read exit status")
string(CONCAT expected_output
    "ok first\n"
    "ok second\n"
    "too_long\n"
    "invalid_text\n"
    "ok exactly-sixteen!\n"
    "ok last\n"
    "end\n")
expect_equal("${output}" "${expected_output}" "read output")

# exit: tasks run newest first, a task may register another, and the
# status is the one given.
execute_process(COMMAND "${PROGRAM}" exit RESULT_VARIABLE result OUTPUT_VARIABLE output)
expect_equal("${result}" "7" "exit status")
string(CONCAT expected_output
    "null task refused\n"
    "task two\n"
    "task three, registered by task two\n"
    "task one\n")
expect_equal("${output}" "${expected_output}" "exit task order")

# fail: one report line naming the location, then an abnormal end; nothing
# after the fault runs.
execute_process(COMMAND "${PROGRAM}" fail
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if("${result}" STREQUAL "0")
    message(FATAL_ERROR "fail ended normally")
endif()
expect_equal("${output}" "before the fault\n" "output before the fault")
# The function name is the compiler's spelling and may itself contain
# parentheses ("int (anonymous namespace)::fail_mode()").
if(NOT error MATCHES "fatal: broken invariant \\(.*console_process_tests\\.cpp:[0-9]+, .*fail_mode.*\\)\n")
    message(FATAL_ERROR "fault report is wrong:\n${error}")
endif()
