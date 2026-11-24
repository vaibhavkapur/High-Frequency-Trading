# End-to-end CLI test, run by ctest:
#   1. generate a binary command log,
#   2. replay it twice with the pool book and once with the map book,
#   3. require all three report streams to be byte-identical,
#   4. check that the text pipeline (encode -> replay -> dump) agrees with direct text replay,
#   5. check that a truncated log is reported as an error.
foreach(var ME_CLI WORK_DIR EXAMPLES_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is not set")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${err}")
  endif()
endfunction()

function(require_same a b)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${a}" "${b}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "files differ: ${a} ${b}")
  endif()
endfunction()

set(common --symbols 3 --tick 5 --lot 10)
run(${ME_CLI} gen --out ${WORK_DIR}/commands.bin --count 200000 --seed 7 --live 5000
    --invalid-pm 20 --random-ids ${common})
run(${ME_CLI} replay --in ${WORK_DIR}/commands.bin --out ${WORK_DIR}/pool1.bin --book pool ${common})
run(${ME_CLI} replay --in ${WORK_DIR}/commands.bin --out ${WORK_DIR}/pool2.bin --book pool ${common})
run(${ME_CLI} replay --in ${WORK_DIR}/commands.bin --out ${WORK_DIR}/map.bin --book map ${common})
require_same(${WORK_DIR}/pool1.bin ${WORK_DIR}/pool2.bin)
require_same(${WORK_DIR}/pool1.bin ${WORK_DIR}/map.bin)

file(SIZE ${WORK_DIR}/pool1.bin report_bytes)
if(report_bytes LESS 12800000)  # at least one 64-byte report per command
  message(FATAL_ERROR "report stream is unexpectedly small: ${report_bytes} bytes")
endif()

# Text pipeline: encode + binary replay + dump must equal direct text replay.
set(sample --symbols 2 --tick 5 --lot 10)
run(${ME_CLI} encode --in ${EXAMPLES_DIR}/sample_session.txt --out ${WORK_DIR}/sample.bin)
run(${ME_CLI} replay --in ${WORK_DIR}/sample.bin --out ${WORK_DIR}/sample_reports.bin ${sample})
execute_process(COMMAND ${ME_CLI} dump --in ${WORK_DIR}/sample_reports.bin
                OUTPUT_FILE ${WORK_DIR}/sample_dump.txt RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "dump failed")
endif()
run(${ME_CLI} replay --text-in --text --in ${EXAMPLES_DIR}/sample_session.txt
    --out ${WORK_DIR}/sample_text.txt ${sample})
require_same(${WORK_DIR}/sample_dump.txt ${WORK_DIR}/sample_text.txt)
require_same(${WORK_DIR}/sample_text.txt ${EXAMPLES_DIR}/sample_session.expected.txt)

# A log cut in the middle of a record must be flagged, not silently accepted.
# 50 bytes: one whole command plus a fragment of the next.
execute_process(COMMAND sh -c "head -c 50 '${WORK_DIR}/sample.bin' > '${WORK_DIR}/truncated.bin'" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "could not build truncated input")
endif()
execute_process(COMMAND ${ME_CLI} replay --in ${WORK_DIR}/truncated.bin --out ${WORK_DIR}/truncated_out.bin ${sample}
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "truncated input should exit with status 2, got ${rc}")
endif()
file(SIZE ${WORK_DIR}/truncated_out.bin truncated_bytes)
if(NOT truncated_bytes EQUAL 64)
  message(FATAL_ERROR "expected exactly one report for the one complete command, got ${truncated_bytes} bytes")
endif()
