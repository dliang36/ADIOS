#!/usr/bin/perl
# This script has been build to run the tests in the bin directory.
use Getopt::Std;
use strict;

##Parse Args
our($opt_v, $opt_r, $opt_w);

getopts('vrw');

my $writer_output = "1>/dev/null 2>&1";
my $reader_output = "1>/dev/null 2>&1";

$reader_output = "" if $opt_r == 1;
$writer_output = "" if $opt_w == 1;
if($opt_v == 1)
{
    $writer_output = "";
    $reader_output = "";
}

## Determine what tests we should run by looking in the current directory for a specific pattern
#my @files = `ls`;
#my @files = ("reader_global_range_select.c");
my @files = ("reader_maya_noxml.c");
my @groups = ();
foreach (@files)
{
    if(/reader_(.*)\./)
    {
        push @groups, $1;
    }
}

## Change the directory to the testing bin directory
chdir 'bin' or die "Cannot change directory to bin: $!";

foreach (@groups)
{
    my @num_procs = (4, 4, 5, 3, 3, 5, 7, 1, 1, 7);
    my $i, my $j;
    for($i = 0; $i < (@num_procs / 2); $i++)
    {
        #sleep 1;
        my $num_readers = $num_procs[$i * 2];
        my $num_writers = $num_procs[$i * 2 + 1];

        my $reader_prog = "reader_" . $_;
        my $writer_prog = "writer_" . $_;
        my $reader_return;
        my $writer_return;

        defined(my $test_pid = fork) or die "Cannot fork $!";
        unless($test_pid)
        {
            #Child process is here
            exec ("../run_test -nw $num_writers -nr $num_readers -w $writer_prog -r $reader_prog 2>&1 1>/dev/null"); #.  "$writer_output");
            die "Can't exec run_test! $!";
        }

        # This allows us to sidestep an issue with the filesystem being slow to copy metadata info
        # after creating a file...this isn't needed with a third party rendevous point
        #sleep 1;

        #defined(my $reader_pid = fork) or die "Cannot fork $!";
        #unless($reader_pid)
        #{
        #    #Child process is here
        #    exec ("mpirun -n $num_readers $reader_prog -t flx " . "$reader_output");
        #    die "Can't exec reader! $!";
        #}
        
        #for($j = 0; $j < 2; $j++)
        #{
        my $return_test = wait;
        #print "Returned PID: $returned_pid\n";
        
        if($return_test == $test_pid)
        {
            $return_test = $? >> 8;
            #kill 9, $writer_pid if $reader_return == 139;
            #print "Reader returned: $reader_return\n";
        }
        #elsif($returned_pid == $writer_pid)
        #{
        #    $writer_return = $? >> 8;
        #    kill 9, $reader_pid if $writer_return == 139;
        #    #print "Writer returned: $writer_return\n";
        #}
        else
        {
            print "Weird return PID: $return_test\n";
        }
        #}

        if($return_test == 0)
        {
            print "$_ with $num_readers readers and $num_writers writers: PASSED!\n";
        }
        else
        {
            print "$_ with $num_readers readers and $num_writers writers: FAILED!\n";
        }
    }
}

#print "@groups\n";

