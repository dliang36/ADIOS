#!/bin/bash

#FLEXPATH_DIR=$HOME/lib

export CC=mpicc
export FC=mpif90
./configure --prefix=/usr/local/adios --with-mxml \
        --with-flexpath CFLAGS='-g -O0'
