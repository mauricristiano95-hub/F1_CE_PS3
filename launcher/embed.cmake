# A file as a C array, for the launcher's fonts and logo (built into the exe):
#   cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
file(WRITE "${OUT}" "// generated from ${IN} (launcher/embed.cmake)\nstatic const unsigned char ${NAME}[] = {${body}};\nstatic const unsigned ${NAME}_size = ${size};\n")
