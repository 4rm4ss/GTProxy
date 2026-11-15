# Copies the files of SRC into DST that DST does not have yet, keeping the
# ones already there (config/ next to the built proxy may have been edited).
# Usage: cmake -DSRC=<dir> -DDST=<dir> -P copy_missing.cmake
file(GLOB_RECURSE files RELATIVE "${SRC}" "${SRC}/*")
foreach (file ${files})
    if (NOT EXISTS "${DST}/${file}")
        get_filename_component(dir "${DST}/${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${dir}")
        file(COPY_FILE "${SRC}/${file}" "${DST}/${file}")
    endif ()
endforeach ()
