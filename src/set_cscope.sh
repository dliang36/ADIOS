#!/bin/bash

SRC_DIR="$PWD"
OUT_DIR="$PWD/.."

cd /
find $SRC_DIR -name '*.c' -o -name '*.h' > $OUT_DIR/cscope.files
cd $OUT_DIR
cscope -b
export CSCOPE_DB=$OUT_DIR/cscope.out
