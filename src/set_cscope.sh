#!/bin/bash

SRC_DIR="$PWD"
OUT_DIR="$PWD/.."

if [ -e $OUT_DIR/cscope.files ]
then
    echo "Deleting files..."
    rm $OUT_DIR/cscope.files $OUT_DIR/cscope.out
fi

cd /
find $SRC_DIR -name '*.c' -o -name '*.h' > $OUT_DIR/cscope.files
cd $OUT_DIR
cscope -b
export CSCOPE_DB=$OUT_DIR/cscope.out

cd $SRC_DIR
