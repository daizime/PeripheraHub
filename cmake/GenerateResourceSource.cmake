# rcc 6.7 writes absolute-path comments using the local Windows code page.
# Remove only full-line comments from generated C++; payloads remain unchanged.
foreach(required RCC INPUT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
set(raw "${OUTPUT}.raw")
execute_process(COMMAND "${RCC}" --name UiTheme --output "${raw}" "${INPUT}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "rcc failed: ${error}")
endif()
file(READ "${raw}" source)
string(REGEX REPLACE "(^|\n)([ \t]*)//[^\r\n]*" "\\1\\2// Embedded resource" source "${source}")
if(source MATCHES "[^\t\r\n -~]")
    message(FATAL_ERROR "Unexpected non-ASCII generated code after resource-comment normalization")
endif()
file(WRITE "${OUTPUT}" "${source}")
