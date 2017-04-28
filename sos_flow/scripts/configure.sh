#!/bin/bash 

if [ "x$SOS_ENV_SET" == "x" ] ; then
	echo "Please set up your SOS environment first (source hosts/<hostname>/setenv.sh)"
    kill -INT $$
fi

# remember where we are
STARTDIR=`pwd`
BUILD_ENVIRONMENT=`uname`

case "${BUILD_ENVIRONMENT}" in
    Linux)
        echo "Build environment detected: Linux"
        # Absolute path to this script, e.g. /home/user/bin/foo.sh
        SCRIPT=$(readlink -f "$0")
        ;;
    Darwin)
        echo "Build environment detected: Mac OS"
        # Absolute path to this script, e.g. /home/user/bin/foo.sh
        SCRIPT=$(readlink "$0")
        ;;
     *)
        # --- default case ---
        echo "WARNING: Unrecognized build environment.    (${BUILD_ENVIRONMENT})"
        ;;
esac

# Absolute path this script is in, thus /home/user/bin
SCRIPTPATH=$(dirname "$SCRIPT")
echo $SCRIPTPATH
BASEDIR=$SCRIPTPATH/..

echo "Building SOS $BUILDDIR from source in $BASEDIR"

# Parse the arguments
args=$(getopt -l "searchpath:" -o "cdht" -- "$@")
clean=0
debug=0
tau=0

eval set -- "${args}"

while [ $# -ge 1 ]; do
    case "$1" in
        --)
            # No more options left.
            break
            ;;
        -c)
            clean=1
            echo "doing clean configure"
            ;;
        -d)
            debug=1
            echo "doing debug configure"
            ;;
        -t)
            tau=1
            echo "doing TAU configure"
            ;;
        -h)
            echo "$0 [-c]"
            echo "-c : Clean"
            echo "-d : Debug"
            echo "-t : Use TAU"
            kill -INT $$
            ;;
        esac
    shift
done


host=`hostname`

if [ "x${SOS_ROOT}" == "x" ] ; then
    echo "Please set the SOS_ROOT environment variable."
    kill -INT $$
fi
if [ "x${SOS_WORK}" == "x" ] ; then
    echo "Please set the SOS_WORK environment variable."
    kill -INT $$
fi
if [ "x${SOS_CMD_PORT}" == "x" ] ; then
    echo "Please set the SOS_CMD_PORT environment variable."
    kill -INT $$
fi

if [ ${clean} -eq 1 ] ; then
    rm -rf $BUILDDIR
else
    if [ -d $BUILDDIR ] ; then
        echo "Warning: build directory exists. To start fresh, use the -c option."
        kill -INT $$
    fi
fi
mkdir -p $BUILDDIR
cd $BUILDDIR

buildtype=RelWithDebInfo
if [ ${debug} -eq 1 ] ; then
    buildtype=Debug
fi

tauopts=""
if [ ${tau} -eq 1 ] ; then
    tauopts="-DUSE_TAU=TRUE -DTAU_ROOT=$TAU_ROOT -DTAU_ARCH=$TAU_ARCH -DTAU_OPTIONS=$TAU_OPTIONS"
fi

cmd="cmake \
     -DCMAKE_BUILD_TYPE=${buildtype} \
     -DCMAKE_INSTALL_PREFIX=. \
     -DCMAKE_C_COMPILER=$CC \
     -DCMAKE_CXX_COMPILER=$CXX \
     -DMPI_C_COMPILER=$MPICC \
     -DMPI_CXX_COMPILER=$MPICXX \
     $cmake_extras \
     $BASEDIR"

     echo $cmd
     $cmd
