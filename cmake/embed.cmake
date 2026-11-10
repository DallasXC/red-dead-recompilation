# embed_file(<input> <output.c> <symbol>): a C array of the file's bytes,
# <symbol>[] and <symbol>_size, regenerated when the input changes.
function(embed_file input output symbol)
    file(READ "${input}" hex HEX)
    string(LENGTH "${hex}" n)
    math(EXPR bytes "${n} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
    # Break the lines every 32 bytes so compilers stay happy.
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" arr "${arr}")
    file(WRITE "${output}"
        "/* Generated from ${input} by cmake/embed.cmake. */\n"
        "const unsigned char ${symbol}[] = {\n${arr}\n};\n"
        "const unsigned int ${symbol}_size = ${bytes};\n")
endfunction()
