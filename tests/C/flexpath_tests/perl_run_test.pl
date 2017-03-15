#!/usr/bin/perl
# This script has been build to run the tests in the bin directory.

use strict;


my @files = `ls`;
my @groups = ();
foreach (@files)
{
    if(/reader_(.*)\./)
    {
        push @groups, $1;
    }
}

chdir 'bin' or die "Cannot change directory to bin: $!";

#foreach (@groups)
{
    $_ = $groups[0];
    my $reader_prog = "./reader_" . $_;
    my $writer_prog = "./writer_" . $_;

    open my $writer_output, '-|', "mpirun -n 2 $writer_prog -t flx 2>&1" or die "cannot pipe from writer_prog: $!";
    #system "mpirun -n 2 $writer_prog -t flx &";
    open my $reader_output, '-|', "mpirun -n 2 $reader_prog -t flx 2>&1" or die "cannot pipe from reader_prog: $!";

    my $reader_run = 1;
    my $writer_run = 1;
    while(1)
    {
        if($reader_run && defined(my $read = <$reader_output>))
        {
            #if($read =~ /TEST PASSED(.*)
            print "Reader: $read";
        }
        else
        {
            $reader_run = 0;
        }

        if($writer_run && defined(my $write = <$writer_output>))
        {
            print "Writer: $write";
        }
        else
        {
            $writer_run = 0;
        }

        if((!$reader_run) && (!$writer_run))
        {
            last;
        }
    }
}

#sleep 7;

print "Exiting\n";
#print "@groups\n";

